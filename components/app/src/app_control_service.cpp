#include "app_control_service.hpp"

#include "freertos/task.h"

#include <limits.h>
#include "position_math.hpp"

#include "esp_log.h"
#include "esp_timer.h"

extern "C" {
#include "gui_app.h"
#include "motor_driver.h"
}

static const char *TAG = "APP";

static int16_t rpm10_to_rpm_abs(int16_t rpm10)
{
    int32_t abs_rpm10 = rpm10 < 0 ? -rpm10 : rpm10;
    return (int16_t)((abs_rpm10 + 5) / 10);
}

// Count mode speed envelope:
// - Starts from slow speed
// - Accelerates toward target speed
// - Decelerates smoothly near destination
// The ramp span is increased automatically for high target RPM.
static int16_t count_profile_rpm(int32_t progress, int32_t total, int16_t target_rpm)
{
    const int16_t k_low_rpm_threshold = 50;
    const int32_t k_low_rpm_hold_rot = 2;

    if (target_rpm < SMOOTH_SLOW_RPM) {
        target_rpm = SMOOTH_SLOW_RPM;
    }
    if (total <= 0) {
        return SMOOTH_SLOW_RPM;
    }
    if (progress < 0) {
        progress = 0;
    } else if (progress > total) {
        progress = total;
    }

    int32_t accel_ramp = ((int32_t)target_rpm * target_rpm) / 14400 + 2;
    int32_t decel_ramp = ((int32_t)target_rpm * target_rpm) / 14400 + 2;

    if (accel_ramp < (k_low_rpm_hold_rot + 2)) {
        accel_ramp = k_low_rpm_hold_rot + 2;
    }
    if (decel_ramp < (k_low_rpm_hold_rot + 2)) {
        decel_ramp = k_low_rpm_hold_rot + 2;
    }

    if (accel_ramp + decel_ramp > total) {
        int32_t sum = accel_ramp + decel_ramp;
        if (sum > 0) {
            accel_ramp = (accel_ramp * total) / sum;
            decel_ramp = total - accel_ramp;
        }
    }
    if (accel_ramp <= 0) accel_ramp = 1;
    if (decel_ramp <= 0) decel_ramp = 1;

    const int32_t span = (int32_t)target_rpm - SMOOTH_SLOW_RPM;
    if (span <= 0) {
        return SMOOTH_SLOW_RPM;
    }

    // Smoothstep(t)=3t^2-2t^3, Q10 fixed-point for a softer S-curve.
    int16_t accel_cap = target_rpm;
    if (progress < accel_ramp) {
        int32_t t = (progress * 1024) / accel_ramp;
        if (t < 0) t = 0;
        if (t > 1024) t = 1024;
        int32_t t2 = (t * t) / 1024;
        int32_t t3 = (t2 * t) / 1024;
        int32_t smooth = 3 * t2 - 2 * t3;
        accel_cap = (int16_t)(SMOOTH_SLOW_RPM + (span * smooth) / 1024);
    }

    const int32_t remaining = total - progress;
    int16_t decel_cap = target_rpm;
    if (remaining < decel_ramp) {
        int32_t t = (remaining * 1024) / decel_ramp;
        if (t < 0) t = 0;
        if (t > 1024) t = 1024;
        int32_t t2 = (t * t) / 1024;
        int32_t t3 = (t2 * t) / 1024;
        int32_t smooth = 3 * t2 - 2 * t3;
        decel_cap = (int16_t)(SMOOTH_SLOW_RPM + (span * smooth) / 1024);
    }

    int16_t envelope = accel_cap < decel_cap ? accel_cap : decel_cap;
    if (target_rpm > k_low_rpm_threshold) {
        if (progress < k_low_rpm_hold_rot || remaining < k_low_rpm_hold_rot) {
            if (envelope >= k_low_rpm_threshold) {
                envelope = k_low_rpm_threshold - 1;
            }
        }
    }
    if (envelope < SMOOTH_SLOW_RPM) {
        envelope = SMOOTH_SLOW_RPM;
    }
    return envelope;
}

