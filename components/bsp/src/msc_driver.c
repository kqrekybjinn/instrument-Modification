#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "msc_driver.h"

static const char *TAG = "MSC_DRIVER";
#define MNT_PATH "/usb"

static bool s_is_mounted = false;
static msc_host_device_handle_t s_msc_device = NULL;
static msc_host_vfs_handle_t s_vfs_handle = NULL;
static SemaphoreHandle_t s_io_lock = NULL;

typedef enum {
    MSC_MSG_DEVICE_CONNECTED = 1,
    MSC_MSG_DEVICE_DISCONNECTED = 2,
} msc_msg_id_t;

typedef struct {
    msc_msg_id_t id;
    uint8_t address;                 // valid for CONNECTED
    msc_host_device_handle_t handle; // valid for DISCONNECTED
} msc_msg_t;

static QueueHandle_t s_msc_queue = NULL;

static esp_err_t msc_build_abs_path(const char *rel_path, char *out, size_t out_size)
{
    if (!rel_path || rel_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (rel_path[0] == '/' || strstr(rel_path, "..")) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, out_size, "%s/%s", MNT_PATH, rel_path);
    if (n <= 0 || (size_t)n >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t msc_ensure_dir_for_file(const char *abs_path)
{
    // Create intermediate directories if needed
    char tmp[256];
    size_t len = strlcpy(tmp, abs_path, sizeof(tmp));
    if (len >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return ESP_OK;
}

bool msc_is_mounted(void)
{
    return s_is_mounted;
}

const char *msc_mount_path(void)
{
    return s_is_mounted ? MNT_PATH : NULL;
}

static void msc_cleanup(void)
{
    if (s_io_lock) {
        (void)xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(200));
    }
    if (s_vfs_handle) {
        esp_err_t err = msc_host_vfs_unregister(s_vfs_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "msc_host_vfs_unregister failed: %s", esp_err_to_name(err));
        }
        s_vfs_handle = NULL;
    }
    if (s_msc_device) {
        esp_err_t err = msc_host_uninstall_device(s_msc_device);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "msc_host_uninstall_device failed: %s", esp_err_to_name(err));
        }
        s_msc_device = NULL;
    }
    s_is_mounted = false;
    if (s_io_lock) {
        xSemaphoreGive(s_io_lock);
    }
}

esp_err_t msc_write_file(const char *rel_path, const void *data, size_t len, bool append)
{
    if (!s_is_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_io_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    char abs_path[256];
    esp_err_t err = msc_build_abs_path(rel_path, abs_path, sizeof(abs_path));
    if (err != ESP_OK) {
        xSemaphoreGive(s_io_lock);
        return err;
    }

    msc_ensure_dir_for_file(abs_path);

    FILE *f = fopen(abs_path, append ? "ab" : "wb");
    if (!f) {
        err = ESP_FAIL;
        goto out;
    }
    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        err = ESP_FAIL;
    } else {
        fflush(f);
        fsync(fileno(f));
        err = ESP_OK;
    }
    fclose(f);
out:
    xSemaphoreGive(s_io_lock);
    return err;
}

esp_err_t msc_read_file(const char *rel_path, void *out, size_t out_size, size_t *out_len)
{
    if (!s_is_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_io_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    char abs_path[256];
    esp_err_t err = msc_build_abs_path(rel_path, abs_path, sizeof(abs_path));
    if (err != ESP_OK) {
        xSemaphoreGive(s_io_lock);
        return err;
    }

    FILE *f = fopen(abs_path, "rb");
    if (!f) {
        xSemaphoreGive(s_io_lock);
        return ESP_FAIL;
    }

    size_t total = fread(out, 1, out_size, f);
    if (out_len) {
        *out_len = total;
    }
    fclose(f);
    xSemaphoreGive(s_io_lock);
    return ESP_OK;
}

static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (!s_msc_queue || !event) {
        return;
    }

    msc_msg_t msg = {0};
    if (event->event == MSC_DEVICE_CONNECTED) {
        msg.id = MSC_MSG_DEVICE_CONNECTED;
        msg.address = event->device.address;
        (void)xQueueSend(s_msc_queue, &msg, 0);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        msg.id = MSC_MSG_DEVICE_DISCONNECTED;
        msg.handle = event->device.handle;
        (void)xQueueSend(s_msc_queue, &msg, 0);
    }
}

void msc_driver_task(void *arg)
{
    ESP_LOGI(TAG, "Starting MSC Driver Task");

    if (!s_msc_queue) {
        s_msc_queue = xQueueCreate(8, sizeof(msc_msg_t));
        if (!s_msc_queue) {
            ESP_LOGE(TAG, "Failed to create MSC queue");
            vTaskDelete(NULL);
            return;
        }
    }

    if (!s_io_lock) {
        s_io_lock = xSemaphoreCreateMutex();
        if (!s_io_lock) {
            ESP_LOGE(TAG, "Failed to create MSC IO mutex");
            vTaskDelete(NULL);
            return;
        }
    }

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 6144,
        .callback = msc_event_cb,
        .callback_arg = NULL,
    };

    ESP_ERROR_CHECK(msc_host_install(&msc_config));
    ESP_LOGI(TAG, "MSC Host installed");

    while (1) {
        msc_msg_t msg;
        if (xQueueReceive(s_msc_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.id == MSC_MSG_DEVICE_CONNECTED) {
            ESP_LOGI(TAG, "MSC device connected (addr=%u)", (unsigned)msg.address);

            msc_cleanup();

            esp_err_t err = msc_host_install_device(msg.address, &s_msc_device);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
                msc_cleanup();
                continue;
            }

            const esp_vfs_fat_mount_config_t mount_config = {
                .format_if_mount_failed = false,
                .max_files = 5,
                .allocation_unit_size = 4096,
            };

            err = msc_host_vfs_register(s_msc_device, MNT_PATH, &mount_config, &s_vfs_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
                msc_cleanup();
                continue;
            }

            s_is_mounted = true;
            ESP_LOGI(TAG, "MSC mounted at %s", MNT_PATH);

        } else if (msg.id == MSC_MSG_DEVICE_DISCONNECTED) {
            ESP_LOGW(TAG, "MSC device disconnected");
            msc_cleanup();
        }
    }
}
