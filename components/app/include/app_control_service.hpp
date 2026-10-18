#pragma once
#include "app_types.hpp"
#include "phase_position.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
class ControlService {
public:
 void init();
 void start_task(int core_id);
 void push_cmd(control_cmd_id_t id, int16_t value = 0);
 control_state_t snapshot();
 void set_sensor_value(int32_t value);
private:
#ifdef MODI_HOST_TEST
 friend struct ControlServiceTest;
#endif
 static void task_trampoline(void *arg);
 void task_loop();
 bool refresh_position();
 void publish();
 bool should_poll_position() const;
 void finish_stop(bool emergency = false);
 void begin_settle(workflow::Phase destination, bool smooth);
 void handle_command(control_cmd_t command);
 void service_motion(int64_t now);
 bool send_speed(int16_t rpm10);
 bool release_motor();
 QueueHandle_t control_q_ = nullptr;
 SemaphoreHandle_t state_lock_ = nullptr;
 control_state_t state_{}; // Written only by control task after init.
 control_state_t published_{}; // Cross-task snapshot guarded by state_lock_.
 position_math::PhasePosition position_tracker_;
 int64_t last_count_warning_us_ = 0;
 bool motor_released_ = true;
 unsigned diagnostic_samples_ = 0;
 bool final_sample_pending_ = false;
 int16_t sent_rpm10_ = 0;
 int sample_peak_rpm_ = 0;
 int64_t last_motion_us_ = 0;
 int64_t last_idle_sample_us_ = 0;
};
