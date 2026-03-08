#include "app_usb_log_service.hpp"

#include <stdio.h>
#include <string.h>

#include "freertos/task.h"

#include "esp_timer.h"

extern "C" {
#include "gui_app.h"
#include "msc_driver.h"
}

UsbLogService::UsbLogService(ControlService &control) : control_(control) {}

void UsbLogService::start_task(int core_id)
{
    xTaskCreatePinnedToCore(task_trampoline, "usb_log", 6144, this, 4, NULL, core_id);
}

void UsbLogService::task_trampoline(void *arg)
{
    static_cast<UsbLogService *>(arg)->task_loop();
}

void UsbLogService::task_loop()
{
    bool header_written = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!msc_is_mounted()) {
            header_written = false;
            gui_app_update_usb(false);
            continue;
        }

        control_state_t st = control_.snapshot();
        gui_app_update_usb(true);

        if (!st.running) {
            continue;
        }

        if (!header_written) {
            const char *hdr = "timestamp_ms,mode,motor_running,motor_rpm,sensor_value,target_count,mileage_now,mileage_delta\n";
            (void)msc_write_file("logs/motor_log.csv", hdr, strlen(hdr), false);
            header_written = true;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        char line[160];
        int n = snprintf(line, sizeof(line), "%lld,%s,%d,%d,%ld,%d,%ld,%ld\n",
                         (long long)now_ms,
                         st.mode == MODE_COUNT ? "COUNT" : "TIMER",
                         st.running,
                         (int)st.target_rpm,
                         (long)st.sensor_value,
                         (int)st.target_count,
                         (long)st.mileage_now,
                         (long)(st.mileage_start >= 0 ? st.mileage_now - st.mileage_start : 0));
        if (n > 0 && n < (int)sizeof(line)) {
            (void)msc_write_file("logs/motor_log.csv", line, (size_t)n, true);
        }
    }
}
