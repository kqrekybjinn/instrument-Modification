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
    GUI_EVENT_MODE_SWITCH,
    GUI_EVENT_COUNT_INC,
    GUI_EVENT_COUNT_DEC,
    GUI_EVENT_COUNT_STEP_TOGGLE,
    GUI_EVENT_FORWARD_DIR_TOGGLE,
} gui_event_t;

typedef void (*gui_event_cb_t)(gui_event_t evt);

esp_err_t gui_app_init(SemaphoreHandle_t lvgl_lock, gui_event_cb_t cb);

// Boot screen (shown at power-on)
void gui_app_update_boot(uint8_t percent);
void gui_app_enter_main_ui(void);

void gui_app_update_motor(bool running, bool reversing, bool paused, int16_t rpm, bool can_reverse,
                          uint8_t mode, bool forward_ccw);
// Override the RPM label with actual measured speed (0.1 rpm/unit from 0x65 response).
// Call this periodically when running to show real-time actual speed.
void gui_app_update_actual_rpm(int16_t actual_rpm10);
// Motor run time counter in centiseconds (1/100s).
// UI formats as mm:ss.cc.
void gui_app_update_run_time(uint32_t centiseconds);
void gui_app_update_usb(bool mounted);
void gui_app_update_tf(bool mounted);
void gui_app_sync_button(gui_event_t evt);

// Count mode UI
void gui_app_set_mode(uint8_t mode);   // 0=timer, 1=count
void gui_app_update_rotation_count(int32_t current, int32_t target);
void gui_app_update_count_buttons(bool enabled);
void gui_app_update_count_step(int step);
void gui_app_update_state_button(const char *text, uint32_t color_hex);
void gui_app_update_forward_direction(bool forward_ccw);

#ifdef __cplusplus
}
#endif
