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

// Returns the RPM for the current phase of a smooth trapezoidal profile.
// progress: rotations completed since phase start (0-based)
// total:    total rotations for this phase
// target_rpm: cruise speed; SMOOTH_SLOW_RPM is used for ramp phases
// When total <= 2*SMOOTH_RAMP_COUNT, ramp=0 and entire run uses SMOOTH_SLOW_RPM.
static int16_t smooth_phase_rpm(int32_t progress, int32_t total, int16_t target_rpm)
{
    if (total <= 0) return SMOOTH_SLOW_RPM;
    const int32_t ramp = ((int32_t)SMOOTH_RAMP_COUNT * 2 < total) ? SMOOTH_RAMP_COUNT : 0;
    if (ramp == 0 || progress < ramp || progress >= total - ramp) {
        return SMOOTH_SLOW_RPM;
    }
    return target_rpm;
}

static int16_t motion_cmd_rpm10(const control_state_t &st, bool reversing, int16_t rpm_abs)
{
    int32_t sign = st.forward_ccw ? -1 : 1;
    if (reversing) {
        sign = -sign;
    }
    return (int16_t)(sign * rpm_abs * 10);
}


void ControlService::init()
{
    state_ = control_state_t{};
    state_.running = false;
    state_.reversing = false;
    state_.paused = false;
    state_.forward_ccw = false;
    state_.target_rpm = DEFAULT_FORWARD_RPM;
    state_.run_start_us = 0;
    state_.last_forward_us = 0;
    state_.last_forward_rpm = 0;
    state_.reverse_target_us = 0;
    state_.sensor_value = 0;
    state_.mode = MODE_TIMER;
    state_.target_count = DEFAULT_TARGET_COUNT;
    state_.mileage_start = -1;
    state_.mileage_now = 0;
    state_.actual_rpm = 0;
    state_.count_step = 10;

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
    gui_app_update_motor(st->running, st->reversing, st->paused, st->target_rpm, can_reverse,
                         st->mode == MODE_COUNT ? 1 : 0, st->forward_ccw);
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
    gui_app_update_motor(false, false, false, DEFAULT_FORWARD_RPM, false, 0, false);
    gui_app_update_run_time(0);
    gui_app_set_mode(0);             // starts in timer mode
    gui_app_update_rotation_count(0, 0);  // timer mode: just show "0"
    gui_app_update_count_buttons(true);
    gui_app_update_count_step(10);
    gui_app_update_forward_direction(false);

    // Keep the last displayed time when stopped; only reset to 0 after reverse finishes.
    int64_t held_display_us = 0;
    uint32_t last_display_cs = UINT32_MAX;
    bool last_running = false;
    int32_t last_count_display = -1;
    // Tracks the last RPM10 command sent in count mode.
    int16_t count_phase_rpm10 = 0;

    while (1) {
        control_cmd_t cmd;
        const bool got = xQueueReceive(control_q_, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;

        control_state_t st = snapshot();
        const int64_t now_us = esp_timer_get_time();

        // ---- Timer mode: periodic drive keepalive + actual speed feedback (0x64 → 0x65) ----
        if (st.mode == MODE_TIMER && st.running) {
            motor_drive_cmd_t drive{};
            drive.rpm10 = motion_cmd_rpm10(st, st.reversing, st.target_rpm);
            motor_drive_resp_t dresp{};
            if (motor_drive_speed_fb(MOTOR_DEFAULT_ID, drive, &dresp) == ESP_OK) {
                int16_t actual = (int16_t)((dresp.actual_rpm10 < 0 ? -dresp.actual_rpm10 : dresp.actual_rpm10) / 10);
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.actual_rpm = actual;
                    xSemaphoreGive(state_lock_);
                }
                gui_app_update_actual_rpm(dresp.actual_rpm10);
            }
        }

        // ---- Timer mode: mileage polling for real-time rotation display (0x74 → 0x75) ----
        if (st.mode == MODE_TIMER && st.running && st.mileage_start >= 0) {
            motor_mileage_resp_t mresp{};
            if (motor_query_mileage(MOTOR_DEFAULT_ID, &mresp) == ESP_OK) {
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.mileage_now = mresp.mileage_rotations;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
                int32_t delta = st.mileage_now - st.mileage_start;
                int32_t display_rot = delta < 0 ? -delta : delta;
                gui_app_update_rotation_count(display_rot, 0);
            }
        }

        // ---- Count mode: mileage feedback polling (0x74 → 0x75) ----
        if (st.mode == MODE_COUNT && st.running) {
            motor_mileage_resp_t mresp{};
            if (motor_query_mileage(MOTOR_DEFAULT_ID, &mresp) == ESP_OK) {
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.mileage_now = mresp.mileage_rotations;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
            }

            // delta: rotations completed relative to run start
            int32_t delta = st.mileage_now - st.mileage_start;
            int32_t cur_rot = delta < 0 ? 0 : (delta > (int32_t)st.target_count ? (int32_t)st.target_count : delta);

            if (cur_rot != last_count_display) {
                gui_app_update_rotation_count(cur_rot, st.target_count);
                last_count_display = cur_rot;
            }

            if (!st.reversing) {
                // Smooth profile: adjust speed based on rotation progress
                int16_t desired_fwd = smooth_phase_rpm(delta, st.target_count, st.target_rpm);
                int16_t desired_fwd_rpm10 = motion_cmd_rpm10(st, false, desired_fwd);
                if (desired_fwd_rpm10 != count_phase_rpm10) {
                    motor_drive_cmd_t d{};
                    d.rpm10 = desired_fwd_rpm10;
                    motor_drive_speed(MOTOR_DEFAULT_ID, d);
                    count_phase_rpm10 = desired_fwd_rpm10;
                }

                // Forward: auto-stop when mileage delta reaches target
                if (delta >= (int32_t)st.target_count) {
                    ESP_LOGI(TAG, "Count mode: target reached (delta=%ld)", (long)delta);
                    motor_brake(MOTOR_DEFAULT_ID);
                    count_phase_rpm10 = 0;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = false;
                        state_.paused = false;
                        state_.run_start_us = 0;
                        xSemaphoreGive(state_lock_);
                    }
                    st = snapshot();
                    gui_app_update_rotation_count(st.target_count, st.target_count);
                    last_count_display = st.target_count;
                    gui_app_update_state_button("Start", 0x22c55e);
                    gui_app_update_count_buttons(false);
                    gui_app_update_motor(false, false, false, st.target_rpm, false, 1, st.forward_ccw);
                }
            } else {
                // Smooth profile: adjust speed based on reverse rotation progress
                // rev_progress: 0 at reverse-start, increases as motor returns to origin
                int32_t rev_progress = (int32_t)st.target_count - delta;
                if (rev_progress < 0) rev_progress = 0;
                int16_t desired_rev_abs = smooth_phase_rpm(rev_progress, st.target_count, st.target_rpm);
                int16_t desired_rev_rpm10 = motion_cmd_rpm10(st, true, desired_rev_abs);
                if (desired_rev_rpm10 != count_phase_rpm10) {
                    motor_drive_cmd_t d{};
                    d.rpm10 = desired_rev_rpm10;
                    motor_drive_speed(MOTOR_DEFAULT_ID, d);
                    count_phase_rpm10 = desired_rev_rpm10;
                }

                // Reverse: auto-stop when mileage returns to start (back at origin)
                if (st.mileage_now <= st.mileage_start) {
                    ESP_LOGI(TAG, "Count mode: reverse complete, at origin");
                    motor_brake(MOTOR_DEFAULT_ID);
                    count_phase_rpm10 = 0;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = false;
                        state_.reversing = false;
                        state_.paused = false;
                        state_.mileage_start = -1;
                        state_.mileage_now = 0;
                        state_.run_start_us = 0;
                        xSemaphoreGive(state_lock_);
                    }
                    st = snapshot();
                    gui_app_update_rotation_count(0, st.target_count);
                    last_count_display = 0;
                    gui_app_update_count_buttons(true);
                    apply_gui_refresh(&st);
                    gui_app_update_state_button("Start", 0x22c55e);
                    gui_app_set_mode(1);
                }
            }
        }

        // ---- Timer mode: time display update ----
        if (st.mode == MODE_TIMER) {
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
        }

        // ---- Timer mode: reverse auto-stop (return to origin) ----
        if (st.mode == MODE_TIMER && st.running && st.reversing && st.reverse_target_us > 0 &&
            (now_us - st.run_start_us) >= st.reverse_target_us) {
            ESP_LOGI(TAG, "Reverse window elapsed, braking");
            motor_brake(MOTOR_DEFAULT_ID);
            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.running = false;
                state_.reversing = false;
                state_.paused = false;
                state_.reverse_target_us = 0;
                state_.last_forward_us = 0;
                state_.last_forward_rpm = 0;
                state_.mileage_start = -1;
                state_.mileage_now = 0;
                xSemaphoreGive(state_lock_);
            }

            st = snapshot();
            held_display_us = 0;
            last_display_cs = UINT32_MAX;
            gui_app_update_run_time(0);
            gui_app_update_rotation_count(0, 0);
            apply_gui_refresh(&st);
            gui_app_update_state_button("Start", 0x22c55e);
            gui_app_set_mode(0);
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
            if      (next == SPEED_LEVEL_0_RPM) next = SPEED_LEVEL_1_RPM;
            else if (next == SPEED_LEVEL_1_RPM) next = SPEED_LEVEL_2_RPM;
            else if (next == SPEED_LEVEL_2_RPM) next = SPEED_LEVEL_3_RPM;
            else if (next == SPEED_LEVEL_3_RPM) next = SPEED_LEVEL_4_RPM;
            else                                 next = SPEED_LEVEL_0_RPM;

            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.target_rpm = next;
                xSemaphoreGive(state_lock_);
            }
            st = snapshot();
            apply_gui_refresh(&st);
            ESP_LOGI(TAG, "Speed -> %d RPM", (int)st.target_rpm);
            break;
        }

        case CMD_MODE_SWITCH: {
            // Only allow mode switch when fully idle (no run in progress).
            if (st.running || st.mileage_start >= 0 || st.last_forward_us > 0) {
                ESP_LOGW(TAG, "Mode switch ignored: not idle");
                break;
            }

            operating_mode_t new_mode = (st.mode == MODE_TIMER) ? MODE_COUNT : MODE_TIMER;
            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.mode = new_mode;
                state_.paused = false;
                state_.mileage_start = -1;
                state_.mileage_now = 0;
                xSemaphoreGive(state_lock_);
            }
            st = snapshot();

            gui_app_set_mode(new_mode == MODE_TIMER ? 0 : 1);
            if (new_mode == MODE_COUNT) {
                gui_app_update_rotation_count(0, st.target_count);
                gui_app_update_count_buttons(true);
                last_count_display = 0;
            } else {
                held_display_us = 0;
                last_display_cs = UINT32_MAX;
                gui_app_update_run_time(0);
                gui_app_update_rotation_count(0, 0);
            }
            gui_app_update_state_button("Start", 0x22c55e);
            apply_gui_refresh(&st);
            ESP_LOGI(TAG, "Mode -> %s", new_mode == MODE_TIMER ? "TIMER" : "COUNT");
            break;
        }

        case CMD_COUNT_INC: {
            // Only adjust when in count mode, stopped, and at origin
            if (st.mode != MODE_COUNT || st.running || st.mileage_start >= 0) {
                break;
            }
            int16_t next = st.target_count + st.count_step;
            if (next > MAX_TARGET_COUNT) next = MAX_TARGET_COUNT;
            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.target_count = next;
                xSemaphoreGive(state_lock_);
            }
            gui_app_update_rotation_count(0, next);
            last_count_display = 0;
            ESP_LOGI(TAG, "Target count -> %d", (int)next);
            break;
        }

        case CMD_COUNT_DEC: {
            if (st.mode != MODE_COUNT || st.running || st.mileage_start >= 0) {
                break;
            }
            int16_t next = st.target_count - st.count_step;
            if (next < MIN_TARGET_COUNT) next = MIN_TARGET_COUNT;
            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.target_count = next;
                xSemaphoreGive(state_lock_);
            }
            gui_app_update_rotation_count(0, next);
            last_count_display = 0;
            ESP_LOGI(TAG, "Target count -> %d", (int)next);
            break;
        }

        case CMD_STATE_TOGGLE: {
            if (st.mode == MODE_COUNT) {
                // ---- Count mode A key / UI state key: pause/resume/start-forward ----
                const int32_t delta = st.mileage_now - st.mileage_start;

                if (st.running) {
                    // Running (forward/reverse) -> pause
                    motor_brake(MOTOR_DEFAULT_ID);
                    count_phase_rpm10 = 0;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = false;
                        state_.paused = true;
                        state_.run_start_us = 0;
                        xSemaphoreGive(state_lock_);
                    }
                    st = snapshot();
                    int32_t cur = delta < 0 ? 0 : (delta > st.target_count ? st.target_count : delta);
                    gui_app_update_rotation_count(cur, st.target_count);
                    last_count_display = cur;
                    gui_app_update_state_button("Resume", 0x22c55e);
                    gui_app_update_count_buttons(false);
                    apply_gui_refresh(&st);
                    break;
                }

                if (st.paused) {
                    // Paused -> resume same direction
                    if (!st.reversing && delta >= (int32_t)st.target_count) {
                        ESP_LOGW(TAG, "Count mode: target reached, use stop/reverse");
                        break;
                    }

                    int32_t progress = delta;
                    if (progress < 0) progress = 0;
                    if (st.reversing) {
                        progress = (int32_t)st.target_count - delta;
                        if (progress < 0) progress = 0;
                    }
                    int16_t resume_rpm = smooth_phase_rpm(progress, st.target_count, st.target_rpm);
                    motor_drive_cmd_t drive{};
                    drive.rpm10 = motion_cmd_rpm10(st, st.reversing, resume_rpm);
                    motor_drive_speed(MOTOR_DEFAULT_ID, drive);
                    count_phase_rpm10 = drive.rpm10;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = true;
                        state_.paused = false;
                        state_.run_start_us = now_us;
                        xSemaphoreGive(state_lock_);
                    }
                    st = snapshot();
                    gui_app_update_state_button("Pause", 0x3b82f6);
                    apply_gui_refresh(&st);
                    break;
                }

                if (st.mileage_start < 0) {
                    // Idle -> start forward (record start mileage)
                    motor_mileage_resp_t mresp{};
                    int32_t start_mileage = 0;
                    if (motor_query_mileage(MOTOR_DEFAULT_ID, &mresp) == ESP_OK) {
                        start_mileage = mresp.mileage_rotations;
                    } else {
                        ESP_LOGW(TAG, "Mileage query failed at start, using 0 as origin");
                    }
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.mileage_start = start_mileage;
                        state_.mileage_now = start_mileage;
                        xSemaphoreGive(state_lock_);
                    }
                    motor_drive_cmd_t drive{};
                    drive.rpm10 = motion_cmd_rpm10(st, false, SMOOTH_SLOW_RPM);
                    motor_drive_speed(MOTOR_DEFAULT_ID, drive);
                    count_phase_rpm10 = drive.rpm10;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = true;
                        state_.reversing = false;
                        state_.paused = false;
                        state_.run_start_us = now_us;
                        xSemaphoreGive(state_lock_);
                    }
                    st = snapshot();
                    gui_app_update_state_button("Pause", 0x3b82f6);
                    gui_app_update_count_buttons(false);
                    apply_gui_refresh(&st);
                    break;
                }

                // Forward already finished and waiting for stop/reverse key.
                ESP_LOGI(TAG, "Count mode: use stop/reverse key to start return");
                break;
            }

            // ---- Timer mode state machine (original) ----
            if (st.running) {
                // Stop immediately.
                motor_brake(MOTOR_DEFAULT_ID);
                const bool was_reversing = st.reversing;
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    if (state_.running && !state_.reversing) {
                        state_.last_forward_us = now_us - state_.run_start_us;
                        state_.last_forward_rpm = state_.target_rpm;
                    }
                    state_.running = false;
                    state_.reversing = false;
                    state_.reverse_target_us = 0;
                    // If stopped while reversing: clear mileage so next run resets display
                    if (was_reversing) {
                        state_.mileage_start = -1;
                        state_.mileage_now = 0;
                    }
                    xSemaphoreGive(state_lock_);
                }
                if (was_reversing) {
                    gui_app_update_rotation_count(0, 0);
                }
                st = snapshot();
                apply_gui_refresh(&st);
                break;
            }

            // Stopped: start forward if no forward record, otherwise start reverse.
            const bool can_reverse = st.last_forward_us > 0;
            if (!can_reverse) {
                motor_drive_cmd_t drive{};
                drive.rpm10 = motion_cmd_rpm10(st, false, st.target_rpm);
                motor_drive_speed(MOTOR_DEFAULT_ID, drive);

                // Capture mileage reference for rotation display
                motor_mileage_resp_t mresp_start{};
                int32_t start_mil = 0;
                if (motor_query_mileage(MOTOR_DEFAULT_ID, &mresp_start) == ESP_OK) {
                    start_mil = mresp_start.mileage_rotations;
                }

                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.running = true;
                    state_.reversing = false;
                    state_.run_start_us = now_us;
                    state_.last_forward_us = 0;
                    state_.last_forward_rpm = 0;
                    state_.reverse_target_us = 0;
                    state_.mileage_start = start_mil;
                    state_.mileage_now = start_mil;
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
            drive.rpm10 = motion_cmd_rpm10(st, true, reverse_rpm);
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

        case CMD_COUNT_ABORT: {
            if (st.mode != MODE_COUNT) break;
            // Fully idle: no action
            if (!st.running && !st.paused && st.mileage_start < 0) break;

            // B key / UI reverse key: stop current motion then enter reverse flow.
            motor_brake(MOTOR_DEFAULT_ID);
            count_phase_rpm10 = 0;
            ESP_LOGI(TAG, "Count abort, reversing=%d", (int)st.reversing);

            if (st.reversing) {
                // Stopping reverse: full reset to origin-idle state.
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.running = false;
                    state_.reversing = false;
                    state_.paused = false;
                    state_.mileage_start = -1;
                    state_.mileage_now = 0;
                    state_.run_start_us = 0;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
                gui_app_update_rotation_count(0, st.target_count);
                last_count_display = 0;
                gui_app_update_count_buttons(true);
                gui_app_update_state_button("Start", 0x22c55e);
                apply_gui_refresh(&st);
            } else {
                // Was running/paused forward or at target: start reverse immediately.
                motor_drive_cmd_t drive{};
                drive.rpm10 = motion_cmd_rpm10(st, true, SMOOTH_SLOW_RPM);
                motor_drive_speed(MOTOR_DEFAULT_ID, drive);
                count_phase_rpm10 = drive.rpm10;
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.running = true;
                    state_.reversing = true;
                    state_.paused = false;
                    state_.run_start_us = now_us;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
                gui_app_update_state_button("Pause", 0x3b82f6);
                gui_app_update_count_buttons(false);
                apply_gui_refresh(&st);
            }
            break;
        }

        case CMD_COUNT_STEP_TOGGLE: {
            if (st.mode != MODE_COUNT || st.running || st.mileage_start >= 0) {
                break;
            }
            int16_t next_step = (st.count_step == 10) ? 1 : 10;
            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.count_step = next_step;
                xSemaphoreGive(state_lock_);
            }
            gui_app_update_count_step(next_step);
            ESP_LOGI(TAG, "Count step -> %d", (int)next_step);
            break;
        }

        case CMD_FORWARD_DIR_TOGGLE: {
            // Keep direction switching safe: only when fully idle.
            if (st.running || st.paused || st.mileage_start >= 0 || st.last_forward_us > 0) {
                ESP_LOGW(TAG, "Forward direction switch ignored: not idle");
                break;
            }
            bool next_forward_ccw = !st.forward_ccw;
            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.forward_ccw = next_forward_ccw;
                xSemaphoreGive(state_lock_);
            }
            st = snapshot();
            gui_app_update_forward_direction(st.forward_ccw);
            apply_gui_refresh(&st);
            ESP_LOGI(TAG, "Forward direction -> %s", st.forward_ccw ? "CCW" : "CW");
            break;
        }

        default:
            break;
        }
    }
}
