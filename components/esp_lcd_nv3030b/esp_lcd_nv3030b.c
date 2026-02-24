/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_log.h"

#include "esp_lcd_nv3030b.h"

#define LCD_OPCODE_WRITE_CMD        (0x02ULL)
#define LCD_OPCODE_READ_CMD         (0x0BULL)
#define LCD_OPCODE_WRITE_COLOR      (0x32ULL)

static const char *TAG = "nv3030b";

static esp_err_t panel_nv3030b_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3030b_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3030b_init(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3030b_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_nv3030b_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_nv3030b_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_nv3030b_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_nv3030b_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_nv3030b_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // 保存 LCD_CMD_MADCTL 寄存器的当前值
    uint8_t colmod_val; // 保存 LCD_CMD_COLMOD 寄存器的当前值
    const nv3030b_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int reset_level: 1;
    } flags;
} nv3030b_panel_t;

esp_err_t esp_lcd_new_panel_nv3030b(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config, 
                                     esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    esp_err_t ret = ESP_OK;
    nv3030b_panel_t *nv3030b = NULL;
    nv3030b = calloc(1, sizeof(nv3030b_panel_t));
    ESP_GOTO_ON_FALSE(nv3030b, ESP_ERR_NO_MEM, err, TAG, "no mem for nv3030b panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        nv3030b->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        nv3030b->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color element order");
        break;
    }

    uint8_t fb_bits_per_pixel = 0;
    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        nv3030b->colmod_val = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        nv3030b->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    nv3030b->io = io;
    nv3030b->reset_gpio_num = panel_dev_config->reset_gpio_num;
    nv3030b->fb_bits_per_pixel = fb_bits_per_pixel;
    nv3030b_vendor_config_t *vendor_config = (nv3030b_vendor_config_t *)panel_dev_config->vendor_config;
    if (vendor_config) {
        nv3030b->init_cmds = vendor_config->init_cmds;
        nv3030b->init_cmds_size = vendor_config->init_cmds_size;
    }
    nv3030b->flags.reset_level = panel_dev_config->flags.reset_active_high;
    nv3030b->base.del = panel_nv3030b_del;
    nv3030b->base.reset = panel_nv3030b_reset;
    nv3030b->base.init = panel_nv3030b_init;
    nv3030b->base.draw_bitmap = panel_nv3030b_draw_bitmap;
    nv3030b->base.invert_color = panel_nv3030b_invert_color;
    nv3030b->base.set_gap = panel_nv3030b_set_gap;
    nv3030b->base.mirror = panel_nv3030b_mirror;
    nv3030b->base.swap_xy = panel_nv3030b_swap_xy;
    nv3030b->base.disp_on_off = panel_nv3030b_disp_on_off;
    *ret_panel = &(nv3030b->base);
    ESP_LOGD(TAG, "new nv3030b panel @%p", nv3030b);

    ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_NV3030B_VER_MAJOR, ESP_LCD_NV3030B_VER_MINOR,
             ESP_LCD_NV3030B_VER_PATCH);

    return ESP_OK;

err:
    if (nv3030b) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(nv3030b);
    }
    return ret;
}

static esp_err_t tx_param(nv3030b_panel_t *nv3030b, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    return esp_lcd_panel_io_tx_param(io, lcd_cmd, param, param_size);
}

static esp_err_t tx_color(nv3030b_panel_t *nv3030b, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_COLOR << 24;
    return esp_lcd_panel_io_tx_color(io, lcd_cmd, param, param_size);
}

