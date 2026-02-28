#include "bsp_touch_ft6336u.h"

#include <stdbool.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "BSP_TOUCH";

// ---- Kconfig defaults (in case touch menu not included) ----
#ifndef CONFIG_TOUCH_I2C_PORT
#define CONFIG_TOUCH_I2C_PORT 0
#endif
#ifndef CONFIG_TOUCH_I2C_SDA
#define CONFIG_TOUCH_I2C_SDA 1
#endif
#ifndef CONFIG_TOUCH_I2C_SCL
#define CONFIG_TOUCH_I2C_SCL 2
#endif
#ifndef CONFIG_TOUCH_I2C_INT
#define CONFIG_TOUCH_I2C_INT -1
#endif
#ifndef CONFIG_TOUCH_I2C_RST
#define CONFIG_TOUCH_I2C_RST -1
#endif
#ifndef CONFIG_TOUCH_I2C_ADDR
#define CONFIG_TOUCH_I2C_ADDR 0x38
#endif
#ifndef CONFIG_TOUCH_I2C_FREQ_HZ
#define CONFIG_TOUCH_I2C_FREQ_HZ 400000
#endif
#ifndef CONFIG_TOUCH_SWAP_XY
#define CONFIG_TOUCH_SWAP_XY 0
#endif
#ifndef CONFIG_TOUCH_MIRROR_X
#define CONFIG_TOUCH_MIRROR_X 0
#endif
#ifndef CONFIG_TOUCH_MIRROR_Y
#define CONFIG_TOUCH_MIRROR_Y 0
#endif

// ---- FT6x36/FT6336U registers (common subset) ----
#define FT6X36_REG_DEV_MODE      0x00
#define FT6X36_REG_TD_STATUS     0x02
#define FT6X36_REG_TOUCH1_XH     0x03
#define FT6X36_REG_ID_G_CHIPID   0xA3
#define FT6X36_REG_ID_G_VENDID   0xA8
#define FT6X36_REG_ID_G_FIRMID   0xA6

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_dev;

static uint16_t s_x_max;
static uint16_t s_y_max;
static lv_point_t s_last_point;

typedef struct {
    int found;
    uint8_t first_addr;
    bool found_0x38;
    bool found_0x3A;
} i2c_scan_result_t;

static i2c_scan_result_t i2c_scan_and_log(i2c_master_bus_handle_t bus_handle)
{
    // Typical usable 7-bit range: 0x08..0x77
    i2c_scan_result_t r = {
        .found = 0,
        .first_addr = 0,
        .found_0x38 = false,
        .found_0x3A = false,
    };

    ESP_LOGI(TAG, "I2C scan start (0x08..0x77)");
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(bus_handle, addr, 20);
        if (err == ESP_OK) {
            r.found++;
            if (r.found == 1) {
                r.first_addr = (uint8_t)addr;
            }
            if (addr == 0x38) {
                r.found_0x38 = true;
            } else if (addr == 0x3A) {
                r.found_0x3A = true;
            }
            ESP_LOGI(TAG, "I2C device found @ 0x%02X", (unsigned)addr);
        } else if (err == ESP_ERR_TIMEOUT) {
            // Timeout usually means bus stuck / missing pullups; continuing can add a lot of delays.
            ESP_LOGW(TAG, "I2C probe timeout at 0x%02X; check SDA/SCL pull-ups and wiring", (unsigned)addr);
            break;
        }
    }

    if (r.found == 0) {
        ESP_LOGW(TAG, "I2C scan: no devices found");
    } else {
        ESP_LOGI(TAG,
                 "I2C scan done: %d device(s) found (touch candidates: 0x38=%d 0x3A=%d)",
                 r.found,
                 r.found_0x38 ? 1 : 0,
                 r.found_0x3A ? 1 : 0);
    }

    return r;
}

static esp_err_t ft6x36_read(uint8_t reg, uint8_t *out, size_t out_len)
{
    if (!s_i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_i2c_dev, &reg, 1, out, out_len, 50);
}

