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
#include "lv_demos.h"

#include "esp_lcd_nv3030b.h"
#include "cst816d.h"

#define LCD_HOST             SPI2_HOST
#define LCD_H_RES            (240)
#define LCD_V_RES            (284)
#define LCD_BIT_PER_PIXEL    (16)

#define PIN_NUM_LCD_CS       (GPIO_NUM_10)
#define PIN_NUM_LCD_PCLK     (GPIO_NUM_13)
#define PIN_NUM_LCD_MOSI     (GPIO_NUM_11)
#define PIN_NUM_LCD_DATA1    (GPIO_NUM_12)
#define PIN_NUM_LCD_DATA2    (GPIO_NUM_15)
#define PIN_NUM_LCD_DATA3    (GPIO_NUM_14)
#define PIN_NUM_LCD_RST      (GPIO_NUM_NC)
#define PIN_NUM_LCD_BL       (GPIO_NUM_NC)
#define LCD_PCLK_FREQ_HZ     (10 * 1000 * 1000)

#define TOUCH_INT            (GPIO_NUM_NC)

#define I2C0_SCL_PIN         (GPIO_NUM_6)
#define I2C0_SDA_PIN         (GPIO_NUM_5)

static esp_lcd_panel_io_handle_t lcd_io = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static esp_lcd_panel_io_handle_t tp_io_handle = NULL;
esp_lcd_touch_handle_t tp_handle = NULL;

static const char *TAG = "main";
static SemaphoreHandle_t refresh_finish = NULL;

IRAM_ATTR static bool test_notify_refresh_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;

    xSemaphoreGiveFromISR(refresh_finish, &need_yield);
    return (need_yield == pdTRUE);
}



esp_err_t lcd_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Initialize LCD");

    const spi_bus_config_t buscfg = NV3030B_PANEL_BUS_QSPI_CONFIG(
                                                                PIN_NUM_LCD_PCLK,
                                                                PIN_NUM_LCD_MOSI,
                                                                PIN_NUM_LCD_DATA1,
                                                                PIN_NUM_LCD_DATA2,
                                                                PIN_NUM_LCD_DATA3,
                                                                LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "initialize LCD bus failed");

    const esp_lcd_panel_io_spi_config_t io_cfg = NV3030B_PANEL_IO_QSPI_CONFIG(
                                                                PIN_NUM_LCD_CS,
                                                                NULL,
                                                                NULL);

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &lcd_io), TAG, "initialize LCD panel IO failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
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
        .buffer_size = 284 * 240,
        .double_buffer = false,
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
            .buff_dma = false,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    lvgl_port_flush_ready(disp);
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
    esp_lcd_touch_new_i2c_cst816d(tp_io_handle, &tp_cfg, &tp_handle, NULL);

    return ESP_OK;
}

static void test_draw_bitmap(esp_lcd_panel_handle_t panel_handle)
{
    refresh_finish = xSemaphoreCreateBinary();
    //TEST_ASSERT_NOT_NULL(refresh_finish);

    uint16_t row_line = LCD_V_RES / LCD_BIT_PER_PIXEL;
    uint8_t byte_per_pixel = LCD_BIT_PER_PIXEL / 8;
    uint8_t *color = (uint8_t *)heap_caps_calloc(1, row_line * LCD_H_RES * byte_per_pixel, MALLOC_CAP_DMA);
    //TEST_ASSERT_NOT_NULL(color);

    for (int j = 0; j < LCD_BIT_PER_PIXEL; j++) {
        for (int i = 0; i < row_line * LCD_H_RES; i++) {
            for (int k = 0; k < byte_per_pixel; k++) {
                color[i * byte_per_pixel + k] = (SPI_SWAP_DATA_TX(BIT(j), LCD_BIT_PER_PIXEL) >> (k * 8)) & 0xff;
            }
        }
        esp_lcd_panel_draw_bitmap(panel_handle, 0, j * row_line, LCD_H_RES, (j + 1) * row_line, color);
        xSemaphoreTake(refresh_finish, portMAX_DELAY);
    }
    free(color);
    vSemaphoreDelete(refresh_finish);
    //vTaskDelay(pdMS_TO_TICKS(3000));
}

void app_main(void)
{   
    ESP_ERROR_CHECK(lcd_init());
    lvgl_init();
    lv_demo_benchmark();
}
