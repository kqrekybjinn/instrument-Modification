#include "app_control_service.hpp"

#include "freertos/task.h"

#include <limits.h>

#include "esp_log.h"
#include "esp_timer.h"

extern "C" {
#include "gui_app.h"
#include "motor_driver.h"
}

static const char *TAG = "APP";

void ControlService::init()
{
    state_ = control_state_t{};
    state_.running = false;
    state_.reversing = false;
    state_.target_rpm = DEFAULT_FORWARD_RPM;
    state_.run_start_us = 0;
    state_.last_forward_us = 0;
    state_.last_forward_rpm = 0;
    state_.reverse_target_us = 0;
    state_.sensor_value = 0;

    state_lock_ = xSemaphoreCreateMutex();
    control_q_ = xQueueCreate(16, sizeof(control_cmd_t));
}

void ControlService::start_task(int core_id)
{
    xTaskCreatePinnedToCore(task_trampoline, "control", 6144, this, 6, NULL, core_id);
}

void ControlService::push_cmd(control_cmd_id_t id)
{
    control_cmd_t cmd{};
    cmd.id = id;
    if (control_q_) {
        (void)xQueueSend(control_q_, &cmd, 0);
    }
}

control_state_t ControlService::snapshot()
{
    control_state_t copy;
    if (state_lock_ && xSemaphoreTake(state_lock_, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = state_;
        xSemaphoreGive(state_lock_);
    } else {
        copy = state_;
    }
    return copy;
}

void ControlService::set_sensor_value(int32_t value)
{
    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
        state_.sensor_value = value;
        xSemaphoreGive(state_lock_);
    }
}

void ControlService::apply_gui_refresh(const control_state_t *st)
{
    const bool can_reverse = (!st->running) && (st->last_forward_us > 0);
    gui_app_update_motor(st->running, st->reversing, st->target_rpm, can_reverse);
}

void ControlService::task_trampoline(void *arg)
{
    static_cast<ControlService *>(arg)->task_loop();
}

