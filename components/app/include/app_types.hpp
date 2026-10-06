#pragma once
#include "workflow.hpp"
extern "C" {
#include "motor_driver.h"
}
#define DEFAULT_FORWARD_RPM 100
#define KEY_SCAN_MS 10
#define SMOOTH_SLOW_RPM 30
#define MIN_TARGET_COUNT 1
#define MAX_TARGET_COUNT 9999
typedef enum { MODE_FREE = 0, MODE_COUNT = 1 } operating_mode_t;
typedef enum {
 CMD_STATE_TOGGLE = 0, CMD_STOP, CMD_NEW_RUN, CMD_RETURN, CMD_SET_SPEED,
 CMD_MODE_SWITCH, CMD_FORWARD_DIR_TOGGLE, CMD_SET_COUNT, CMD_COUNT_INC,
 CMD_COUNT_DEC, CMD_COUNT_STEP_TOGGLE
} control_cmd_id_t;
typedef struct { control_cmd_id_t id; int16_t value; } control_cmd_t;
using control_state_t = workflow::State;
