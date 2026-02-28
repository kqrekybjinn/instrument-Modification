#pragma once

#include "app_control_service.hpp"

class UsbLogService {
public:
    explicit UsbLogService(ControlService &control);
    void start_task(int core_id);

private:
    static void task_trampoline(void *arg);
    void task_loop();

    ControlService &control_;
};