// Free run mode speed profile: smooth acceleration only, no automatic deceleration.
static int16_t free_run_profile_rpm(int32_t progress, int16_t target_rpm)
{
    if (target_rpm < SMOOTH_SLOW_RPM) {
        target_rpm = SMOOTH_SLOW_RPM;
    }
    if (progress < 0) progress = 0;
    int32_t accel_ramp = ((int32_t)target_rpm * target_rpm) / 14400 + 2;
    if (accel_ramp < 2) accel_ramp = 2;
    if (progress >= accel_ramp) {
        return target_rpm;
    }
    const int32_t span = (int32_t)target_rpm - SMOOTH_SLOW_RPM;
    if (span <= 0) return SMOOTH_SLOW_RPM;
    int32_t t = (progress * 1024) / accel_ramp;
    if (t > 1024) t = 1024;
    int32_t t2 = (t * t) / 1024;
    int32_t t3 = (t2 * t) / 1024;
    int32_t smooth = 3 * t2 - 2 * t3;
    return (int16_t)(SMOOTH_SLOW_RPM + (span * smooth) / 1024);
}

// Limits per-cycle RPM change to avoid abrupt jumps.
static int16_t limit_rpm_slew(int16_t current_rpm, int16_t desired_rpm, int64_t dt_us)
{
    if (dt_us <= 0) {
        dt_us = 50000;
    } else if (dt_us > 200000) {
        dt_us = 200000;
    }

    const int32_t k_accel_rpm_per_s = 120; // Swapped to make start faster
    const int32_t k_decel_rpm_per_s = 120; // Swapped to make stop smoother
    int32_t up_step = (k_accel_rpm_per_s * dt_us + 999999) / 1000000;
    int32_t down_step = (k_decel_rpm_per_s * dt_us + 999999) / 1000000;
    if (up_step < 1) up_step = 1;
    if (down_step < 1) down_step = 1;

    if (desired_rpm > current_rpm) {
        int32_t next = current_rpm + up_step;
        if (next > desired_rpm) next = desired_rpm;
        return (int16_t)next;
    }

    if (desired_rpm < current_rpm) {
        int32_t next = current_rpm - down_step;
        if (next < desired_rpm) next = desired_rpm;
        if (next < SMOOTH_SLOW_RPM) next = SMOOTH_SLOW_RPM;
        return (int16_t)next;
    }

    return current_rpm;
}

static int16_t motion_cmd_rpm10(const control_state_t &st, bool reversing, int16_t rpm_abs)
{
    int32_t sign = st.forward_ccw ? -1 : 1;
    if (reversing) {
        sign = -sign;
    }
    return (int16_t)(sign * rpm_abs * 10);
}

static void soft_stop_count_mode(const control_state_t &st, int16_t current_rpm10)
{
    const int16_t current_rpm = rpm10_to_rpm_abs(current_rpm10);
    if (current_rpm <= 0) {
        motor_brake(MOTOR_DEFAULT_ID);
        return;
    }

    int16_t rpm = current_rpm;
    const int16_t k_stop_tail_rpm = 8;
    const int32_t k_stop_decel_rpm_per_s = 260;
    const int32_t k_step_ms = 30;
    const int32_t k_max_steps = 14;

    for (int32_t i = 0; i < k_max_steps && rpm > k_stop_tail_rpm; ++i) {
        int32_t step = (k_stop_decel_rpm_per_s * k_step_ms + 999) / 1000;
        if (step < 1) {
            step = 1;
        }
        rpm = (rpm > step) ? (int16_t)(rpm - step) : 0;
        if (rpm < k_stop_tail_rpm) {
            rpm = k_stop_tail_rpm;
        }

        motor_drive_cmd_t d{};
        d.rpm10 = motion_cmd_rpm10(st, st.reversing, rpm);
        motor_drive_speed(MOTOR_DEFAULT_ID, d);
        vTaskDelay(pdMS_TO_TICKS(k_step_ms));
    }

    // Hold a short low-speed tail before final brake to reduce stop jerk.
    motor_drive_cmd_t tail{};
    tail.rpm10 = motion_cmd_rpm10(st, st.reversing, k_stop_tail_rpm);
    motor_drive_speed(MOTOR_DEFAULT_ID, tail);
    vTaskDelay(pdMS_TO_TICKS(40));
    motor_brake(MOTOR_DEFAULT_ID);
}


