#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_io_spi.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "esp_lcd_nv3030b.h"
#include "cst816d.h"

#define LCD_HOST             SPI2_HOST
#define LCD_H_RES            (240)
#define LCD_V_RES            (300)
#define LCD_BIT_PER_PIXEL    (16)

#define PIN_NUM_LCD_CS       (GPIO_NUM_10)
#define PIN_NUM_LCD_SCLK     (GPIO_NUM_13)
#define PIN_NUM_LCD_MOSI     (GPIO_NUM_11)
#define PIN_NUM_LCD_DATA1    (GPIO_NUM_12)
#define PIN_NUM_LCD_DATA2    (GPIO_NUM_15)
#define PIN_NUM_LCD_DATA3    (GPIO_NUM_14)
#define PIN_NUM_LCD_RST      (GPIO_NUM_NC)
#define PIN_NUM_LCD_BL       (GPIO_NUM_NC)
#define LCD_SCLK_FREQ_HZ     (40 * 1000 * 1000)

#define TOUCH_INT            (GPIO_NUM_NC)

#define I2C0_SCL_PIN         (GPIO_NUM_6)
#define I2C0_SDA_PIN         (GPIO_NUM_5)

static esp_lcd_panel_io_handle_t lcd_io = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static esp_lcd_panel_io_handle_t tp_io_handle = NULL;

static const char *TAG = "main";

esp_err_t lcd_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Initialize LCD");

    const spi_bus_config_t buscfg = NV3030B_PANEL_BUS_QSPI_CONFIG(
                                                                PIN_NUM_LCD_SCLK,
                                                                PIN_NUM_LCD_MOSI,
                                                                PIN_NUM_LCD_DATA1,
                                                                PIN_NUM_LCD_DATA2,
                                                                PIN_NUM_LCD_DATA3,
                                                                LCD_SCLK_FREQ_HZ );
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "initialize LCD bus failed");

    const esp_lcd_panel_io_spi_config_t io_cfg = NV3030B_PANEL_IO_QSPI_CONFIG(
                                                                PIN_NUM_LCD_CS,
                                                                NULL,
                                                                NULL);

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &lcd_io), TAG, "initialize LCD panel IO failed");

    const nv3030b_vendor_config_t vendor_cfg = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = (void *) &vendor_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_nv3030b(lcd_io, &panel_config, &lcd_panel), TAG, "initialize LCD panel failed");
    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_disp_on_off(lcd_panel, true);


    return ESP_OK;
}

lv_display_t *lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 8196,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    lvgl_port_init(&lvgl_cfg);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = LCD_H_RES * 10,  // 80 行缓冲区
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    return disp;
}


esp_err_t iic_init()
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = 0,
        .scl_io_num = I2C0_SCL_PIN,
        .sda_io_num = I2C0_SDA_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus_handle), TAG, "initialize I2C bus failed");

    esp_lcd_panel_io_i2c_config_t iic_io_config = {
        .dev_addr = 0x15,
        .control_phase_bytes = 1,
        .scl_speed_hz = 100000,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .disable_control_phase = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus_handle, &iic_io_config, &tp_io_handle), TAG, "initialize touch panel IO failed");

    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(lcd_init());
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(iic_init());

    for(uint8_t addr = 1; addr < 127; addr++) {
        esp_err_t ret = i2c_master_probe(i2c_bus_handle, addr, pdMS_TO_TICKS(10));
        if(addr % 10 == 0)
        {
            ESP_LOGI(TAG, "Probing I2C address: 0x%02X", addr);
        }
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at address 0x%02X", addr);
        }
    }

    esp_lcd_touch_handle_t tp_handle = NULL;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .int_gpio_num = TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .interrupt_callback = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816(tp_io_handle, &tp_cfg, &tp_handle));

    while (1)
    {
        uint8_t finger = 0;
        esp_err_t ret = test(tp_handle, &finger);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "test: %d", finger);
        } else {
            ESP_LOGI(TAG, "failed");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    uint16_t green = 0x07E0;
    uint16_t white = 0xFFFF; 
    esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, LCD_H_RES, LCD_V_RES, &white);

    while(1)
    {
        esp_err_t ret = cst816d_get_xy(tp_handle);
        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "wait touch");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint16_t x, y;
        uint8_t touch_cnt = 0;
        esp_lcd_touch_point_data_t touch_points[1];
        esp_lcd_touch_get_data(tp_handle, touch_points, &touch_cnt, 1);   

        if(touch_cnt > 0)
        {
            x = touch_points[0].x;
            y = touch_points[0].y;
            ESP_LOGI(TAG, "Touch: X=%d, Y=%d", x, y);
            esp_lcd_panel_draw_bitmap(lcd_panel, x, y, x + 1, y + 1, &green);
        }
    }
}