static esp_err_t ft6x36_write(uint8_t reg, uint8_t val)
{
    if (!s_i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), 50);
}

static void apply_transform(uint16_t *x, uint16_t *y)
{
    uint16_t xx = *x;
    uint16_t yy = *y;

#if CONFIG_TOUCH_SWAP_XY
    uint16_t t = xx;
    xx = yy;
    yy = t;
#endif

#if CONFIG_TOUCH_MIRROR_X
    if (s_x_max > 0) {
        xx = (uint16_t)(s_x_max - 1 - xx);
    }
#endif

#if CONFIG_TOUCH_MIRROR_Y
    if (s_y_max > 0) {
        yy = (uint16_t)(s_y_max - 1 - yy);
    }
#endif

    if (s_x_max > 0 && xx >= s_x_max) {
        xx = (uint16_t)(s_x_max - 1);
    }
    if (s_y_max > 0 && yy >= s_y_max) {
        yy = (uint16_t)(s_y_max - 1);
    }

    *x = xx;
    *y = yy;
}

esp_err_t bsp_touch_init(uint16_t x_max, uint16_t y_max)
{
    s_x_max = x_max;
    s_y_max = y_max;
    s_last_point.x = 0;
    s_last_point.y = 0;

    // Optional INT pin (compile-time guarded to avoid BIT64(-1) warnings)
#if CONFIG_TOUCH_I2C_INT >= 0
    {
        gpio_config_t int_cfg = {
            .pin_bit_mask = BIT64(CONFIG_TOUCH_I2C_INT),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "int gpio_config");
    }
#endif

    // Optional RST pin (compile-time guarded to avoid BIT64(-1) warnings)
#if CONFIG_TOUCH_I2C_RST >= 0
    {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = BIT64(CONFIG_TOUCH_I2C_RST),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "rst gpio_config");
        // Active low reset is typical for FT6x36
        gpio_set_level(CONFIG_TOUCH_I2C_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(CONFIG_TOUCH_I2C_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
#endif

    if (!s_i2c_bus) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = CONFIG_TOUCH_I2C_PORT,
            .sda_io_num = CONFIG_TOUCH_I2C_SDA,
            .scl_io_num = CONFIG_TOUCH_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c_new_master_bus");

        // Scan once right after bus creation so wiring/pullups issues show up early.
        i2c_scan_result_t scan = i2c_scan_and_log(s_i2c_bus);
        if (scan.found == 0) {
            ESP_LOGE(TAG,
                     "Touch init aborted: I2C bus has no devices. Check wiring/power and external pull-ups (SDA/SCL ~4.7k-10k to 3.3V).");
            return ESP_ERR_NOT_FOUND;
        }
    }

    if (!s_i2c_dev) {
        // Prefer configured address, but fall back to common FT6x36 addresses.
        uint16_t addr = CONFIG_TOUCH_I2C_ADDR;
        const bool cfg_found = (addr >= 0x08 && addr <= 0x77) ? (i2c_master_probe(s_i2c_bus, addr, 20) == ESP_OK) : false;

        if (!cfg_found) {
            if (i2c_master_probe(s_i2c_bus, 0x38, 20) == ESP_OK) {
                addr = 0x38;
            } else if (i2c_master_probe(s_i2c_bus, 0x3A, 20) == ESP_OK) {
                addr = 0x3A;
            }

            if (addr != CONFIG_TOUCH_I2C_ADDR) {
                ESP_LOGW(TAG, "Configured touch addr 0x%02X not responding; using detected candidate 0x%02X", (unsigned)CONFIG_TOUCH_I2C_ADDR, (unsigned)addr);
            }
        }

        // If neither configured nor 0x38/0x3A respond, fail early to avoid NACK spam.
        if (!cfg_found && i2c_master_probe(s_i2c_bus, addr, 20) != ESP_OK) {
            ESP_LOGE(TAG,
                     "No touch controller ACK at 0x%02X (and no 0x38/0x3A detected). Likely wrong SDA/SCL pins, no pull-ups, or touch IC not powered.",
                     (unsigned)addr);
            return ESP_ERR_NOT_FOUND;
        }

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = CONFIG_TOUCH_I2C_FREQ_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev), TAG, "i2c_bus_add_device");
    }

    // Basic probe: read chip/vendor/firmware IDs (may return 0x00/0xFF depending on variant)
    uint8_t chip = 0, vend = 0, firm = 0;
    esp_err_t err_chip = ft6x36_read(FT6X36_REG_ID_G_CHIPID, &chip, 1);
    esp_err_t err_vend = ft6x36_read(FT6X36_REG_ID_G_VENDID, &vend, 1);
    esp_err_t err_firm = ft6x36_read(FT6X36_REG_ID_G_FIRMID, &firm, 1);

    ESP_LOGI(TAG,
             "Touch init: I2C port=%d SDA=%d SCL=%d addr=0x%02X freq=%u INT=%d RST=%d",
             CONFIG_TOUCH_I2C_PORT,
             CONFIG_TOUCH_I2C_SDA,
             CONFIG_TOUCH_I2C_SCL,
             CONFIG_TOUCH_I2C_ADDR,
             (unsigned)CONFIG_TOUCH_I2C_FREQ_HZ,
             CONFIG_TOUCH_I2C_INT,
             CONFIG_TOUCH_I2C_RST);

    ESP_LOGI(TAG,
             "FT6x36 IDs: chip=0x%02X(%s) vend=0x%02X(%s) firm=0x%02X(%s)",
             chip, esp_err_to_name(err_chip),
             vend, esp_err_to_name(err_vend),
             firm, esp_err_to_name(err_firm));

    // Ensure device mode is normal (best-effort)
    (void)ft6x36_write(FT6X36_REG_DEV_MODE, 0x00);

    // Quick sanity read of TD_STATUS
    uint8_t td = 0;
    ESP_RETURN_ON_ERROR(ft6x36_read(FT6X36_REG_TD_STATUS, &td, 1), TAG, "read TD_STATUS");

    ESP_LOGI(TAG, "FT6x36 TD_STATUS=0x%02X (x_max=%u y_max=%u, swap_xy=%d mirror_x=%d mirror_y=%d)",
             td, (unsigned)s_x_max, (unsigned)s_y_max,
             CONFIG_TOUCH_SWAP_XY, CONFIG_TOUCH_MIRROR_X, CONFIG_TOUCH_MIRROR_Y);

    return ESP_OK;
}

