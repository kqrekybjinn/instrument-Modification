#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

// UART1 wiring for the M0603A motor
#define MOTOR_UART_NUM       UART_NUM_1
#define MOTOR_TX_PIN         GPIO_NUM_13
#define MOTOR_RX_PIN         GPIO_NUM_14
#define MOTOR_UART_BAUD      38400

// Protocol constants
#define MOTOR_DEFAULT_ID     0x01
#define MOTOR_FRAME_LEN      10
#define MOTOR_CMD_SWITCH     0xA0
#define MOTOR_CMD_DRIVE      0x64

typedef enum {
	MOTOR_MODE_MANUAL = 0,
	MOTOR_MODE_AUTO,
} motor_mode_t;

typedef struct {
	// Speed value in protocol units: rpm10 (0.1rpm)
	// Example: 30.0rpm -> rpm10=300 (0x012C)
	int16_t rpm10;
} motor_drive_cmd_t;

// CRC-8/MAXIM helper exposed for unit tests and logging
uint8_t crc8_maxim(const uint8_t *data, size_t len);

// Driver bring-up
esp_err_t motor_driver_init(void);

// High level protocol helpers (all return ESP_OK on successful queue to UART)
esp_err_t motor_enable(uint8_t motor_id);
esp_err_t motor_set_speed_mode(uint8_t motor_id);
esp_err_t motor_drive_speed(uint8_t motor_id, motor_drive_cmd_t drive);
esp_err_t motor_brake(uint8_t motor_id);
