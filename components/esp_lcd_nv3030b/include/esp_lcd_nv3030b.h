/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD 面板初始化命令。
 *
 */
typedef struct {
    int cmd;                /*<! 特定的 LCD 命令 */
    const void *data;       /*<! 保存命令相关数据的缓冲区 */
    size_t data_bytes;      /*<! `data` 在内存中的大小，单位为字节 */
    unsigned int delay_ms;  /*<! 在此命令之后的延迟，单位为毫秒 */
} nv3030b_lcd_init_cmd_t;

/**
 * @brief LCD 面板供应商配置。
 *
 * @note  该结构可用于选择接口类型并覆盖默认初始化命令。
 * @note  该结构需要传递给 `esp_lcd_panel_dev_config_t` 中的 `vendor_config` 字段。
 *
 */
typedef struct {
    const nv3030b_lcd_init_cmd_t *init_cmds;    /*!< 指向初始化命令数组的指针。
                                                 *   该数组应声明为 `static const` 并放在函数外部。
                                                 *   参考源码中的 `vendor_specific_init_default`
                                                 */
    uint16_t init_cmds_size;    /*<! 上述数组中的命令数量 */
    struct {
        unsigned int use_qspi_interface: 1;     /*<! 使用 QSPI 接口时设为 1，默认使用 SPI 接口 */
    } flags;
} nv3030b_vendor_config_t;

/**
 * @brief 为 nv3030b 型号创建 LCD 面板
 *
 * @param[in]  io LCD 面板 IO 句柄
 * @param[in]  panel_dev_config 面板设备通用配置（使用 `vendor_config` 可选择 QSPI 接口或覆盖默认初始化命令）
 * @param[out] ret_panel 返回的 LCD 面板句柄
 * @return
 *      - ESP_OK: 成功
 *      - 其它: 失败
 */
esp_err_t esp_lcd_new_panel_nv3030b(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief LCD 面板总线配置结构
 *
 */
#define NV3030B_PANEL_BUS_SPI_CONFIG(sclk, mosi, max_trans_sz)  \
    {                                                           \
        .sclk_io_num = sclk,                                    \
        .mosi_io_num = mosi,                                    \
        .miso_io_num = -1,                                      \
        .quadhd_io_num = -1,                                    \
        .quadwp_io_num = -1,                                    \
        .max_transfer_sz = max_trans_sz,                        \
    }
#define NV3030B_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                           \
        .sclk_io_num = sclk,                                    \
        .data0_io_num = d0,                                     \
        .data1_io_num = d1,                                     \
        .data2_io_num = d2,                                     \
        .data3_io_num = d3,                                     \
        .max_transfer_sz = max_trans_sz,                        \
    }

/**
 * @brief LCD 面板 IO 配置结构
 *
 */
#define NV3030B_PANEL_IO_SPI_CONFIG(cs, dc, cb, cb_ctx)         \
    {                                                           \
        .cs_gpio_num = cs,                                      \
        .dc_gpio_num = dc,                                      \
        .spi_mode = 3,                                          \
        .pclk_hz = 60 * 1000 * 1000,                            \
        .trans_queue_depth = 10,                                \
        .on_color_trans_done = cb,                              \
        .user_ctx = cb_ctx,                                     \
        .lcd_cmd_bits = 8,                                      \
        .lcd_param_bits = 8,                                    \
    }
#define NV3030B_PANEL_IO_QSPI_CONFIG(cs, cb, cb_ctx)            \
    {                                                           \
        .cs_gpio_num = cs,                                      \
        .dc_gpio_num = -1,                                      \
        .spi_mode = 3,                                          \
        .pclk_hz = 20 * 1000 * 1000,                            \
        .trans_queue_depth = 10,                                \
        .on_color_trans_done = cb,                              \
        .user_ctx = cb_ctx,                                     \
        .lcd_cmd_bits = 32,                                     \
        .lcd_param_bits = 8,                                    \
        .flags = {                                              \
            .quad_mode = true,                                  \
        },                                                      \
    }

#ifdef __cplusplus
}
#endif
