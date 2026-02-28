#pragma once

#include "app_control_service.hpp"

#include "driver/gpio.h"

class InputService {
public:
    explicit InputService(ControlService &control);

    void start_task(int core_id);

private:
    static void task_trampoline(void *arg);
    void task_loop();

    static void keys_init();
    static bool key_pressed(gpio_num_t pin);

    ControlService &control_;
};
