#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class UsbHostService {
public:
    void init();
    void start_task(int core_id);
    bool wait_ready(TickType_t ticks);

private:
    static void task_trampoline(void *arg);
    void task_loop();

    SemaphoreHandle_t ready_sem_ = nullptr;
};
