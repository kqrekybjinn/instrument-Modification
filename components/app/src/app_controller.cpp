#include "app_controller.hpp"

#include "app_control_service.hpp"
#include "app_gui_service.hpp"
#include "app_input_service.hpp"
#include "app_sensor_service.hpp"
#include "app_usb_host_service.hpp"
#include "app_usb_log_service.hpp"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

extern "C" {
#include "msc_driver.h"
#include "tf_card.h"
#include "gui_app.h"
}

static const char *TAG = "APP";
static ControlService *s_control = nullptr;

static void storage_status_task(void *arg)
{
    (void)arg;

    TickType_t last_tf_try = 0;
    TickType_t tf_retry_interval = pdMS_TO_TICKS(5000);
    bool last_usb = false;
    bool last_tf = false;

    while (1) {
        const bool usb = msc_is_mounted();
        if (usb != last_usb) {
            gui_app_update_usb(usb);
            last_usb = usb;
        }

        bool tf = tf_card_is_mounted();
        if (!tf) {
            const TickType_t now = xTaskGetTickCount();
            if (last_tf_try == 0 || (now - last_tf_try) > tf_retry_interval) {
                last_tf_try = now;
                esp_err_t err = tf_card_mount();
                tf = (err == ESP_OK);
                // If the card isn't present / wiring is wrong, back off to reduce log spam.
                tf_retry_interval = tf ? pdMS_TO_TICKS(5000) : pdMS_TO_TICKS(30000);
            }
        }

        if (tf != last_tf) {
            gui_app_update_tf(tf);
            last_tf = tf;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void storage_debug_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Storage debug: start");

    // ---- TF card (SDMMC) ----
    esp_err_t err = tf_card_mount();
    if (err == ESP_OK) {
        gui_app_update_tf(true);
        const char *payload = "tf_card_write_test\n";
        err = tf_card_write_file("debug/tf_test.txt", payload, strlen(payload), false);
        ESP_LOGI(TAG, "TF write: %s", esp_err_to_name(err));

        char buf[64] = {0};
        size_t out_len = 0;
        err = tf_card_read_file("debug/tf_test.txt", buf, sizeof(buf) - 1, &out_len);
        ESP_LOGI(TAG, "TF read: %s (len=%u, data='%s')", esp_err_to_name(err), (unsigned)out_len, buf);
    } else {
        gui_app_update_tf(false);
        ESP_LOGE(TAG, "TF mount failed: %s", esp_err_to_name(err));
    }

    // ---- USB MSC (U盘) ----
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
    while (!msc_is_mounted() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (msc_is_mounted()) {
        const char *payload = "usb_msc_write_test\n";
        err = msc_write_file("debug/usb_test.txt", payload, strlen(payload), false);
        ESP_LOGI(TAG, "USB write: %s", esp_err_to_name(err));

        char buf[64] = {0};
        size_t out_len = 0;
        err = msc_read_file("debug/usb_test.txt", buf, sizeof(buf) - 1, &out_len);
        ESP_LOGI(TAG, "USB read: %s (len=%u, data='%s')", esp_err_to_name(err), (unsigned)out_len, buf);
    } else {
        ESP_LOGW(TAG, "USB not mounted within timeout; insert U盘 and retry");
    }

    ESP_LOGI(TAG, "Storage debug: done");
    vTaskDelete(NULL);
}

static void gui_event_bridge(gui_event_t evt)
{
    if (!s_control) {
        return;
    }

    switch (evt) {
    case GUI_EVENT_SPEED_STEP:
        if (s_control->snapshot().mode == MODE_COUNT) {
            s_control->push_cmd(CMD_COUNT_ABORT);
        } else {
            s_control->push_cmd(CMD_SPEED_STEP);
        }
        break;
    case GUI_EVENT_STATE_TOGGLE:
        s_control->push_cmd(CMD_STATE_TOGGLE);
        break;
    case GUI_EVENT_MODE_SWITCH:
        s_control->push_cmd(CMD_MODE_SWITCH);
        break;
    case GUI_EVENT_COUNT_INC:
        s_control->push_cmd(CMD_COUNT_INC);
        break;
    case GUI_EVENT_COUNT_DEC:
        s_control->push_cmd(CMD_COUNT_DEC);
        break;
    case GUI_EVENT_COUNT_STEP_TOGGLE:
        s_control->push_cmd(CMD_COUNT_STEP_TOGGLE);
        break;
    case GUI_EVENT_FORWARD_DIR_TOGGLE:
        s_control->push_cmd(CMD_FORWARD_DIR_TOGGLE);
        break;
    default:
        break;
    }
}

void AppController::start()
{
    static ControlService control;
    static UsbHostService usb_host;

    s_control = &control;

    static GuiService gui(&gui_event_bridge);
    static InputService input(control);
    static SensorService sensor(control);
    static UsbLogService usb_log(control);

    control.init();
    usb_host.init();

    // Keep task core placement identical to previous version
    gui.start_task(1);
    control.start_task(0);
    input.start_task(0);
    sensor.start_task(0);
    usb_log.start_task(0);
    usb_host.start_task(0);

    // Periodically refresh storage indicators (independent of USB ready)
    xTaskCreatePinnedToCore(storage_status_task, "storage_status", 4096, NULL, 3, NULL, 0);

    // Wait until usb_host_install() is done, then register/start class driver
    if (usb_host.wait_ready(pdMS_TO_TICKS(2000))) {
        xTaskCreatePinnedToCore(msc_driver_task, "msc_driver", 6144, NULL, 5, NULL, 0);

        // One-shot bring-up test for TF + USB storage
        xTaskCreatePinnedToCore(storage_debug_task, "storage_dbg", 4096, NULL, 5, NULL, 0);
    } else {
        ESP_LOGE(TAG, "USB host init timeout, MSC skipped");
    }
}
