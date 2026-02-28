#include "bsp_lcd.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_log.h"

#include "bsp_pinmap.h"

static const char *TAG = "BSP_LCD";

// Force landscape orientation (logical resolution: CONFIG_LCD_V_RES x CONFIG_LCD_H_RES)
#define BSP_LCD_LANDSCAPE 1

// Provide defaults when sdkconfig does not define panel resolution
#ifndef CONFIG_LCD_H_RES
#define CONFIG_LCD_H_RES 320
#endif
#ifndef CONFIG_LCD_V_RES
#define CONFIG_LCD_V_RES 480
#endif
#ifndef CONFIG_LCD_PCLK_HZ
#define CONFIG_LCD_PCLK_HZ 10000000
#endif

static esp_lcd_i80_bus_handle_t s_i80_bus = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

static SemaphoreHandle_t s_trans_done_sem = NULL;
static volatile bool s_lvgl_flush_inflight = false;
static lv_disp_drv_t *s_lvgl_flush_drv = NULL;

// ST7796 command set (common ST77xx)
#define LCD_CMD_SWRESET 0x01
#define LCD_CMD_SLPOUT  0x11
#define LCD_CMD_DISPON  0x29
#define LCD_CMD_CASET   0x2A
#define LCD_CMD_RASET   0x2B
#define LCD_CMD_RAMWR   0x2C
#define LCD_CMD_MADCTL  0x36
#define LCD_CMD_COLMOD  0x3A

static int s_hor_res = CONFIG_LCD_H_RES;
static int s_ver_res = CONFIG_LCD_V_RES;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    BaseType_t hp_task_woken = pdFALSE;

    if (s_trans_done_sem) {
        xSemaphoreGiveFromISR(s_trans_done_sem, &hp_task_woken);
    }

    if (s_lvgl_flush_inflight && s_lvgl_flush_drv) {
        lv_disp_flush_ready(s_lvgl_flush_drv);
        s_lvgl_flush_inflight = false;
        s_lvgl_flush_drv = NULL;
    }

    return hp_task_woken == pdTRUE;
}

void bsp_lcd_backlight(bool on)
{
    if (BSP_LCD_BL_GPIO == GPIO_NUM_NC) {
        return;
    }
    gpio_set_level(BSP_LCD_BL_GPIO, on ? 1 : 0);
}

static esp_err_t bsp_lcd_bus_init(void)
{
    if (s_i80_bus && s_io) {
        return ESP_OK;
    }

    if (BSP_LCD_BL_GPIO != GPIO_NUM_NC) {
        gpio_config_t bk_cfg = {
            .pin_bit_mask = BIT64(BSP_LCD_BL_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&bk_cfg), TAG, "bk gpio_config");
        bsp_lcd_backlight(false);
    }

    const int data_gpios[8] = {
        BSP_LCD_D0_GPIO,
        BSP_LCD_D1_GPIO,
        BSP_LCD_D2_GPIO,
        BSP_LCD_D3_GPIO,
        BSP_LCD_D4_GPIO,
        BSP_LCD_D5_GPIO,
        BSP_LCD_D6_GPIO,
        BSP_LCD_D7_GPIO,
    };

    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = BSP_LCD_DC_GPIO,
        .wr_gpio_num = BSP_LCD_WR_GPIO,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            data_gpios[0], data_gpios[1], data_gpios[2], data_gpios[3],
            data_gpios[4], data_gpios[5], data_gpios[6], data_gpios[7],
        },
        .bus_width = 8,
        .max_transfer_bytes = CONFIG_LCD_H_RES * 40 * sizeof(uint16_t),
        .psram_trans_align = 64,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_config, &s_i80_bus), TAG, "esp_lcd_new_i80_bus");

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = BSP_LCD_CS_GPIO,
        .pclk_hz = CONFIG_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .flags = {
            .swap_color_bytes = 1, // LVGL uses little-endian lv_color_t; swap to RGB565 MSB first
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(s_i80_bus, &io_config, &s_io), TAG, "esp_lcd_new_panel_io_i80");
    return ESP_OK;
}

static esp_err_t st7796_init_sequence(void)
{
    ESP_RETURN_ON_FALSE(s_io != NULL, ESP_ERR_INVALID_STATE, TAG, "panel io not ready");

    // Software reset
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SWRESET, NULL, 0), TAG, "SWRESET");
    vTaskDelay(pdMS_TO_TICKS(120));

    // Sleep out
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SLPOUT, NULL, 0), TAG, "SLPOUT");
    vTaskDelay(pdMS_TO_TICKS(120));

    // Pixel format: 16-bit/pixel
    uint8_t colmod = 0x55;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_COLMOD, &colmod, 1), TAG, "COLMOD");
    vTaskDelay(pdMS_TO_TICKS(10));

    // Memory access control (orientation/color order)
    // MADCTL bits: MY(0x80) MX(0x40) MV(0x20) BGR(0x08)
