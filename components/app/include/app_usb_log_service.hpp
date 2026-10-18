#pragma once

#include "app_control_service.hpp"

class UsbLogService {
public:
    explicit UsbLogService(ControlService &control);
    void start_task(int core_id);

private:
#ifdef MODI_HOST_TEST
    friend struct UsbLogServiceTest;
#endif
    void sample_once();
    bool header_written_ = false;
    int last_phase_ = -1;
    int64_t last_error_us_ = -1000000;
    void report_error(const char *operation, esp_err_t error);
    static void task_trampoline(void *arg);
    void task_loop();

    ControlService &control_;
};
