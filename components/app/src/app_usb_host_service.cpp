#include "app_usb_host_service.hpp"

#include "freertos/task.h"

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"

#include "usb/usb_host.h"

static const char *TAG = "APP";

void UsbHostService::init()
{
    ready_sem_ = xSemaphoreCreateBinary();
}

void UsbHostService::start_task(int core_id)
{
    xTaskCreatePinnedToCore(task_trampoline, "usb_host", 6144, this, 6, NULL, core_id);
}

bool UsbHostService::wait_ready(TickType_t ticks)
{
    if (!ready_sem_) {
        return false;
    }
    return xSemaphoreTake(ready_sem_, ticks) == pdTRUE;
}

void UsbHostService::task_trampoline(void *arg)
{
    static_cast<UsbHostService *>(arg)->task_loop();
}

void UsbHostService::task_loop()
{
    usb_host_config_t host_config{};
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;

    ESP_LOGI(TAG, "USB Host installing...");
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");

    if (ready_sem_) {
        xSemaphoreGive(ready_sem_);
    }

    while (1) {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events err=0x%x", err);
        }
    }
}
