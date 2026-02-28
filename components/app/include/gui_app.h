#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GUI_EVENT_SPEED_STEP = 0,
    GUI_EVENT_STATE_TOGGLE,
} gui_event_t;

typedef void (*gui_event_cb_t)(gui_event_t evt);

esp_err_t gui_app_init(SemaphoreHandle_t lvgl_lock, gui_event_cb_t cb);

// Boot screen (shown at power-on)
void gui_app_update_boot(uint8_t percent);
void gui_app_enter_main_ui(void);

void gui_app_update_motor(bool running, bool reversing, int16_t rpm, bool can_reverse);
// Motor run time counter in centiseconds (1/100s).
// UI formats as mm:ss.cc.
void gui_app_update_run_time(uint32_t centiseconds);
void gui_app_update_usb(bool mounted);
void gui_app_update_tf(bool mounted);
void gui_app_sync_button(gui_event_t evt);

#ifdef __cplusplus
}
#endif
