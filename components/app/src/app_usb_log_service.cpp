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
    int last_phase = -1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!msc_is_mounted()) {
            header_written = false;
            gui_app_update_usb(false);
            continue;
        }

        control_state_t st = control_.snapshot();
        gui_app_update_usb(true);

        // Also capture the final stop and the start-preparation transition.
        const bool changed = last_phase != (int)st.phase;
        last_phase = (int)st.phase;
        if (!st.running && !changed) continue;

        if (!header_written) {
            const char *hdr = "timestamp_ms,mode,motor_running,motor_rpm,sensor_value,target_count,mileage_now,mileage_delta,rotation_units,feedback_valid,phase,angle_raw,origin_mileage,origin_angle,origin_units\n";
            (void)msc_write_file("logs/motor_log.csv", hdr, strlen(hdr), false);
            header_written = true;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        char line[256];
        int n = snprintf(line, sizeof(line), "%lld,%s,%d,%d,%ld,%d,%ld,%ld,%lld,%d,%u,%u,%ld,%u,%lld\n",
                         (long long)now_ms,
                         st.mode == MODE_COUNT ? "COUNT" : "FREE",
                         st.running,
                         (int)st.target_rpm,
                         (long)st.sensor_value,
                         (int)st.target_count,
                         (long)st.mileage_now,
                         (long)(st.pos_fixed_start != INT64_MIN
                                ? (st.pos_fixed_now - st.pos_fixed_start) / 32768
                                : (st.mileage_start >= 0 ? st.mileage_now - st.mileage_start : 0)),
                         (long long)(st.pos_fixed_start != INT64_MIN ? st.pos_fixed_now - st.pos_fixed_start : 0),
                         st.feedback_valid, (unsigned)st.phase, (unsigned)st.motor_angle,
                         (long)st.mileage_start, (unsigned)st.motor_angle_start,
                         (long long)st.pos_fixed_start);
        if (n > 0 && n < (int)sizeof(line)) {
            (void)msc_write_file("logs/motor_log.csv", line, (size_t)n, true);
        }
    }
}
