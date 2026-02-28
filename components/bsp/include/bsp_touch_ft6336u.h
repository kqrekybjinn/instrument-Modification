#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize FT6336U touch controller over I2C.
 *
 * @param x_max Maximum X coordinate in LVGL space (typically display hor_res)
 * @param y_max Maximum Y coordinate in LVGL space (typically display ver_res)
 */
esp_err_t bsp_touch_init(uint16_t x_max, uint16_t y_max);

/**
 * @brief LVGL input device read callback.
 */
void bsp_touch_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif
