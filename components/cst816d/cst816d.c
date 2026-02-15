#include <stdio.h>
/*
 * SPDX-FileCopyrightText: 2025 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "cst816d.h"

static const char *TAG = "cst816d";

/* Registers */

#define CST816_REG_GESTURE           0x01  /*Gesture ID register 0 
                    (0x00: no gesture, 0x01: 上划, 0x02: 下划, 0x03: 左划, 0x04: 右划, 0x05: 单击, 0x0C 长按）
                    不会清除状态，直到下次手势*/ 
#define CST816_REG_TOUCH_POINTS      0x02  //有没有触摸点；只有0和1
#define CST816_REG_TOUCH_STATUS      0x03  //bit 4-7: touch valid, bit 0-3: touch points；10有触摸，01无触摸；
#define CST816_REG_XPOS_H            0x03  //high 4 bits of X,bit 0-3；一般不用
#define CST816_REG_XPOS_L            0x04  //low 8 bits of X;一般只用低8位
#define CST816_REG_YPOS_H            0x05  //high 4 bits；一般不用
#define CST816_REG_YPOS_L            0x06  //low 8 bits； 大黑边是y=0；原点在屏幕左下脚（大黑边向下）
#define CST816_REG_CHIP_ID           0xA7   // chip ID register ; 0xB6
// A8 为0；A9 为2;AA 为6

/* Expected chip ID (may vary, adjust if needed) */
#define CST816_EXPECTED_ID          0xB5

/* Maximum number of touch points supported */
#define CST816_MAX_POINTS           1

/* Internal driver data structure */
typedef struct {
    esp_lcd_touch_t base;               /* Base touch structure */
    esp_lcd_panel_io_handle_t io;       /* I2C panel IO handle */
    TaskHandle_t task_handle;            /* Task handle for interrupt mode */
    gpio_num_t irq_pin;                   /* Interrupt pin, GPIO_NUM_NC if unused */ 
    TaskHandle_t external_task_handle;       /* External task handle for notifications */
    uint8_t touch_points;
    uint16_t touch_x;
    uint16_t touch_y;
} cst816_t;

static esp_err_t i2c_read_bytes(esp_lcd_panel_io_handle_t io, uint8_t reg,
                                 uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_rx_param(io, reg, data, len);
}

static esp_err_t i2c_write_byte(esp_lcd_panel_io_handle_t io, uint8_t reg,
                                 uint8_t value)
{
    return esp_lcd_panel_io_tx_param(io, reg, &value, 1);
}


static esp_err_t cst816d_read_data(esp_lcd_touch_handle_t tp)
{
    cst816_t *ctx = __containerof(tp, cst816_t, base);
    uint8_t buf[8];
    esp_err_t ret = i2c_read_bytes(ctx->io, CST816_REG_TOUCH_POINTS, buf, sizeof(buf));

    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t points = 0;
    uint16_t x = 0, y = 0;

    if (buf[0] & 0x01) {
        points = 1;
    }
    //x

    ret = i2c_read_bytes(ctx->io, CST816_REG_XPOS_L, (uint8_t*)&x, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }


    //y

    ret = i2c_read_bytes(ctx->io, CST816_REG_YPOS_H, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    y = (buf[0] & 0x0F) << 8;

    ret = i2c_read_bytes(ctx->io, CST816_REG_YPOS_L, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    y = y | buf[0];

    /* 更新缓存（临界区保护） */
    portENTER_CRITICAL(&ctx->base.data.lock);
    ctx->touch_points = points;
    ctx->touch_x = x;
    ctx->touch_y = y;
    portEXIT_CRITICAL(&ctx->base.data.lock);

    return ESP_OK;
}

static bool cst816d_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                          uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    if (point_num == NULL || x == NULL || y == NULL)
    {
        return false;
    }
    cst816_t *ctx = __containerof(tp, cst816_t, base);

    if (ctx->touch_points) {
        portENTER_CRITICAL(&tp->data.lock);
        x[0] = ctx->touch_x;
        y[0] = ctx->touch_y;
        *point_num = ctx->touch_points;

        ctx->touch_points = 0;
        portEXIT_CRITICAL(&tp->data.lock);
        return true;
    }

    return false;
}

static esp_err_t cst816d_del(esp_lcd_touch_handle_t tp)
{
    cst816_t *ctx = __containerof(tp, cst816_t, base);

    if(ctx->external_task_handle != NULL)
    {
        gpio_isr_handler_remove(ctx->irq_pin);
    }

    free(ctx);
    return ESP_OK;
}

static void IRAM_ATTR irq_handler(void *arg)
{
    cst816_t *ctx = (cst816_t *)arg;
    BaseType_t higher_prio_woken = pdFALSE;

    vTaskNotifyGiveFromISR(ctx->external_task_handle, &higher_prio_woken);
    if(higher_prio_woken){
        portYIELD_FROM_ISR(higher_prio_woken);
    }
}

/*******************************************************************************
 * Public API
 ******************************************************************************/

esp_err_t esp_lcd_touch_new_i2c_cst816d(const esp_lcd_panel_io_handle_t io,
                                       const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *out_touch,
                                        TaskHandle_t external_task_handle)
{
    esp_err_t ret = ESP_OK;
    cst816_t *ctx = NULL;

    ESP_GOTO_ON_FALSE(io && config && out_touch, ESP_ERR_INVALID_ARG, err, TAG,
                      "invalid argument");

    /* 分配内存 */
    ctx = calloc(1, sizeof(cst816_t));
    ESP_GOTO_ON_FALSE(ctx, ESP_ERR_NO_MEM, err, TAG, "no mem for cst816");

    /* 保存基本数据 */
    ctx->io = io;
    ctx->irq_pin = config->int_gpio_num;
    ctx->external_task_handle = external_task_handle;

    /* 初始化基础结构 */
    ctx->base.io = io;
    memcpy(&ctx->base.config, config, sizeof(esp_lcd_touch_config_t));
    ctx->base.read_data = cst816d_read_data;
    ctx->base.get_xy = cst816d_get_xy;
    ctx->base.del = cst816d_del;
    ctx->base.data.lock.owner = portMUX_FREE_VAL;

    /* 配置中断引脚（如果使用中断模式） */
    if (ctx->external_task_handle != NULL) {
        gpio_config_t irq_conf = {
            .pin_bit_mask = 1ULL << ctx->irq_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE   /* 下降沿触发 */
        };
        ESP_GOTO_ON_ERROR(gpio_config(&irq_conf), err, TAG, "GPIO config failed");
        //ESP_GOTO_ON_ERROR(gpio_install_isr_service(0), err, TAG, "install ISR failed");
        ESP_GOTO_ON_ERROR(gpio_isr_handler_add(ctx->irq_pin, irq_handler, ctx), err, TAG, "install ISR handler failed");
        
    } else {
        ESP_LOGI(TAG, "Polling mode enabled");
    }

    *out_touch = &ctx->base;
    return ESP_OK;

err:
    if (ctx) {
        free(ctx);
    }
    return ret;
}