bool ControlService::refresh_position()
{
    motor_mileage_resp_t response{};
    const bool valid = motor_query_mileage(MOTOR_DEFAULT_ID, &response) == ESP_OK
                    && response.fault_code == 0 && response.position_raw < 32768;
    xSemaphoreTake(state_lock_, portMAX_DELAY);
    state_.feedback_valid = valid;
    if (valid) {
        state_.mileage_now = response.mileage_rotations;
        state_.pos_fixed_now = position_math::oriented_position(
            response.mileage_rotations, response.position_raw, state_.forward_ccw);
    }
    xSemaphoreGive(state_lock_);
    return valid;
}

void ControlService::stop_and_capture(const control_state_t &st, int16_t rpm10, bool smooth)
{
    if (smooth) soft_stop_count_mode(st, rpm10);
    else motor_brake(MOTOR_DEFAULT_ID);
    xSemaphoreTake(state_lock_, portMAX_DELAY);
    state_.running = false;
    state_.paused = true;
    state_.run_start_us = 0;
    xSemaphoreGive(state_lock_);
    // Wait for 3 stable samples spanning at least 100ms, bounded by 1 second.
    const int64_t deadline = esp_timer_get_time() + 1000000;
    int stable = 0;
    int64_t previous = INT64_MIN;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!refresh_position()) { stable = 0; continue; }
        const int64_t position = snapshot().pos_fixed_now;
        const int64_t change = previous == INT64_MIN ? INT64_MAX : position - previous;
        stable = (change >= -32 && change <= 32) ? stable + 1 : 0;
        previous = position;
        if (stable >= 2) return;
    }
    xSemaphoreTake(state_lock_, portMAX_DELAY);
    state_.feedback_valid = false;
    xSemaphoreGive(state_lock_);
    ESP_LOGW(TAG, "Stop position could not be confirmed stable");
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

    state_.sensor_value = 0;
    state_.mode = MODE_FREE;
    state_.target_count = DEFAULT_TARGET_COUNT;
    state_.pos_fixed_start = INT64_MIN;
    state_.pos_fixed_now = 0;
    state_.pos_fixed_peak = 0;
    state_.mileage_start = -1;
    state_.mileage_now = 0;
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
    gui_app_update_motor(st->running, st->reversing, st->paused, st->target_rpm, false,
                         st->mode == MODE_COUNT ? 1 : 0, st->forward_ccw);
}

void ControlService::task_trampoline(void *arg)
{
    static_cast<ControlService *>(arg)->task_loop();
}