void bsp_touch_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;

    static int64_t last_err_log_us;

    // Read TD_STATUS + first touch point
    uint8_t buf[7] = {0};
    esp_err_t err = ft6x36_read(FT6X36_REG_TD_STATUS, buf, sizeof(buf));
    if (err != ESP_OK) {
        int64_t now = esp_timer_get_time();
        if (now - last_err_log_us > 1000 * 1000) {
            last_err_log_us = now;
            ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(err));
        }
        data->state = LV_INDEV_STATE_REL;
        data->point = s_last_point;
        return;
    }

    uint8_t touch_points = (uint8_t)(buf[0] & 0x0F);
    if (touch_points == 0) {
        data->state = LV_INDEV_STATE_REL;
        data->point = s_last_point;
        return;
    }

    const uint8_t xh = buf[1];
    const uint8_t xl = buf[2];
    const uint8_t yh = buf[3];
    const uint8_t yl = buf[4];

    uint16_t x = (uint16_t)(((xh & 0x0F) << 8) | xl);
    uint16_t y = (uint16_t)(((yh & 0x0F) << 8) | yl);

    apply_transform(&x, &y);

    s_last_point.x = (lv_coord_t)x;
    s_last_point.y = (lv_coord_t)y;

    data->state = LV_INDEV_STATE_PR;
    data->point = s_last_point;
}
