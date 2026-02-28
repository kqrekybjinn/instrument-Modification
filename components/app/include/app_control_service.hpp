#pragma once

#include "app_types.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

class ControlService {
public:
    void init();
    void start_task(int core_id);

    void push_cmd(control_cmd_id_t id);
    control_state_t snapshot();
    void set_sensor_value(int32_t value);

private:
    static void task_trampoline(void *arg);
    void task_loop();

    void apply_gui_refresh(const control_state_t *st);

    QueueHandle_t control_q_ = nullptr;
    SemaphoreHandle_t state_lock_ = nullptr;
    control_state_t state_{};
};
