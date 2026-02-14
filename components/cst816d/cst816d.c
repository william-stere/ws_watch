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
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "cst816d.h"

static const char *TAG = "cst816";

/* Registers */
#define CST816_REG_TOUCH_POINTS      0x02
#define CST816_REG_TOUCH_STATUS     0x03  //bit 4-7: touch valid, bit 0-3: touch points
#define CST816_REG_XPOS_H            0x03  //high 4 bits of X,bit 0-3
#define CST816_REG_XPOS_L            0x04  //low 8 bits of X
#define CST816_REG_YPOS_H            0x05  //high 4 bits
#define CST816_REG_YPOS_L            0x06  //low 8 bits
#define CST816_REG_CHIP_ID          0xA7   // chip ID register

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
    bool use_interrupt;                   /* Whether interrupt mode is enabled */
    // 数据缓存（用普通变量 + 临界区保护）
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


/*******************************************************************************
 * Callbacks for esp_lcd_touch
 ******************************************************************************/

static esp_err_t cst816_read_data(esp_lcd_touch_handle_t tp)
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
    esp_err_t ret = i2c_read_bytes(ctx->io, CST816_REG_XPOS_H, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    x = (buf[7] & 0x0F) << 8;

    esp_err_t ret = i2c_read_bytes(ctx->io, CST816_REG_XPOS_L, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    x = x | buf[7];

    //y
        esp_err_t ret = i2c_read_bytes(ctx->io, CST816_REG_YPOS_H, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    y = (buf[7] & 0x0F) << 8;

    esp_err_t ret = i2c_read_bytes(ctx->io, CST816_REG_YPOS_L, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    y = y | buf[7];

    /* 更新缓存（临界区保护） */
    portENTER_CRITICAL(&ctx->base.data.lock);
    ctx->touch_points = points;
    ctx->touch_x = x;
    ctx->touch_y = y;
    portEXIT_CRITICAL(&ctx->base.data.lock);

    return ESP_OK;
}

static bool cst816_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                          uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    cst816_t *ctx = __containerof(tp, cst816_t, base);

    if (max_point_num == 0 || point_num == NULL || x == NULL || y == NULL) {
        return false;
    }

    portENTER_CRITICAL(&tp->data.lock);
    if (*point_num > 0) {
        point_num = 1;
        x[0] = ctx->touch_x;
        y[0] = ctx->touch_y;
    }
    /* 清除缓存，表示数据已被取走 */
    ctx->touch_points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t cst816_del(esp_lcd_touch_handle_t tp)
{
    cst816_t *ctx = __containerof(tp, cst816_t, base);

    /* 中断模式下，删除任务并移除中断 */
    if (ctx->use_interrupt && ctx->irq_pin != GPIO_NUM_NC) {
        if (ctx->task_handle) {
            vTaskDelete(ctx->task_handle);
        }
        gpio_isr_handler_remove(ctx->irq_pin);
        gpio_reset_pin(ctx->irq_pin);
    }

    free(ctx);
    return ESP_OK;
}

/*******************************************************************************
 * Interrupt handling (only used in interrupt mode)
 ******************************************************************************/

static void IRAM_ATTR irq_handler(void *arg)
{
    cst816_t *ctx = (cst816_t *)arg;
    BaseType_t higher_prio_woken = pdFALSE;
    if (ctx->task_handle) {
        vTaskNotifyGiveFromISR(ctx->task_handle, &higher_prio_woken);
    }
    if (higher_prio_woken) {
        portYIELD_FROM_ISR();
    }
}

static void touch_task(void *arg)
{
    cst816_t *ctx = (cst816_t *)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  /* 等待中断通知 */
        update_touch_data(ctx);                    /* 更新数据 */
    }
}

/*******************************************************************************
 * Public API
 ******************************************************************************/

esp_err_t esp_lcd_touch_new_i2c_cst816(const esp_lcd_panel_io_handle_t io,
                                       const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *out_touch)
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
    ctx->use_interrupt = (ctx->irq_pin != GPIO_NUM_NC);

    /* 初始化基础结构 */
    ctx->base.io = io;
    memcpy(&ctx->base.config, config, sizeof(esp_lcd_touch_config_t));
    ctx->base.read_data = cst816_read_data;
    ctx->base.get_xy = cst816_get_xy;
    ctx->base.del = cst816_del;
    ctx->base.data.lock.owner = portMUX_FREE_VAL;

    /* 验证芯片 ID（可选，若不匹配则警告但不失败） */
    uint8_t chip_id = 0;
    ret = i2c_read_bytes(io, CST816_REG_CHIP_ID, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read chip ID, ret=%d", ret);
    } else if (chip_id != CST816_EXPECTED_ID) {
        ESP_LOGW(TAG, "Unexpected chip ID: 0x%02X (expected 0x%02X)", chip_id, CST816_EXPECTED_ID);
    } else {
        ESP_LOGI(TAG, "Chip ID verified: 0x%02X", chip_id);
    }

    /* 配置中断引脚（如果使用中断模式） */
    if (ctx->use_interrupt) {
        gpio_config_t irq_conf = {
            .pin_bit_mask = 1ULL << ctx->irq_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE   /* 下降沿触发 */
        };
        ESP_GOTO_ON_ERROR(gpio_config(&irq_conf), err, TAG, "GPIO config failed");

        /* 安装 ISR 服务（如果尚未安装） */
        static bool isr_service_installed = false;
        if (!isr_service_installed) {
            ESP_GOTO_ON_ERROR(gpio_install_isr_service(0), err, TAG,
                              "install isr service failed");
            isr_service_installed = true;
        }

        /* 创建后台任务 */
        BaseType_t task_ret = xTaskCreate(touch_task, "cst816_task", 4096, ctx,
                                          configMAX_PRIORITIES - 5, &ctx->task_handle);
        ESP_GOTO_ON_FALSE(task_ret == pdTRUE, ESP_FAIL, err, TAG,
                          "create touch task failed");

        /* 添加中断处理 */
        ESP_GOTO_ON_ERROR(gpio_isr_handler_add(ctx->irq_pin, irq_handler, ctx),
                          err, TAG, "add isr handler failed");

        ESP_LOGI(TAG, "Interrupt mode enabled on GPIO %d", ctx->irq_pin);
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

esp_err_t test(esp_lcd_touch_handle_t tp, uint8_t *finger_num)
{
    ESP_RETURN_ON_FALSE(tp && finger_num, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    cst816_t *ctx = __containerof(tp, cst816_t, base);
    uint8_t reg = CST816_REG_TOUCH_DATA;
    esp_err_t ret = i2c_read_bytes(ctx->io, reg, finger_num, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read touch data, ret=%d", ret);
    }
    return ret;
}