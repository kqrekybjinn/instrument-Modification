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
#define MOTOR_CMD_SWITCH     0xA0   // Mode switch command
#define MOTOR_CMD_DRIVE      0x64   // Drive command (speed/current/position setpoint)
#define MOTOR_CMD_QUERY      0x74   // Query: mileage rotations + current angle

// Response codes (motor → host)
#define MOTOR_RESP_DRIVE     0x65   // Drive command response
#define MOTOR_RESP_QUERY     0x75   // Query response
#define MOTOR_RESP_SWITCH    0xA1   // Mode switch response

// DATA[2] values for MOTOR_CMD_SWITCH
#define MOTOR_MODE_VAL_OPEN_LOOP 0x00
#define MOTOR_MODE_VAL_CURRENT   0x01
#define MOTOR_MODE_VAL_SPEED     0x02
#define MOTOR_MODE_VAL_POSITION  0x03
#define MOTOR_MODE_VAL_ENABLE    0x08
#define MOTOR_MODE_VAL_DISABLE   0x09

typedef enum {
    MOTOR_MODE_MANUAL = 0,
    MOTOR_MODE_AUTO,
} motor_mode_t;

typedef struct {
    // Speed value in protocol units: rpm10 (0.1 rpm/unit)
    // Example: 30.0 rpm -> rpm10=300 (0x012C); reverse: rpm10=-300 (0xFED4)
    int16_t rpm10;
} motor_drive_cmd_t;

// Response from 0x64 drive command (0x65 frame)
typedef struct {
    int16_t  actual_rpm10;   // actual speed, 0.1 rpm/unit (signed; negative = reverse)
    int16_t  actual_cur10;   // actual current (raw int16)
    uint8_t  temperature;    // winding temperature in °C
    uint8_t  fault_code;     // bit field: same as query response
} motor_drive_resp_t;

// Response from 0x74 query (0x75 frame)
typedef struct {
    int32_t  mileage_rotations; // cumulative rotation count, signed int32, big-endian
                                // positive = forward, resets to 0 on power-on
                                // range: -2,147,483,647 ~ +2,147,483,647
    uint16_t position_raw;      // current angle: 0~32767 maps to 0°~360°
    uint8_t  fault_code;        // bit field: see motor protocol doc
} motor_mileage_resp_t;

// CRC-8/MAXIM helper exposed for unit tests and logging
uint8_t crc8_maxim(const uint8_t *data, size_t len);

// Driver bring-up
esp_err_t motor_driver_init(void);

// Mode control
esp_err_t motor_enable(uint8_t motor_id);
esp_err_t motor_set_speed_mode(uint8_t motor_id);
esp_err_t motor_set_position_mode(uint8_t motor_id);

// Drive
esp_err_t motor_drive_speed(uint8_t motor_id, motor_drive_cmd_t drive);
esp_err_t motor_drive_position(uint8_t motor_id, int16_t pos_01deg);
esp_err_t motor_brake(uint8_t motor_id);

// Drive at speed and capture the 0x65 feedback response (actual speed, current, temp).
// Blocks up to ~15ms waiting for motor response. Pass resp=NULL to skip reading response.
esp_err_t motor_drive_speed_fb(uint8_t motor_id, motor_drive_cmd_t drive, motor_drive_resp_t *resp);

// Feedback: send 0x74, read 0x75 response with mileage + angle
// Blocks up to ~15ms waiting for motor response.
// Returns ESP_OK on success; ESP_ERR_TIMEOUT if no response.
esp_err_t motor_query_mileage(uint8_t motor_id, motor_mileage_resp_t *resp);
