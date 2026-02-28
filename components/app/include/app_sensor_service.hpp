#pragma once

#include "app_control_service.hpp"

#include "esp_err.h"

class SensorService {
public:
    explicit SensorService(ControlService &control);
    void start_task(int core_id);

private:
    static void task_trampoline(void *arg);
    void task_loop();

    static esp_err_t sensor_uart_init();
    static int32_t parse_sensor_value(const uint8_t *data, int len);

    ControlService &control_;
};
