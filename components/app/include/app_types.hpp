#pragma once

#include <stdbool.h>
#include <stdint.h>

// motor_driver.h is implemented in C in this project; wrap it to keep C linkage
extern "C" {
#include "motor_driver.h"
}

// UI / control parameters
#define DEFAULT_FORWARD_RPM 60
#define REVERSE_FALLBACK_MS 1500
#define KEY_SCAN_MS 20

#define SPEED_LEVEL_0_RPM 30
#define SPEED_LEVEL_1_RPM 60
#define SPEED_LEVEL_2_RPM 90

typedef enum {
    // One key cycles motion state: forward -> stop -> reverse -> (auto stop) -> forward...
    CMD_STATE_TOGGLE = 0,
    // One key cycles speed level (only when stopped)
    CMD_SPEED_STEP,
} control_cmd_id_t;

typedef struct {
    control_cmd_id_t id;
} control_cmd_t;

typedef struct {
    bool running;
    bool reversing;
    int16_t target_rpm;
    int64_t run_start_us;
    int64_t last_forward_us;
    int16_t last_forward_rpm;
    int64_t reverse_target_us;
    int32_t sensor_value;
} control_state_t;
