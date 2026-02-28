#include "motor_driver.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "MOTOR_DRIVER";

#ifndef CONFIG_MOTOR_UART_TX_DUMP
#define CONFIG_MOTOR_UART_TX_DUMP 1
#endif

#define FRAME_DATA_LEN 9

uint8_t crc8_maxim(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x01) {
                crc = (crc >> 1) ^ 0x8C; // 0x8C is reflected 0x31
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static esp_err_t motor_uart_write(uint8_t *frame)
{
    frame[9] = crc8_maxim(frame, FRAME_DATA_LEN);

#if CONFIG_MOTOR_UART_TX_DUMP
    ESP_LOGI(TAG, "UART%d TX frame (len=%d)", (int)MOTOR_UART_NUM, (int)MOTOR_FRAME_LEN);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, MOTOR_FRAME_LEN, ESP_LOG_INFO);
#endif

    int written = uart_write_bytes(MOTOR_UART_NUM, frame, MOTOR_FRAME_LEN);
    ESP_RETURN_ON_FALSE(written == MOTOR_FRAME_LEN, ESP_FAIL, TAG, "uart_write_bytes short (%d)", written);
    return ESP_OK;
}

esp_err_t motor_driver_init(void)
{
    uart_config_t cfg = {
        .baud_rate = MOTOR_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(MOTOR_UART_NUM, &cfg), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(MOTOR_UART_NUM, MOTOR_TX_PIN, MOTOR_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(MOTOR_UART_NUM, 256, 0, 0, NULL, 0), TAG, "uart_driver_install failed");

    ESP_LOGI(TAG, "UART%d init @ %d bps (TX=%d RX=%d)", MOTOR_UART_NUM, MOTOR_UART_BAUD, MOTOR_TX_PIN, MOTOR_RX_PIN);
    return ESP_OK;
}

esp_err_t motor_enable(uint8_t motor_id)
{
    uint8_t frame[MOTOR_FRAME_LEN] = {motor_id, MOTOR_CMD_SWITCH, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    esp_err_t err = motor_uart_write(frame);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Motor 0x%02X enable", motor_id);
    }
    return err;
}

esp_err_t motor_set_speed_mode(uint8_t motor_id)
{
    uint8_t frame[MOTOR_FRAME_LEN] = {motor_id, MOTOR_CMD_SWITCH, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    esp_err_t err = motor_uart_write(frame);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Motor 0x%02X -> speed mode", motor_id);
    }
    return err;
}

esp_err_t motor_drive_speed(uint8_t motor_id, motor_drive_cmd_t drive)
{
    uint8_t data_h = (drive.rpm10 >> 8) & 0xFF;
    uint8_t data_l = drive.rpm10 & 0xFF;

    uint8_t frame[MOTOR_FRAME_LEN] = {
        motor_id,
        MOTOR_CMD_DRIVE,
        data_h,
        data_l,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    };

    esp_err_t err = motor_uart_write(frame);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Motor 0x%02X drive rpm10=%d", motor_id, (int)drive.rpm10);
    }
    return err;
}

esp_err_t motor_brake(uint8_t motor_id)
{
    motor_drive_cmd_t stop = {
        .rpm10 = 0,
    };
    return motor_drive_speed(motor_id, stop);
}