void ControlService::task_loop()
{
    ESP_LOGI(TAG, "Control task start");

    // Power-on settle time: show "初始化中" with progress bar for 5 seconds,
    // then send motor init commands and enter the normal UI.
    const int k_boot_ms = 5000;
    const int k_tick_ms = 100;
    for (int elapsed = 0; elapsed < k_boot_ms; elapsed += k_tick_ms) {
        uint8_t percent = (uint8_t)((elapsed * 100) / k_boot_ms);
        gui_app_update_boot(percent);
        vTaskDelay(pdMS_TO_TICKS(k_tick_ms));
    }
    gui_app_update_boot(100);

    ESP_ERROR_CHECK(motor_driver_init());
    ESP_ERROR_CHECK(motor_enable(MOTOR_DEFAULT_ID));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(motor_set_speed_mode(MOTOR_DEFAULT_ID));

    gui_app_enter_main_ui();
    gui_app_update_motor(false, false, DEFAULT_FORWARD_RPM, false);
    gui_app_update_run_time(0);

    // Keep the last displayed time when stopped; only reset to 0 after reverse finishes.
    int64_t held_display_us = 0;
    uint32_t last_display_cs = UINT32_MAX;
    bool last_running = false;

    while (1) {
        control_cmd_t cmd;
        const bool got = xQueueReceive(control_q_, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;

        control_state_t st = snapshot();
        const int64_t now_us = esp_timer_get_time();

        // ---- Time display update ----
        // Forward: counts up; Reverse: counts down; Stopped: holds last value.
        int64_t display_us = held_display_us;
        if (st.running && st.run_start_us > 0 && now_us >= st.run_start_us) {
            int64_t elapsed_us = now_us - st.run_start_us;
            if (elapsed_us < 0) elapsed_us = 0;

            if (st.reversing && st.reverse_target_us > 0) {
                int64_t remain_us = st.reverse_target_us - elapsed_us;
                if (remain_us < 0) remain_us = 0;
                display_us = remain_us;
            } else {
                display_us = elapsed_us;
            }
            held_display_us = display_us;
        }

        const uint32_t display_cs = (uint32_t)(display_us / 10000); // 1/100s
        if (display_cs != last_display_cs || st.running != last_running) {
            gui_app_update_run_time(display_cs);
            last_display_cs = display_cs;
            last_running = st.running;
        }

        // ---- Reverse auto-stop (return to origin) ----
        if (st.running && st.reversing && st.reverse_target_us > 0 &&
            (now_us - st.run_start_us) >= st.reverse_target_us) {
            ESP_LOGI(TAG, "Reverse window elapsed, braking");
            motor_brake(MOTOR_DEFAULT_ID);
            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.running = false;
                state_.reversing = false;
                state_.reverse_target_us = 0;
                state_.last_forward_us = 0;
                state_.last_forward_rpm = 0;
                xSemaphoreGive(state_lock_);
            }

            // Visually return to 0.
            held_display_us = 0;
            last_display_cs = UINT32_MAX;
            gui_app_update_run_time(0);

            st = snapshot();
            apply_gui_refresh(&st);
        }

        if (!got) {
            continue;
        }

        // ---- Commands ----
        switch (cmd.id) {
        case CMD_SPEED_STEP: {
            // Only allow speed change when stopped.
            if (st.running) {
                ESP_LOGW(TAG, "Speed step ignored: running");
                break;
            }

            int16_t next = st.target_rpm;
            if (next == SPEED_LEVEL_0_RPM) next = SPEED_LEVEL_1_RPM;
            else if (next == SPEED_LEVEL_1_RPM) next = SPEED_LEVEL_2_RPM;
            else next = SPEED_LEVEL_0_RPM;

            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.target_rpm = next;
                xSemaphoreGive(state_lock_);
            }
            st = snapshot();
            apply_gui_refresh(&st);
            ESP_LOGI(TAG, "Speed -> %d RPM", (int)st.target_rpm);
            break;
        }

        case CMD_STATE_TOGGLE: {
            if (st.running) {
                // Stop immediately.
                motor_brake(MOTOR_DEFAULT_ID);
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    if (state_.running && !state_.reversing) {
                        state_.last_forward_us = now_us - state_.run_start_us;
                        state_.last_forward_rpm = state_.target_rpm;
                    }
                    state_.running = false;
                    state_.reversing = false;
                    state_.reverse_target_us = 0;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
                apply_gui_refresh(&st);
                break;
            }

            // Stopped: start forward if no forward record, otherwise start reverse.
            const bool can_reverse = st.last_forward_us > 0;
            if (!can_reverse) {
                motor_drive_cmd_t drive{};
                drive.rpm10 = (int16_t)(st.target_rpm * 10);
                motor_drive_speed(MOTOR_DEFAULT_ID, drive);

                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.running = true;
                    state_.reversing = false;
                    state_.run_start_us = now_us;
                    state_.last_forward_us = 0;
                    state_.last_forward_rpm = 0;
                    state_.reverse_target_us = 0;
                    xSemaphoreGive(state_lock_);
                }

                held_display_us = 0;
                last_display_cs = UINT32_MAX;
                st = snapshot();
                apply_gui_refresh(&st);
                break;
            }

            // Start reverse: duration scales with speed so distance matches.
            int64_t forward_us = st.last_forward_us;
            int16_t forward_rpm = st.last_forward_rpm;
            if (forward_rpm <= 0) forward_rpm = st.target_rpm;
            int16_t reverse_rpm = st.target_rpm;
            if (reverse_rpm <= 0) reverse_rpm = SPEED_LEVEL_1_RPM;

            int64_t dur_us = 0;
            if (reverse_rpm > 0) {
                if (forward_us > (INT64_MAX / (int64_t)forward_rpm)) {
                    dur_us = INT64_MAX;
                } else {
                    dur_us = (forward_us * (int64_t)forward_rpm) / (int64_t)reverse_rpm;
                }
            }
            if (dur_us <= 0) {
                dur_us = REVERSE_FALLBACK_MS * 1000;
            }

            motor_drive_cmd_t drive{};
            drive.rpm10 = (int16_t)(-reverse_rpm * 10);
            motor_drive_speed(MOTOR_DEFAULT_ID, drive);

            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.running = true;
                state_.reversing = true;
                state_.run_start_us = now_us;
                state_.reverse_target_us = dur_us;
                xSemaphoreGive(state_lock_);
            }

            held_display_us = dur_us;
            last_display_cs = UINT32_MAX;
            st = snapshot();
            apply_gui_refresh(&st);
            break;
        }

        default:
            break;
        }
    }
}
