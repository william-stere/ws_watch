/*
 * SPDX-FileCopyrightText: 2025 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_lcd_touch.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new CST816D touch driver
 *
 * @note The I2C communication should be initialized before use this function.
 *
 * @param io LCD/Touch panel IO handle (must be I2C)
 * @param config Touch configuration (x_max, y_max, etc.)
 * @param out_touch Touch instance handle
 * @return
 *      - ESP_OK                    on success
 *      - ESP_ERR_NO_MEM             if out of memory
 *      - ESP_ERR_INVALID_ARG        if parameter is invalid
 *      - ESP_ERR_NOT_FOUND          if chip ID mismatch
 */
esp_err_t esp_lcd_touch_new_i2c_cst816d(const esp_lcd_panel_io_handle_t io,
                                       const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *out_touch,
                                       TaskHandle_t external_task_handle);


#ifdef __cplusplus
}
#endif