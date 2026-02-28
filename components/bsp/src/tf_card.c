#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "bsp_pinmap.h"
#include "tf_card.h"

static const char *TAG = "TF_CARD";
#define MNT_PATH "/sdcard"

// For bring-up/debug, keep SDMMC clock conservative.
// You can override at build time via -DTF_CARD_MAX_FREQ_KHZ=10000 (etc.).
#ifndef TF_CARD_MAX_FREQ_KHZ
#define TF_CARD_MAX_FREQ_KHZ SDMMC_FREQ_PROBING
#endif

static bool s_is_mounted = false;
static sdmmc_card_t *s_card = NULL;
static SemaphoreHandle_t s_io_lock = NULL;

static esp_err_t tf_build_abs_path(const char *rel_path, char *out, size_t out_size)
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

static esp_err_t tf_ensure_dir_for_file(const char *abs_path)
{
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

bool tf_card_is_mounted(void)
{
    return s_is_mounted;
}

const char *tf_card_mount_path(void)
{
    return s_is_mounted ? MNT_PATH : NULL;
}

esp_err_t tf_card_mount(void)
{
    if (s_is_mounted) {
        return ESP_OK;
    }

    if (!s_io_lock) {
        s_io_lock = xSemaphoreCreateMutex();
        if (!s_io_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = TF_CARD_MAX_FREQ_KHZ;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = BSP_SDMMC_CLK_GPIO;
    slot_config.cmd = BSP_SDMMC_CMD_GPIO;
    slot_config.d0 = BSP_SDMMC_D0_GPIO;
    slot_config.width = BSP_SDMMC_WIDTH;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Mounting TF card at %s (CMD=%d CLK=%d D0=%d width=%u)",
             MNT_PATH,
             (int)BSP_SDMMC_CMD_GPIO,
             (int)BSP_SDMMC_CLK_GPIO,
             (int)BSP_SDMMC_D0_GPIO,
             (unsigned)BSP_SDMMC_WIDTH);

    ESP_LOGI(TAG, "SDMMC max freq: %u kHz", (unsigned)host.max_freq_khz);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        err = esp_vfs_fat_sdmmc_mount(MNT_PATH, &host, &slot_config, &mount_config, &s_card);
        if (err == ESP_OK) {
            break;
        }

        ESP_LOGW(TAG, "esp_vfs_fat_sdmmc_mount attempt %d/3 failed: %s", attempt, esp_err_to_name(err));
        s_card = NULL;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGE(TAG,
                     "Got ESP_ERR_INVALID_RESPONSE (0x%x). Common causes: no card inserted, wrong wiring, SD socket powered off, "
                     "missing external pull-ups (CMD/D0 need ~10k to 3.3V), or the TF module is SPI-only (needs CS + SDSPI driver).",
                     (unsigned)err);
        } else {
            ESP_LOGE(TAG, "esp_vfs_fat_sdmmc_mount failed: %s", esp_err_to_name(err));
        }
        s_card = NULL;
        s_is_mounted = false;
        return err;
    }

    s_is_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "TF mounted at %s", MNT_PATH);
    return ESP_OK;
}

esp_err_t tf_card_unmount(void)
{
    if (!s_is_mounted || !s_card) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_io_lock) {
        (void)xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(200));
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(MNT_PATH, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_sdcard_unmount failed: %s", esp_err_to_name(err));
    }

    s_card = NULL;
    s_is_mounted = false;

    if (s_io_lock) {
        xSemaphoreGive(s_io_lock);
    }

    return err;
}

esp_err_t tf_card_write_file(const char *rel_path, const void *data, size_t len, bool append)
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
    esp_err_t err = tf_build_abs_path(rel_path, abs_path, sizeof(abs_path));
    if (err != ESP_OK) {
        xSemaphoreGive(s_io_lock);
        return err;
    }

    tf_ensure_dir_for_file(abs_path);

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

esp_err_t tf_card_read_file(const char *rel_path, void *out, size_t out_size, size_t *out_len)
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
    esp_err_t err = tf_build_abs_path(rel_path, abs_path, sizeof(abs_path));
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