void ControlService::task_loop()
{
    ESP_LOGI(TAG, "Control task start");

    // Power-on settle time: show "电机初始化中" with progress bar for 2.5 seconds,
    // then send motor init commands and enter the normal UI.
    const int k_boot_ms = 2500;
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
    gui_app_set_mode(0);             // starts in free run mode
    gui_app_update_rotation_count(0, 0);
    gui_app_update_count_step(10);
    gui_app_update_forward_direction(false);

    int32_t last_count_display = -1;
    int64_t last_feedback_us = 0;
    // Tracks the last RPM10 command sent in count/free run mode.
    int16_t count_phase_rpm10 = 0;
    int64_t count_speed_update_us = 0;

    while (1) {
        control_cmd_t cmd;
        const bool got = xQueueReceive(control_q_, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;

        control_state_t st = snapshot();
        const int64_t now_us = esp_timer_get_time();

        if (st.pos_fixed_start != INT64_MIN) {
            if (refresh_position()) {
                last_feedback_us = esp_timer_get_time();
            }
            st = snapshot();
            if (st.running && !st.feedback_valid) {
                // Retry brief dropouts; never continue motion on stale data indefinitely.
                if (last_feedback_us == 0) last_feedback_us = now_us;
                if (now_us - last_feedback_us >= 300000) {
                    stop_and_capture(st, count_phase_rpm10, false);
                    count_phase_rpm10 = 0;
                    count_speed_update_us = 0;
                    xSemaphoreTake(state_lock_, portMAX_DELAY);
                    state_.running = false;
                    state_.paused = true;
                    state_.run_start_us = 0;
                    xSemaphoreGive(state_lock_);
                    st = snapshot();
                    apply_gui_refresh(&st);
                    gui_app_update_state_button("Feedback lost", 0xef4444);
                }
                // Do not process start/resume commands against stale feedback.
                continue;
            }
            const int64_t displacement = st.pos_fixed_now - st.pos_fixed_start;
            const int32_t display = position_math::hundredths(displacement);
            if (display != last_count_display) {
                gui_app_update_rotation_fixed(displacement, st.mode == MODE_COUNT ? st.target_count : 0);
                last_count_display = display;
            }
        } else {
            last_feedback_us = now_us;
        }

        // ---- Free run mode: mileage polling + speed profile (forward & reverse return) ----
        if (st.mode == MODE_FREE && st.running) {


            int64_t delta_fixed = (st.pos_fixed_start != INT64_MIN)
                                  ? (st.pos_fixed_now - st.pos_fixed_start) : 0;
            if (delta_fixed < 0) delta_fixed = 0;
            int32_t delta = (int32_t)(delta_fixed / 32768);


            int64_t dt_us = 50000;
            if (count_speed_update_us > 0 && now_us > count_speed_update_us) {
                dt_us = now_us - count_speed_update_us;
            }
            if (dt_us < 1000) dt_us = 1000;
            if (dt_us > 200000) dt_us = 200000;

            if (!st.reversing) {
                // Forward: accel-only smooth profile
                const int16_t desired = free_run_profile_rpm(delta, st.target_rpm);
                const int16_t current_free = rpm10_to_rpm_abs(count_phase_rpm10);
                const int16_t next_free = limit_rpm_slew(current_free, desired, dt_us);
                const int16_t next_rpm10 = motion_cmd_rpm10(st, false, next_free);
                if (next_rpm10 != count_phase_rpm10) {
                    motor_drive_cmd_t d{};
                    d.rpm10 = next_rpm10;
                    motor_drive_speed(MOTOR_DEFAULT_ID, d);
                    count_phase_rpm10 = next_rpm10;
                }
                count_speed_update_us = now_us;
            } else {
                // Reverse return: smooth accel+decel profile toward origin
                int64_t total_fixed = (st.pos_fixed_peak > st.pos_fixed_start)
                                      ? (st.pos_fixed_peak - st.pos_fixed_start) : 32768;
                int32_t total = (int32_t)(total_fixed / 32768);
                if (total < 1) total = 1;
                // rev_progress: 0 at peak (start of return) → total at origin (end)
                // delta = current distance from start, decreasing as we return
                int32_t rev_progress = total - delta;
                if (rev_progress < 0) rev_progress = 0;
                if (rev_progress > total) rev_progress = total;
                const int16_t desired_rev = count_profile_rpm(rev_progress, total, st.target_rpm);
                const int16_t current_rev = rpm10_to_rpm_abs(count_phase_rpm10);
                // Bypass slew for decel so S-curve profile is followed precisely near origin
                const int16_t next_rev = (desired_rev >= current_rev)
                                         ? limit_rpm_slew(current_rev, desired_rev, dt_us)
                                         : desired_rev;
                const int16_t next_rev_rpm10 = motion_cmd_rpm10(st, true, next_rev);
                if (next_rev_rpm10 != count_phase_rpm10) {
                    motor_drive_cmd_t d{};
                    d.rpm10 = next_rev_rpm10;
                    motor_drive_speed(MOTOR_DEFAULT_ID, d);
                    count_phase_rpm10 = next_rev_rpm10;
                }
                count_speed_update_us = now_us;
                // Auto-stop when position returns to or past origin
                if (st.pos_fixed_start != INT64_MIN && st.pos_fixed_now <= st.pos_fixed_start) {
                    ESP_LOGI(TAG, "Free mode: return to origin complete");
                    stop_and_capture(st, count_phase_rpm10, false);
                    st = snapshot();
                    if (!st.feedback_valid || st.pos_fixed_now > st.pos_fixed_start) {
                        count_phase_rpm10 = 0;
                        count_speed_update_us = 0;
                        apply_gui_refresh(&st);
                        gui_app_update_state_button("Retry", 0xef4444);
                        continue;
                    }
                    count_phase_rpm10 = 0;
                    count_speed_update_us = 0;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = false;
                        state_.reversing = false;
                        state_.paused = false;
                        state_.pos_fixed_start = INT64_MIN;
                        state_.pos_fixed_peak = 0;
                        state_.pos_fixed_now = 0;
                        state_.mileage_start = -1;
                        state_.mileage_now = 0;
                        state_.run_start_us = 0;
                        xSemaphoreGive(state_lock_);
                    }
                    st = snapshot();
                    gui_app_update_rotation_count(0, 0);
                    last_count_display = 0;
                    gui_app_update_state_button("Start", 0x22c55e);
                    apply_gui_refresh(&st);
                }
            }
        // ---- Count mode: mileage feedback polling (0x74 → 0x75) ----
        } else if (st.mode == MODE_COUNT && st.running) {


            // delta_fixed: Q15 units of rotation completed since run start
            // 1 rotation = 32768 units; target = target_count * 32768
            int64_t delta_fixed = (st.pos_fixed_start != INT64_MIN)
                                  ? (st.pos_fixed_now - st.pos_fixed_start)
                                  : 0;
            if (delta_fixed < 0) delta_fixed = 0;
            const int64_t target_fixed = (int64_t)st.target_count * 32768;

            // Integer rotation count for display (floor)
            int32_t delta = (int32_t)(delta_fixed / 32768);

            int64_t dt_us = 50000;
            if (count_speed_update_us > 0 && now_us > count_speed_update_us) {
                dt_us = now_us - count_speed_update_us;
            }
            if (dt_us < 1000) dt_us = 1000;
            if (dt_us > 200000) dt_us = 200000;

            if (!st.reversing) {
                const int16_t desired_fwd = count_profile_rpm(delta, st.target_count, st.target_rpm);
                const int16_t current_fwd = rpm10_to_rpm_abs(count_phase_rpm10);
                // Bypass slew for deceleration: follow S-curve profile directly for precise stop.
                // Slew limiting is kept only for acceleration to prevent jerk on start/resume.
                const int16_t next_fwd = (desired_fwd >= current_fwd)
                                         ? limit_rpm_slew(current_fwd, desired_fwd, dt_us)
                                         : desired_fwd;
                const int16_t next_fwd_rpm10 = motion_cmd_rpm10(st, false, next_fwd);
                if (next_fwd_rpm10 != count_phase_rpm10) {
                    motor_drive_cmd_t d{};
                    d.rpm10 = next_fwd_rpm10;
                    motor_drive_speed(MOTOR_DEFAULT_ID, d);
                    count_phase_rpm10 = next_fwd_rpm10;
                }
                count_speed_update_us = now_us;

                // Forward: auto-stop when fixed-point delta reaches target (sub-rotation precision)
                if (delta_fixed >= target_fixed) {
                    ESP_LOGI(TAG, "Count mode: target reached (delta_fixed=%lld target=%lld)",
                             (long long)delta_fixed, (long long)target_fixed);
                    stop_and_capture(st, count_phase_rpm10, false);
                    st = snapshot();
                    if (!st.feedback_valid) {
                        count_phase_rpm10 = 0;
                        count_speed_update_us = 0;
                        apply_gui_refresh(&st);
                        gui_app_update_state_button("Feedback lost", 0xef4444);
                        continue;
                    }
                    count_phase_rpm10 = 0;
                    count_speed_update_us = 0;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = false;
                        state_.paused = false;
                        state_.run_start_us = 0;
                        xSemaphoreGive(state_lock_);
                    }
                    st = snapshot();
                    gui_app_update_rotation_fixed(st.pos_fixed_now - st.pos_fixed_start, st.target_count);
                    last_count_display = position_math::hundredths(st.pos_fixed_now - st.pos_fixed_start);
                    gui_app_update_state_button("Start", 0x22c55e);
                    gui_app_update_count_buttons(false);
                    gui_app_update_motor(false, false, false, st.target_rpm, false, 1, st.forward_ccw);
                }
            } else {
                // Reverse: progress is how far we are from origin (target_fixed - delta_fixed)
                int64_t rev_fixed = target_fixed - delta_fixed;
                if (rev_fixed < 0) rev_fixed = 0;
                int32_t rev_progress = (int32_t)(rev_fixed / 32768);
                const int16_t desired_rev = count_profile_rpm(rev_progress, st.target_count, st.target_rpm);
                const int16_t current_rev = rpm10_to_rpm_abs(count_phase_rpm10);
                const int16_t next_rev = (desired_rev >= current_rev)
                                         ? limit_rpm_slew(current_rev, desired_rev, dt_us)
                                         : desired_rev;
                const int16_t next_rev_rpm10 = motion_cmd_rpm10(st, true, next_rev);
                if (next_rev_rpm10 != count_phase_rpm10) {
                    motor_drive_cmd_t d{};
                    d.rpm10 = next_rev_rpm10;
                    motor_drive_speed(MOTOR_DEFAULT_ID, d);
                    count_phase_rpm10 = next_rev_rpm10;
                }
                count_speed_update_us = now_us;

                // Reverse: auto-stop when fixed-point position returns to or past origin
                if (st.pos_fixed_start != INT64_MIN && st.pos_fixed_now <= st.pos_fixed_start) {
                    ESP_LOGI(TAG, "Count mode: reverse complete, at origin");
                    stop_and_capture(st, count_phase_rpm10, false);
                    st = snapshot();
                    if (!st.feedback_valid || st.pos_fixed_now > st.pos_fixed_start) {
                        count_phase_rpm10 = 0;
                        count_speed_update_us = 0;
                        apply_gui_refresh(&st);
                        gui_app_update_state_button("Retry", 0xef4444);
                        continue;
                    }
                    count_phase_rpm10 = 0;
                    count_speed_update_us = 0;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = false;
                        state_.reversing = false;
                        state_.paused = false;
                        state_.pos_fixed_start = INT64_MIN;
                        state_.pos_fixed_now = 0;
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
        } else {
            count_speed_update_us = 0;
        }

        if (!got) {
            continue;
        }

        // ---- Commands ----
        switch (cmd.id) {
        case CMD_SPEED_STEP: {
            // Only block speed change when running in count mode.
            // In free run mode, speed can be changed while running for manual speed control.
            if (st.mode == MODE_COUNT && st.running) {
                ESP_LOGW(TAG, "Speed step ignored: count mode running");
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
            // Only allow mode switch when fully idle.
            if (st.running || st.paused || st.pos_fixed_start != INT64_MIN) {
                ESP_LOGW(TAG, "Mode switch ignored: not idle");
                break;
            }

            operating_mode_t new_mode = (st.mode == MODE_FREE) ? MODE_COUNT : MODE_FREE;
            if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                state_.mode = new_mode;
                state_.paused = false;
                state_.pos_fixed_start = INT64_MIN;
                state_.pos_fixed_now = 0;
                state_.mileage_start = -1;
                state_.mileage_now = 0;
                xSemaphoreGive(state_lock_);
            }
            st = snapshot();

            if (new_mode == MODE_COUNT) {
                gui_app_set_mode(1);
                gui_app_update_rotation_count(0, st.target_count);
                gui_app_update_count_buttons(true);
                last_count_display = 0;
            } else {
                gui_app_set_mode(0);
                gui_app_update_rotation_count(0, 0);
                last_count_display = 0;
            }
            gui_app_update_state_button("Start", 0x22c55e);
            apply_gui_refresh(&st);
            ESP_LOGI(TAG, "Mode -> %s", new_mode == MODE_FREE ? "FREE" : "COUNT");
            break;
        }

        case CMD_COUNT_INC: {
            // Only adjust when in count mode, stopped, and at origin
            if (st.mode != MODE_COUNT || st.running || st.pos_fixed_start != INT64_MIN) {
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
            if (st.mode != MODE_COUNT || st.running || st.pos_fixed_start != INT64_MIN) {
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
                int64_t cur_delta_fixed = (st.pos_fixed_start != INT64_MIN)
                                         ? (st.pos_fixed_now - st.pos_fixed_start) : 0;
                if (cur_delta_fixed < 0) cur_delta_fixed = 0;

                if (st.running) {
                    // Running (forward/reverse) -> pause
                    stop_and_capture(st, count_phase_rpm10, true);
                    st = snapshot();
                    count_phase_rpm10 = 0;
                    count_speed_update_us = 0;
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        state_.running = false;
                        state_.paused = true;
                        state_.run_start_us = 0;
                        xSemaphoreGive(state_lock_);
                    }
                    st = snapshot();
                    gui_app_update_rotation_fixed(st.pos_fixed_now - st.pos_fixed_start, st.target_count);
                    last_count_display = position_math::hundredths(st.pos_fixed_now - st.pos_fixed_start);
                    gui_app_update_state_button("Resume", 0x22c55e);
                    gui_app_update_count_buttons(false);
                    apply_gui_refresh(&st);
                    break;
                }

                if (st.paused) {
                    if (!st.feedback_valid) {
                        gui_app_update_state_button("Retry", 0xef4444);
                        break;
                    }
                    // Paused -> resume same direction
                    if (!st.reversing && cur_delta_fixed >= (int64_t)st.target_count * 32768) {
                        ESP_LOGW(TAG, "Count mode: target reached, use stop/reverse");
                        break;
                    }

                    motor_drive_cmd_t drive{};
                    drive.rpm10 = motion_cmd_rpm10(st, st.reversing, SMOOTH_SLOW_RPM);
                    motor_drive_speed(MOTOR_DEFAULT_ID, drive);
                    count_phase_rpm10 = drive.rpm10;
                    count_speed_update_us = now_us;
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

                if (st.pos_fixed_start == INT64_MIN) {
                    // Idle -> start forward (record sub-rotation start position)
                    motor_mileage_resp_t mresp{};
                    int32_t start_mileage = 0;
                    int64_t start_fixed = 0;
                    if (motor_query_mileage(MOTOR_DEFAULT_ID, &mresp) == ESP_OK && mresp.fault_code == 0 && mresp.position_raw < 32768) {
                        start_mileage = mresp.mileage_rotations;
                        start_fixed = position_math::oriented_position(mresp.mileage_rotations, mresp.position_raw, st.forward_ccw);
                    } else {
                        ESP_LOGW(TAG, "Start refused: no valid origin feedback");
                        gui_app_update_state_button("Retry", 0xef4444);
                        break;
                    }
                    if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                        // Record exact sub-rotation start position for precise delta tracking.
                        state_.feedback_valid = true;
                        state_.pos_fixed_start = start_fixed;
                        state_.pos_fixed_now = start_fixed;
                        state_.mileage_start = start_mileage;
                        state_.mileage_now = start_mileage;
                        xSemaphoreGive(state_lock_);
                    }
                    motor_drive_cmd_t drive{};
                    drive.rpm10 = motion_cmd_rpm10(st, false, SMOOTH_SLOW_RPM);
                    motor_drive_speed(MOTOR_DEFAULT_ID, drive);
                    count_phase_rpm10 = drive.rpm10;
                    count_speed_update_us = now_us;
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

            // ---- Free run mode: 4-state toggle ----
            // 1. Fully idle → start forward
            // 2. Running forward → soft stop, keep pos for return
            // 3. Stopped off-origin (paused) → start reverse return
            // 4. Running reverse → cancel return, full reset
            if (st.pos_fixed_start == INT64_MIN) {
                // Idle → start forward
                motor_mileage_resp_t mresp{};
                int32_t start_mileage = 0;
                int64_t start_fixed = 0;
                if (motor_query_mileage(MOTOR_DEFAULT_ID, &mresp) == ESP_OK && mresp.fault_code == 0 && mresp.position_raw < 32768) {
                    start_mileage = mresp.mileage_rotations;
                    start_fixed = position_math::oriented_position(mresp.mileage_rotations, mresp.position_raw, st.forward_ccw);
                } else {
                    ESP_LOGW(TAG, "Start refused: no valid origin feedback");
                    gui_app_update_state_button("Retry", 0xef4444);
                    break;
                }
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.feedback_valid = true;
                    state_.pos_fixed_start = start_fixed;
                    state_.pos_fixed_now = start_fixed;
                    state_.pos_fixed_peak = 0;
                    state_.mileage_start = start_mileage;
                    state_.mileage_now = start_mileage;
                    xSemaphoreGive(state_lock_);
                }
                motor_drive_cmd_t drive{};
                drive.rpm10 = motion_cmd_rpm10(st, false, SMOOTH_SLOW_RPM);
                motor_drive_speed(MOTOR_DEFAULT_ID, drive);
                count_phase_rpm10 = drive.rpm10;
                count_speed_update_us = now_us;
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.running = true;
                    state_.reversing = false;
                    state_.paused = false;
                    state_.run_start_us = now_us;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
                apply_gui_refresh(&st);
            } else if (st.running && !st.reversing) {
                // Running forward → soft stop, record peak, enter "ready to return" state
                stop_and_capture(st, count_phase_rpm10, true);
                st = snapshot();
                count_phase_rpm10 = 0;
                count_speed_update_us = 0;
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.pos_fixed_peak = state_.pos_fixed_now;
                    state_.running = false;
                    state_.paused = true;   // "ready to return"
                    state_.run_start_us = 0;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
                gui_app_update_rotation_fixed(st.pos_fixed_now - st.pos_fixed_start, 0);
                last_count_display = position_math::hundredths(st.pos_fixed_now - st.pos_fixed_start);
                gui_app_update_state_button("Return", 0x3b82f6);
                apply_gui_refresh(&st);
            } else if (!st.running && st.paused && !st.reversing) {
                if (!st.feedback_valid) {
                    gui_app_update_state_button("Retry", 0xef4444);
                    break;
                }
                // Stopped off-origin → start reverse return
                motor_drive_cmd_t drive{};
                drive.rpm10 = motion_cmd_rpm10(st, true, SMOOTH_SLOW_RPM);
                motor_drive_speed(MOTOR_DEFAULT_ID, drive);
                count_phase_rpm10 = drive.rpm10;
                count_speed_update_us = now_us;
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.running = true;
                    state_.reversing = true;
                    state_.paused = false;
                    state_.run_start_us = now_us;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
                apply_gui_refresh(&st);
            } else if (st.running && st.reversing) {
                // Returning → cancel, full reset to idle
                stop_and_capture(st, count_phase_rpm10, true);
                st = snapshot();
                count_phase_rpm10 = 0;
                count_speed_update_us = 0;
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.running = false;
                    state_.reversing = false;
                    state_.paused = false;
                    state_.pos_fixed_start = INT64_MIN;
                    state_.pos_fixed_peak = 0;
                    state_.pos_fixed_now = 0;
                    state_.mileage_start = -1;
                    state_.mileage_now = 0;
                    state_.run_start_us = 0;
                    xSemaphoreGive(state_lock_);
                }
                st = snapshot();
                last_count_display = -1;
                gui_app_update_rotation_count(0, 0);
                gui_app_update_state_button("Start", 0x22c55e);
                apply_gui_refresh(&st);
            }
            break;
        }

        case CMD_COUNT_ABORT: {
            if (st.mode != MODE_COUNT) break;
            // Fully idle: no action
            if (!st.running && !st.paused && st.pos_fixed_start == INT64_MIN) break;

            // B key / UI reverse key: stop current motion then enter reverse flow.
            if (st.running) {
                stop_and_capture(st, count_phase_rpm10, true);
                st = snapshot();
            } else {
                motor_brake(MOTOR_DEFAULT_ID);
            }
            count_phase_rpm10 = 0;
            count_speed_update_us = 0;
            ESP_LOGI(TAG, "Count abort, reversing=%d", (int)st.reversing);

            if (st.reversing) {
                // Stopping reverse: full reset to origin-idle state.
                if (state_lock_ && xSemaphoreTake(state_lock_, portMAX_DELAY) == pdTRUE) {
                    state_.running = false;
                    state_.reversing = false;
                    state_.paused = false;
                    state_.pos_fixed_start = INT64_MIN;
                    state_.pos_fixed_now = 0;
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
                if (!st.feedback_valid) {
                    gui_app_update_state_button("Retry", 0xef4444);
                    break;
                }
                // Was running/paused forward or at target: start reverse immediately.
                motor_drive_cmd_t drive{};
                drive.rpm10 = motion_cmd_rpm10(st, true, SMOOTH_SLOW_RPM);
                motor_drive_speed(MOTOR_DEFAULT_ID, drive);
                count_phase_rpm10 = drive.rpm10;
                count_speed_update_us = now_us;
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
            if (st.mode != MODE_COUNT || st.running || st.pos_fixed_start != INT64_MIN) {
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
            if (st.running || st.paused || st.pos_fixed_start != INT64_MIN) {
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
