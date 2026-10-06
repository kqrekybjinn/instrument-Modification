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
 const int64_t now = esp_timer_get_time();
 const int64_t position = position_math::oriented_position(response.mileage_rotations, response.position_raw, state_.forward_ccw);
 if (state_.origin_valid && last_feedback_us_ > 0) {
  // Reject discontinuities inconsistent with the maximum configured speed.
  // Small power-on counter resets cannot be identified conclusively from 0x75 alone.
  const int64_t max_change = (now - last_feedback_us_) * 380 * 32768 / 60000000 + 16384;
  if (llabs(position - state_.pos_fixed_now) > max_change) {
   ESP_LOGE(TAG, "Position discontinuity; origin is no longer trustworthy");
   send_speed(0);
   state_.feedback_valid = false;
   state_.origin_valid = false;
   state_.pos_fixed_start = INT64_MIN;
   workflow::set_phase(state_, Phase::OriginLost);
   return false;
  }
 }
 state_.pos_fixed_now = position;
 state_.mileage_now = response.mileage_rotations;
 last_feedback_us_ = now;
 return true;
}
void ControlService::begin_settle(Phase destination, bool smooth) {
 state_.settle_destination = destination;
 state_.settle_smooth = smooth;
 state_.run_start_us = 0;
 workflow::set_phase(state_, Phase::Settling);
 settle_started_us_ = esp_timer_get_time();
 last_motion_us_ = settle_started_us_;
 settle_previous_ = INT64_MIN;
 stable_samples_ = 0;
 zero_sent_ = false;
 if (!smooth || sent_rpm10_ == 0) zero_sent_ = send_speed(0);
}
void ControlService::handle_command(control_cmd_t cmd) {
 const bool unlocked = workflow::parameters_unlocked(state_);
 if (cmd.id == CMD_SET_SPEED) {
  if (state_.phase != Phase::Settling && workflow::valid_speed(cmd.value)) state_.target_rpm = cmd.value;
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
  if (workflow::transition(state_, workflow::Action::NewRun) == Phase::Ready) workflow::prepare_new(state_);
  return;
 }
 if (cmd.id == CMD_STOP) {
  if (state_.phase == Phase::Settling) { state_.settle_destination = state_.reversing ? Phase::ReturnStopped : Phase::Stopped; state_.settle_smooth = false; zero_sent_ = send_speed(0); return; }
  if (state_.phase == Phase::Ready) return;

  begin_settle(state_.reversing ? Phase::ReturnStopped : Phase::Stopped, true);
  return;
 }
 if (cmd.id != CMD_STATE_TOGGLE && cmd.id != CMD_RETURN) return;
 // Running -> pause must not be blocked by a single failed position read.
 if (cmd.id == CMD_STATE_TOGGLE && workflow::motion(state_.phase)) {
  begin_settle(state_.reversing ? Phase::ReturnPaused : Phase::Paused, true);
  return;
 }
 if (!state_.feedback_valid || state_.motor_fault) return;
 Phase next = workflow::transition(state_, cmd.id == CMD_RETURN ? workflow::Action::Return : workflow::Action::Toggle);
 if (next == state_.phase) return;
 if (next == Phase::Ready) { workflow::prepare_new(state_); return; }
 if (next == Phase::Complete) { workflow::set_phase(state_, next); return; }
 if (next != Phase::Running && next != Phase::Returning) return;
 if (state_.phase == Phase::Ready) {
  // An immediate fresh query establishes origin; never use a cached or fallback zero.
  if (!refresh_position()) return;
  state_.origin_valid = true;
  state_.pos_fixed_start = state_.pos_fixed_now;
  state_.pos_fixed_peak = state_.pos_fixed_now;
  state_.mileage_start = state_.mileage_now;
 }
 if (!state_.origin_valid) return;
 workflow::set_phase(state_, next);
 if (next == Phase::Returning) state_.pos_fixed_peak = state_.pos_fixed_now;
 state_.run_start_us = esp_timer_get_time();
 last_motion_us_ = state_.run_start_us;
 if (!send_speed(command_rpm(state_, SMOOTH_SLOW_RPM))) begin_settle(Phase::FeedbackLost, false);
}
void ControlService::service_motion(int64_t now) {

 if (state_.phase == Phase::Settling) {
  if (!zero_sent_) {
   int rpm = abs(sent_rpm10_) / 10;
   int drop = std::max(1, (int)((now - last_motion_us_) * 260 / 1000000));
   if (state_.settle_smooth && rpm > 8) send_speed(command_rpm(state_, std::max(8, rpm - drop)));
   else zero_sent_ = send_speed(0);
   last_motion_us_ = now;
  }
  if (zero_sent_ && state_.feedback_valid) {
   const auto current = state_.pos_fixed_now;
   const bool stable = settle_previous_ != INT64_MIN && llabs(current - settle_previous_) <= 32;
   stable_samples_ = stable ? stable_samples_ + 1 : 0;
   settle_previous_ = current;
   if (stable_samples_ >= 2) {
    Phase destination = state_.settle_destination;
    if (destination == Phase::Ready) {
     if (workflow::displacement(state_) <= 0) workflow::prepare_new(state_);
     else workflow::set_phase(state_, Phase::ReturnPaused);
    } else workflow::set_phase(state_, destination);
    return;
   }
  } else { stable_samples_ = 0; settle_previous_ = INT64_MIN; }
  // At max RPM smooth stop can take ~1.5s; only declare failure after the ramp plus settling.
  if (now - settle_started_us_ > 3000000) {
   send_speed(0);
   workflow::set_phase(state_, Phase::FeedbackLost);
   state_.feedback_valid = false;
   stable_samples_ = 0;
   settle_previous_ = INT64_MIN;
  }
  return;
 }
 if (state_.phase == Phase::FeedbackLost || state_.phase == Phase::OriginLost) {
  // Restoration requires fresh stable samples; it never starts the motor automatically.
  send_speed(0);
  if (state_.feedback_valid) {
   const auto position = state_.pos_fixed_now;
   stable_samples_ = settle_previous_ != INT64_MIN && llabs(position - settle_previous_) <= 32 ? stable_samples_ + 1 : 0;
   settle_previous_ = position;
   if (stable_samples_ < 2) state_.feedback_valid = false;
  } else { stable_samples_ = 0; settle_previous_ = INT64_MIN; }
  return;
 }
 if (!workflow::motion(state_.phase)) return;
 if (state_.motor_fault || !state_.feedback_valid) {
  if (state_.motor_fault || now - last_feedback_us_ >= 300000) {
   begin_settle(Phase::FeedbackLost, false);
  }
  return;
 }
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
 if (rpm10 != sent_rpm10_ && !send_speed(rpm10)) begin_settle(Phase::FeedbackLost, false);
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
 last_feedback_us_ = esp_timer_get_time();
 int64_t last_poll_us = 0;
 while (true) {
  control_cmd_t cmd{};
  bool got = xQueueReceive(control_q_, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
  // Poll before interpreting any motion request, including after a stopped state.
  const int64_t now = esp_timer_get_time();
  if (now - last_poll_us >= 50000) {
   refresh_position();
   service_motion(esp_timer_get_time());
   last_poll_us = now;
  }
  if (got) handle_command(cmd);
  publish();
 }
}