static esp_err_t panel_nv3030b_del(esp_lcd_panel_t *panel)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);

    if (nv3030b->reset_gpio_num >= 0) {
        gpio_reset_pin(nv3030b->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del nv3030b panel @%p", nv3030b);
    free(nv3030b);
    return ESP_OK;
}

static esp_err_t panel_nv3030b_reset(esp_lcd_panel_t *panel)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3030b->io;

    // 执行硬件复位
    if (nv3030b->reset_gpio_num >= 0) {
        gpio_set_level(nv3030b->reset_gpio_num, nv3030b->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(nv3030b->reset_gpio_num, !nv3030b->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else { // 执行软件复位
        ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

static const nv3030b_lcd_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { 数据 }, data_size, delay_ms}
    
    //{0xFF, (uint8_t []){0x20, 0x10, 0x10}, 3, 0},
    //{0x0C, (uint8_t []){0x11}, 1, 0},
    //{0x10, (uint8_t []){0x02}, 1, 0},
    //{0x11, (uint8_t []){0x11}, 1, 0},
    //{0x15, (uint8_t []){0x42}, 1, 0},
    //{0x16, (uint8_t []){0x11}, 1, 0},
    //{0x1A, (uint8_t []){0x02}, 1, 0},
    //{0x11, (uint8_t []){}, 0, 10}, //何意未
    
    {0xFD, (uint8_t []){0x06, 0x08}, 2, 0},
    {0x61, (uint8_t []){0x07, 0x04}, 2, 0},
    {0x62, (uint8_t []){0x00, 0x44, 0x45}, 3, 0},
    {0x63, (uint8_t []){0x41, 0x07, 0x12, 0x12}, 4, 0},
    {0x64, (uint8_t []){0x37}, 1, 0},
    {0x65, (uint8_t []){0x09, 0x10, 0x21}, 3, 0},//vsp
    {0x66, (uint8_t []){0x09, 0x10, 0x21}, 3, 0},//vsn
    {0x67, (uint8_t []){0x20, 0x40}, 2, 0},//add source_neg_time
    {0x68, (uint8_t []){0x90, 0x4c, 0x7C, 0x66}, 4, 0},//gamma vap/van
    {0xB1, (uint8_t []){0x0F, 0x02, 0x01}, 3, 0},
    {0xB4, (uint8_t []){0x01}, 1, 0},
    {0xB5, (uint8_t []){0x02, 0x02, 0x0a, 0x14}, 4, 0},//porch
    {0xB6, (uint8_t []){0x04, 0x01, 0x9f, 0x00, 0x02}, 5, 0},
    {0xDF, (uint8_t []){0x11}, 1, 0},//gamma sel
    {0xE2, (uint8_t []){0x13, 0x00, 0x00, 0x30, 0x33, 0x3f}, 6, 0},
    {0xE5, (uint8_t []){0x3f, 0x33, 0x30, 0x00, 0x00, 0x13}, 6, 0},
    {0xE1, (uint8_t []){0x00, 0x57}, 2, 0},
    {0xE4, (uint8_t []){0x58, 0x00}, 2, 0},
    {0xE0, (uint8_t []){0x01, 0x03, 0x0e, 0x0e, 0x0c, 0x15, 0x19}, 7, 0},
    {0xE3, (uint8_t []){0x1a, 0x16, 0x0C, 0x0f, 0x0e, 0x0d, 0x02, 0x01}, 8, 0},
    {0xE6, (uint8_t []){0x00, 0xff}, 2, 0},
    {0xE7, (uint8_t []){0x01, 0x04, 0x03, 0x03, 0x00, 0x12}, 6, 0},
    {0xE8, (uint8_t []){0x00, 0x70, 0x00}, 3, 0},
    {0xEC, (uint8_t []){0x52}, 1, 0},
    {0xF1, (uint8_t []){0x01, 0x01, 0x02}, 3, 0},
    {0xF6, (uint8_t []){0x09, 0x10, 0x00, 0x00}, 4, 0},
    {0xFD, (uint8_t []){0xfa, 0xfc}, 2, 0},
    //{0x3A, (uint8_t []){0x65}, 1, 0},
    //{0x36, (uint8_t []){0x00}, 1, 0},//mark 8 1000;0 0000
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 1},

    //{0x3A, (uint8_t []){0x55}, 1, 0}, 
    //{0x36, (uint8_t []){0x00}, 1, 0}, //何意未

    {0x11, (uint8_t []){}, 0, 100},
    {0x29, (uint8_t []){}, 0, 0},
    

};

static esp_err_t panel_nv3030b_init(esp_lcd_panel_t *panel)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3030b->io;
    const nv3030b_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, LCD_CMD_MADCTL, 
                                (uint8_t[]) {
                                    nv3030b->madctl_val,
                                    }, 
    1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, LCD_CMD_COLMOD, 
                                (uint8_t[]) {
                                    nv3030b->colmod_val,
                                    }, 
    1), TAG, "send command failed");

    // 厂商特定的初始化，可能因制造商而异
    // 应参考 LCD 供应商提供的初始化序列
    if (nv3030b->init_cmds) {
        init_cmds = nv3030b->init_cmds;
        init_cmds_size = nv3030b->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(nv3030b_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        if (init_cmds[i].data_bytes > 0) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                nv3030b->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                nv3030b->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                         init_cmds[i].cmd);
            }
        }

        // 发送命令
        ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG,
                            "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));

    }
    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_nv3030b_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = nv3030b->io;

    x_start += nv3030b->x_gap;
    x_end += nv3030b->x_gap;
    y_start += nv3030b->y_gap;
    y_end += nv3030b->y_gap;

    // 定义 MCU 可访问的帧内存区域
    ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    // 传输帧缓冲区
    size_t len = (x_end - x_start) * (y_end - y_start) * nv3030b->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(tx_color(nv3030b, io, LCD_CMD_RAMWR, color_data, len), TAG, "send color failed");

    return ESP_OK;
}

static esp_err_t panel_nv3030b_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3030b->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_nv3030b_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3030b->io;
    if (mirror_x) {
        nv3030b->madctl_val |= BIT(1);
    } else {
        nv3030b->madctl_val &= ~BIT(1);
    }
    if (mirror_y) {
        nv3030b->madctl_val |= BIT(0);
    } else {
        nv3030b->madctl_val &= ~BIT(0);
    }
    ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3030b->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_nv3030b_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3030b->io;
    if (swap_axes) {
        nv3030b->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        nv3030b->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(
        tx_param(nv3030b, io, LCD_CMD_MADCTL, (uint8_t[]) {nv3030b->madctl_val}, 1),
        TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_nv3030b_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    nv3030b->x_gap = x_gap;
    nv3030b->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_nv3030b_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3030b->io;
    int command = 0;

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(tx_param(nv3030b, io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
