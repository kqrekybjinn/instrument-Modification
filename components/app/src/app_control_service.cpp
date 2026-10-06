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
  state_.target_count, state_.count_step, workflow::displacement(state_), state_.motor_fault};
 gui_app_render(&view);
}
bool ControlService::send_speed(int16_t rpm10) {
 motor_drive_cmd_t drive{rpm10};
 const esp_err_t err = motor_drive_speed(MOTOR_DEFAULT_ID, drive);
 if (err != ESP_OK) { state_.feedback_valid = false; return false; }
 sent_rpm10_ = rpm10;
 return true;
}
bool ControlService::refresh_position() {
 motor_mileage_resp_t response{};
 const auto err = motor_query_mileage(MOTOR_DEFAULT_ID, &response);
 state_.motor_fault = err == ESP_OK ? response.fault_code : state_.motor_fault;
 state_.feedback_valid = err == ESP_OK && response.fault_code == 0 && response.position_raw < 32768;
 if (!state_.feedback_valid) return false;
 const int64_t sample_us = esp_timer_get_time();
 int64_t raw_step = 0;
 if (state_.origin_valid) {
  raw_step = position_math::angle_step(previous_angle_, response.position_raw);
  const int64_t elapsed = sample_us - previous_sample_us_;
  // At <=380 RPM, ordinary <=70ms samples move less than half a turn.
  // Mileage never contributes to these samples: asynchronous whole-turn updates
  // cannot create a phantom turn or an early count-mode stop.
  if (elapsed > 70000 && sent_rpm10_ != 0) {
   // Long communication gaps cannot be uniquely resolved from one angle.
   // Use mileage only for recovery when its candidate obeys a physical speed bound.
   const int64_t candidate = ((int64_t)response.mileage_rotations - previous_mileage_) * 32768
       + (int32_t)response.position_raw - previous_angle_;
   const int64_t bound = elapsed * 400 * 32768 / 60000000 + 1024;
   if (llabs(candidate) <= bound) raw_step = candidate;
   else ESP_LOGW(TAG, "Ambiguous count gap: mileage inconsistent; whole turns may be lost");
  }
  state_.pos_fixed_now += state_.forward_ccw ? -raw_step : raw_step;
 } else {
  // Each new task starts from a local zero; the motor's integer counter is not origin.
  state_.pos_fixed_now = 0;
 }
 previous_angle_ = response.position_raw;
 previous_mileage_ = response.mileage_rotations;
 previous_sample_us_ = sample_us;
 state_.mileage_now = response.mileage_rotations;
 if (diagnostic_samples_) {
  --diagnostic_samples_;
  ESP_LOGI(TAG, "position mileage=%ld angle=%u origin=%lld now=%lld delta=%lld target=%d phase=%u",
   (long)response.mileage_rotations, (unsigned)response.position_raw,
   (long long)state_.pos_fixed_start, (long long)state_.pos_fixed_now,
   (long long)workflow::displacement(state_), state_.target_count, (unsigned)state_.phase);
 }
 return true;
}
bool ControlService::should_poll_position() const {
 return workflow::motion(state_.phase) || final_sample_pending_;
}
void ControlService::finish_stop(bool emergency) {
 ESP_LOGI(TAG, "Stop phase=%u delta=%lld target=%d", (unsigned)state_.settle_destination, (long long)workflow::displacement(state_), state_.target_count);
 if (emergency) {
  const auto err = motor_brake(MOTOR_DEFAULT_ID);
  if (err == ESP_OK) sent_rpm10_ = 0;
  else { state_.feedback_valid = false; ESP_LOGE(TAG, "Emergency brake write failed"); }
 } else send_speed(0); // One command, without confirmation or retransmit loops.
 const auto destination = state_.settle_destination;
 if (destination == Phase::Ready) workflow::prepare_new(state_);
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
  if (workflow::transition(state_, workflow::Action::NewRun) == Phase::Ready) { workflow::prepare_new(state_); final_sample_pending_ = false; }
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
 if (next == Phase::Ready) { workflow::prepare_new(state_); return; }
 if (next == Phase::Complete) { workflow::set_phase(state_, next); return; }
 if (next != Phase::Running && next != Phase::Returning) return;
 if (state_.phase == Phase::Ready) {
  // An immediate fresh query establishes origin; never use a cached or fallback zero.
  diagnostic_samples_ = 8;
  if (!refresh_position()) { ESP_LOGW(TAG, "Start: origin query failed"); return; }
  state_.origin_valid = true;
  state_.pos_fixed_start = state_.pos_fixed_now;
  state_.pos_fixed_peak = state_.pos_fixed_now;
  state_.mileage_start = state_.mileage_now;
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
 ESP_ERROR_CHECK(motor_enable(MOTOR_DEFAULT_ID));
 vTaskDelay(pdMS_TO_TICKS(100));
 ESP_ERROR_CHECK(motor_set_speed_mode(MOTOR_DEFAULT_ID));
 send_speed(0);
 gui_app_enter_main_ui();
 int64_t last_poll_us = 0;
 while (true) {
  control_cmd_t cmd{};
  // Include query duration in the 50ms period rather than sleeping 50ms after each query.
  const int64_t until_poll = last_poll_us + 50000 - esp_timer_get_time();
  const int wait_ms = (int)std::max<int64_t>(1, std::min<int64_t>(50, until_poll / 1000));
  bool got = xQueueReceive(control_q_, &cmd, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
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
