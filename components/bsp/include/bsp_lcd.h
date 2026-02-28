#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_lcd_init(void);

// LVGL display flush callback
void bsp_lcd_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);

// Simple bring-up helpers (blocking)
esp_err_t bsp_lcd_fill(uint16_t rgb565);
esp_err_t bsp_lcd_test_pattern(void);

void bsp_lcd_backlight(bool on);

#ifdef __cplusplus
}
#endif
