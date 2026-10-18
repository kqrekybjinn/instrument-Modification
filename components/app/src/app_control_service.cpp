#include "app_control_service.hpp"
#include "gui_app.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <algorithm>
#include <stdlib.h>
using workflow::Phase;
static const char *TAG = "CONTROL";
static int16_t command_rpm(const control_state_t &s, int rpm) {
 return (s.forward_ccw != s.reversing ? -1 : 1) * rpm * 10;
}
static int profile_rpm(int64_t travelled, int64_t remaining, int rpm, bool decelerate) {
 const int64_t ramp = ((rpm * rpm) / 14400 + 2) * 32768LL;
 auto envelope = [=](int64_t distance) {
  double t = std::max(0.0, std::min(1.0, (double)distance / ramp));
  return SMOOTH_SLOW_RPM + (int)((rpm - SMOOTH_SLOW_RPM) * t * t * (3.0 - 2.0 * t));
 };
 int desired = envelope(travelled);
 if (decelerate) desired = std::min(desired, envelope(remaining));
 return std::max(SMOOTH_SLOW_RPM, desired);
}
void ControlService::init() {
 state_ = control_state_t{};
 published_ = state_;
 state_lock_ = xSemaphoreCreateMutex();
 control_q_ = xQueueCreate(16, sizeof(control_cmd_t));
 configASSERT(state_lock_ && control_q_);
}
void ControlService::start_task(int core) { xTaskCreatePinnedToCore(task_trampoline, "control", 6144, this, 6, nullptr, core); }
void ControlService::task_trampoline(void *arg) { static_cast<ControlService *>(arg)->task_loop(); }
void ControlService::push_cmd(control_cmd_id_t id, int16_t value) {
 control_cmd_t cmd{id, value};
 // STOP remains reachable even if ordinary GUI input has filled the queue.
 if (id == CMD_STOP && uxQueueSpacesAvailable(control_q_) == 0) xQueueReset(control_q_);
 if (id == CMD_STOP) xQueueSendToFront(control_q_, &cmd, 0);
 else if (xQueueSend(control_q_, &cmd, 0) != pdTRUE) ESP_LOGW(TAG, "Command queue full");
}
control_state_t ControlService::snapshot() {
 xSemaphoreTake(state_lock_, portMAX_DELAY);
 auto result = published_;
 xSemaphoreGive(state_lock_);
 return result;
}
void ControlService::set_sensor_value(int32_t value) {
 xSemaphoreTake(state_lock_, portMAX_DELAY);
 published_.sensor_value = value;
 xSemaphoreGive(state_lock_);
}
void ControlService::publish() {
 xSemaphoreTake(state_lock_, portMAX_DELAY);
 state_.sensor_value = published_.sensor_value;
 published_ = state_;
 xSemaphoreGive(state_lock_);
 gui_view_t view{(uint8_t)state_.phase, state_.mode, state_.feedback_valid,
  state_.origin_valid, state_.reversing, state_.forward_ccw, state_.target_rpm,
  state_.target_count, state_.count_step, workflow::displacement(state_), state_.motor_fault, state_.mileage_now, state_.motor_angle};
 gui_app_render(&view);
}
bool ControlService::release_motor() {
 const auto err = motor_disable(MOTOR_DEFAULT_ID);
 if (err != ESP_OK) { ESP_LOGE(TAG, "Motor release write failed"); return false; }
 motor_released_ = true;
 sent_rpm10_ = 0;
 return true;
}
bool ControlService::send_speed(int16_t rpm10) {
 motor_drive_cmd_t drive{rpm10};
 const esp_err_t err = motor_drive_speed(MOTOR_DEFAULT_ID, drive);
 if (err != ESP_OK) { state_.feedback_valid = false; return false; }
 sent_rpm10_ = rpm10;
 return true;
}
bool ControlService::refresh_position() {
 last_idle_sample_us_ = esp_timer_get_time();
 motor_mileage_resp_t response{};
 const auto err = motor_query_mileage(MOTOR_DEFAULT_ID, &response);
 state_.motor_fault = err == ESP_OK ? response.fault_code : state_.motor_fault;
 state_.feedback_valid = err == ESP_OK && response.fault_code == 0 && response.position_raw < 32768;
 if (!state_.feedback_valid) {
  if (diagnostic_samples_) {
   --diagnostic_samples_;
   ESP_LOGW(TAG, "Position rejected: status=%d fault=0x%02X angle=%u", (int)err,
    (unsigned)response.fault_code, (unsigned)response.position_raw);
  }
  return false;
 }
 const int64_t position = position_math::oriented_position(response.mileage_rotations, response.position_raw, state_.forward_ccw);
 if (state_.origin_valid && response.mileage_rotations != state_.mileage_now) {
  ESP_LOGI(TAG, "CROSS raw %ld/%u -> %ld/%u start=%ld/%u delta=%lld",
   (long)state_.mileage_now,(unsigned)state_.motor_angle,
   (long)response.mileage_rotations,(unsigned)response.position_raw,
   (long)state_.mileage_start,(unsigned)state_.motor_angle_start,
   (long long)(position-state_.pos_fixed_start));
 }
 state_.pos_fixed_now = position;
 state_.mileage_now = response.mileage_rotations;
 state_.motor_angle = response.position_raw;
 if (diagnostic_samples_) {
  --diagnostic_samples_;
  ESP_LOGI(TAG, "position mileage=%ld angle=%u origin=%lld now=%lld delta=%lld target=%d phase=%u",
   (long)response.mileage_rotations, (unsigned)response.position_raw,
   (long long)state_.pos_fixed_start, (long long)position,
   (long long)workflow::displacement(state_), state_.target_count, (unsigned)state_.phase);
 }
 return true;
}
bool ControlService::should_poll_position() const {
 return workflow::motion(state_.phase) || final_sample_pending_ ||
  (state_.phase == Phase::Ready && esp_timer_get_time() - last_idle_sample_us_ >= 200000);
}
void ControlService::finish_stop(bool emergency) {
 ESP_LOGI(TAG, "Stop phase=%u delta=%lld target=%d", (unsigned)state_.settle_destination, (long long)workflow::displacement(state_), state_.target_count);
 if (emergency) {
  const auto err = motor_brake(MOTOR_DEFAULT_ID);
  if (err == ESP_OK) sent_rpm10_ = 0;
  else { state_.feedback_valid = false; ESP_LOGE(TAG, "Emergency brake write failed"); }
 } else send_speed(0); // One command, without confirmation or retransmit loops.
 const auto destination = state_.settle_destination;
 if (destination == Phase::Ready) { workflow::prepare_new(state_); release_motor(); }
 else workflow::set_phase(state_, destination);
 final_sample_pending_ = true; // One final reading; failure never locks the controls.
}
void ControlService::begin_settle(Phase destination, bool smooth) {
 state_.settle_destination = destination;
 state_.settle_smooth = smooth;
 state_.run_start_us = 0;
 workflow::set_phase(state_, Phase::Settling);
 last_motion_us_ = esp_timer_get_time();
 if (!smooth || sent_rpm10_ == 0) finish_stop();
}
void ControlService::handle_command(control_cmd_t cmd) {
 const bool unlocked = workflow::parameters_unlocked(state_);
 if (cmd.id == CMD_SET_SPEED) {
  if (workflow::valid_speed(cmd.value)) state_.target_rpm = cmd.value;
  return;
 }
 if (cmd.id == CMD_MODE_SWITCH) { if (unlocked) state_.mode ^= 1; return; }
 if (cmd.id == CMD_FORWARD_DIR_TOGGLE) { if (unlocked) state_.forward_ccw = !state_.forward_ccw; return; }
 if (cmd.id == CMD_COUNT_STEP_TOGGLE) { if (unlocked) state_.count_step = state_.count_step == 1 ? 10 : 1; return; }
 if (cmd.id == CMD_SET_COUNT || cmd.id == CMD_COUNT_INC || cmd.id == CMD_COUNT_DEC) {
  if (unlocked && state_.mode == MODE_COUNT) {
   int value = cmd.id == CMD_SET_COUNT ? cmd.value : state_.target_count + (cmd.id == CMD_COUNT_INC ? state_.count_step : -state_.count_step);
   state_.target_count = std::max(1, std::min(9999, value));
  }
  return;
 }
 if (cmd.id == CMD_NEW_RUN) {
  if (workflow::transition(state_, workflow::Action::NewRun) == Phase::Ready) { workflow::prepare_new(state_); release_motor(); final_sample_pending_ = false; }
  return;
 }
 if (cmd.id == CMD_STOP) {
  if (state_.phase == Phase::Ready) return;
  state_.settle_destination = state_.reversing ? Phase::ReturnStopped : Phase::Stopped;
  state_.settle_smooth = false;
  state_.run_start_us = 0;
  finish_stop(true); // Touch and physical Stop brake immediately in every mode.
  return;
 }
 if (cmd.id != CMD_STATE_TOGGLE && cmd.id != CMD_RETURN) return;
 // Running -> pause must not be blocked by a single failed position read.
 if (cmd.id == CMD_STATE_TOGGLE && workflow::motion(state_.phase)) {
  state_.settle_destination = state_.reversing ? Phase::ReturnPaused : Phase::Paused;
  state_.settle_smooth = false;
  state_.run_start_us = 0;
  finish_stop(true); // Pause brakes now; resume retains origin and ramps from low speed.
  return;
 }
 Phase next = workflow::transition(state_, cmd.id == CMD_RETURN ? workflow::Action::Return : workflow::Action::Toggle);
 if (next == state_.phase) return;
 if (next == Phase::Ready) { workflow::prepare_new(state_); release_motor(); return; }
 if (next == Phase::Complete) { workflow::set_phase(state_, next); return; }
 if (next != Phase::Running && next != Phase::Returning) return;
 if (state_.phase == Phase::Ready) {
  if (motor_released_) {
   if (motor_enable(MOTOR_DEFAULT_ID) != ESP_OK) return;
   motor_released_ = false;
   // Restore the previous working order after a release: enable -> wait -> speed mode.
   // Allow switch replies to complete before the origin query flushes RX.
   vTaskDelay(pdMS_TO_TICKS(100));
   if (motor_set_speed_mode(MOTOR_DEFAULT_ID) != ESP_OK) { release_motor(); return; }
   vTaskDelay(pdMS_TO_TICKS(20));
  }
  // An immediate fresh query establishes origin; never use a cached or fallback zero.
  diagnostic_samples_ = 8;
  if (!refresh_position()) { ESP_LOGW(TAG, "Start: origin query failed"); release_motor(); return; }
  state_.origin_valid = true;
  state_.pos_fixed_start = state_.pos_fixed_now;
  state_.pos_fixed_peak = state_.pos_fixed_now;
  state_.mileage_start = state_.mileage_now;
  state_.motor_angle_start = state_.motor_angle;
  ESP_LOGI(TAG, "Start target=%d mode=%u origin=%lld", state_.target_count, state_.mode, (long long)state_.pos_fixed_start);
 }
 if (!state_.origin_valid) return;
 workflow::set_phase(state_, next);
 if (next == Phase::Returning) state_.pos_fixed_peak = state_.pos_fixed_now;
 state_.run_start_us = esp_timer_get_time();
 last_motion_us_ = state_.run_start_us;
 send_speed(command_rpm(state_, SMOOTH_SLOW_RPM));
}
void ControlService::service_motion(int64_t now) {

 if (state_.phase == Phase::Settling) {
  const int rpm = abs(sent_rpm10_) / 10;
  const int drop = std::max(1, (int)((now - last_motion_us_) * 260 / 1000000));
  if (state_.settle_smooth && rpm > 8) send_speed(command_rpm(state_, std::max(8, rpm - drop)));
  else finish_stop();
  last_motion_us_ = now;
  return;
 }
 if (!workflow::motion(state_.phase)) return;
 // A missed query keeps the previous count; it does not inject stop commands into the protocol.
 if (!state_.feedback_valid) { last_motion_us_ = now; return; }
 const int64_t delta = workflow::displacement(state_);
 if (state_.phase == Phase::Running && state_.mode == MODE_COUNT && delta >= (int64_t)state_.target_count * 32768) { begin_settle(Phase::Complete, false); return; }
 if (state_.phase == Phase::Returning && delta <= 0) { begin_settle(Phase::Ready, false); return; }
 const int64_t travelled = state_.reversing ? state_.pos_fixed_peak - state_.pos_fixed_now : delta;
 const int64_t remaining = state_.reversing ? delta : (int64_t)state_.target_count * 32768 - delta;
 int desired = profile_rpm(travelled, remaining, state_.target_rpm, state_.reversing || state_.mode == MODE_COUNT);
 const int current = abs(sent_rpm10_) / 10;
 const int step = std::max(1, (int)((now - last_motion_us_) * 120 / 1000000));
 if (desired > current) desired = std::min(desired, current + step);
 else if (state_.mode == MODE_FREE && !state_.reversing) desired = std::max(desired, current - step);
 const auto rpm10 = command_rpm(state_, desired);
 if (rpm10 != sent_rpm10_) send_speed(rpm10);
 last_motion_us_ = now;
}
void ControlService::task_loop() {
 for (int percent = 0; percent < 100; percent += 4) { gui_app_update_boot(percent); vTaskDelay(pdMS_TO_TICKS(100)); }
 ESP_ERROR_CHECK(motor_driver_init());
 vTaskDelay(pdMS_TO_TICKS(100));
 ESP_ERROR_CHECK(motor_disable(MOTOR_DEFAULT_ID));
 motor_released_ = true;
 gui_app_enter_main_ui();
 int64_t last_poll_us = 0;
 while (true) {
  control_cmd_t cmd{};
  bool got = xQueueReceive(control_q_, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
  // No idle/pause polling and no feedback-driven command loops.
  const int64_t now = esp_timer_get_time();
  // User commands (especially Stop) are handled before waiting for a query.
  if (got) {
   const bool establishing_origin = state_.phase == Phase::Ready && cmd.id == CMD_STATE_TOGGLE;
   handle_command(cmd);
   // Start already performed its one origin query; do not immediately query again.
   if (establishing_origin) last_poll_us = esp_timer_get_time();
  }
  if (now - last_poll_us >= 50000) {
   if (should_poll_position()) {
    refresh_position();
    final_sample_pending_ = false;
   }
   service_motion(esp_timer_get_time());
   last_poll_us = now;
  }
  publish();
 }
}
