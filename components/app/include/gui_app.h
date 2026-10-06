#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
 GUI_EVENT_STATE_TOGGLE = 0, GUI_EVENT_STOP, GUI_EVENT_NEW_RUN, GUI_EVENT_RETURN,
 GUI_EVENT_SET_SPEED, GUI_EVENT_MODE_SWITCH, GUI_EVENT_FORWARD_DIR_TOGGLE,
 GUI_EVENT_SET_COUNT, GUI_EVENT_COUNT_INC, GUI_EVENT_COUNT_DEC, GUI_EVENT_COUNT_STEP_TOGGLE
} gui_event_t;
typedef void (*gui_event_cb_t)(gui_event_t evt, int16_t value);
typedef struct {
 uint8_t phase;
 uint8_t mode;
 bool feedback_valid;
 bool origin_valid;
 bool reversing;
 bool forward_ccw;
 int16_t rpm;
 int16_t target;
 int16_t step;
 int64_t displacement;
 uint8_t motor_fault; int32_t motor_turns; uint16_t motor_angle;
} gui_view_t;
esp_err_t gui_app_init(SemaphoreHandle_t lock, gui_event_cb_t cb);
void gui_app_update_boot(uint8_t percent);
void gui_app_enter_main_ui(void);
void gui_app_render(const gui_view_t *view);
void gui_app_update_usb(bool mounted);
void gui_app_update_tf(bool mounted);
#ifdef __cplusplus
}
#endif