#if BSP_LCD_LANDSCAPE
    // Landscape: swap X/Y via MV, keep BGR
    uint8_t madctl = 0x28;
    s_hor_res = CONFIG_LCD_V_RES;
    s_ver_res = CONFIG_LCD_H_RES;
#else
    // Portrait
    uint8_t madctl = 0x48;
    s_hor_res = CONFIG_LCD_H_RES;
    s_ver_res = CONFIG_LCD_V_RES;
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_MADCTL, &madctl, 1), TAG, "MADCTL");

    // Display on
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_DISPON, NULL, 0), TAG, "DISPON");
    vTaskDelay(pdMS_TO_TICKS(50));

    bsp_lcd_backlight(true);

    ESP_LOGI(TAG, "ST7796 ready (i80 8-bit, %dx%d, pclk=%d)", s_hor_res, s_ver_res, CONFIG_LCD_PCLK_HZ);
    return ESP_OK;
}

esp_err_t bsp_lcd_init(void)
{
    if (!s_trans_done_sem) {
        s_trans_done_sem = xSemaphoreCreateBinary();
        if (!s_trans_done_sem) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(bsp_lcd_bus_init(), TAG, "bus_init");
    ESP_RETURN_ON_ERROR(st7796_init_sequence(), TAG, "st7796_init");
    return ESP_OK;
}

void bsp_lcd_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    if (!s_io) {
        // If LCD isn't ready, unblock LVGL to avoid deadlock.
        lv_disp_flush_ready(drv);
        return;
    }

    const uint16_t x1 = (uint16_t)area->x1;
    const uint16_t y1 = (uint16_t)area->y1;
    const uint16_t x2 = (uint16_t)area->x2;
    const uint16_t y2 = (uint16_t)area->y2;

    // Set window (inclusive coordinates)
    uint8_t caset[4] = { (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF), (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF) };
    uint8_t raset[4] = { (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF), (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF) };
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_CASET, caset, sizeof(caset));
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_RASET, raset, sizeof(raset));

    const size_t w = (size_t)(x2 - x1 + 1);
    const size_t h = (size_t)(y2 - y1 + 1);
    const size_t bytes = w * h * sizeof(uint16_t);

    s_lvgl_flush_inflight = true;
    s_lvgl_flush_drv = drv;

    // Push color data (async). on_color_trans_done() will call lv_disp_flush_ready().
    esp_err_t err = esp_lcd_panel_io_tx_color(s_io, LCD_CMD_RAMWR, color_p, bytes);
    if (err != ESP_OK) {
        s_lvgl_flush_inflight = false;
        s_lvgl_flush_drv = NULL;
        lv_disp_flush_ready(drv);
    }
}

static esp_err_t draw_rect_blocking(int x1, int y1, int x2, int y2, const uint16_t *buf, size_t bytes)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t caset[4] = { (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF), (uint8_t)((x2 - 1) >> 8), (uint8_t)((x2 - 1) & 0xFF) };
    uint8_t raset[4] = { (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF), (uint8_t)((y2 - 1) >> 8), (uint8_t)((y2 - 1) & 0xFF) };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_CASET, caset, sizeof(caset)), TAG, "CASET");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_RASET, raset, sizeof(raset)), TAG, "RASET");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(s_io, LCD_CMD_RAMWR, buf, bytes), TAG, "RAMWR");
    if (s_trans_done_sem) {
        (void)xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(2000));
    }
    return ESP_OK;
}

esp_err_t bsp_lcd_fill(uint16_t rgb565)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }

    static uint16_t line[CONFIG_LCD_V_RES];
    for (int x = 0; x < s_hor_res; x++) {
        line[x] = rgb565;
    }

    for (int y = 0; y < s_ver_res; y++) {
        ESP_RETURN_ON_ERROR(draw_rect_blocking(0, y, s_hor_res, y + 1, line, (size_t)s_hor_res * sizeof(uint16_t)), TAG, "fill_line");
    }
    return ESP_OK;
}

esp_err_t bsp_lcd_test_pattern(void)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t colors[] = {
        0xF800, // Red
        0x07E0, // Green
        0x001F, // Blue
        0xFFFF, // White
        0x0000, // Black
    };
    const int bars = (int)(sizeof(colors) / sizeof(colors[0]));
    const int bar_w = s_hor_res / bars;

    static uint16_t line[CONFIG_LCD_V_RES];

    for (int b = 0; b < bars; b++) {
        const int x_start = b * bar_w;
        const int x_end = (b == bars - 1) ? s_hor_res : (b + 1) * bar_w;
        const int w = x_end - x_start;

        for (int x = 0; x < w; x++) {
            line[x] = colors[b];
        }

        const size_t bytes = (size_t)w * sizeof(uint16_t);
        for (int y = 0; y < s_ver_res; y++) {
            ESP_RETURN_ON_ERROR(draw_rect_blocking(x_start, y, x_end, y + 1, line, bytes), TAG, "bar_line");
        }
    }

    return ESP_OK;
}
