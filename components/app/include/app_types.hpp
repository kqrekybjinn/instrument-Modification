#pragma once

#include <stdbool.h>
#include <stdint.h>

// motor_driver.h is implemented in C in this project; wrap it to keep C linkage
extern "C" {
#include "motor_driver.h"
}

// UI / control parameters
#define DEFAULT_FORWARD_RPM 100
#define REVERSE_FALLBACK_MS 1500
#define KEY_SCAN_MS 20

#define SPEED_LEVEL_0_RPM  50
#define SPEED_LEVEL_1_RPM  100
#define SPEED_LEVEL_2_RPM  200
#define SPEED_LEVEL_3_RPM  300
#define SPEED_LEVEL_4_RPM  380

// Count mode parameters
#define DEFAULT_TARGET_COUNT 100
#define MIN_TARGET_COUNT     1
#define MAX_TARGET_COUNT     9999

// Count mode: smooth trapezoidal velocity profile
#define SMOOTH_RAMP_COUNT 3   // rotations for slow start/end phases
#define SMOOTH_SLOW_RPM   30  // RPM during slow phases

// Count mode: mileage feedback query interval
// The control loop polls 0x74 every ~50ms; no extra constant needed.

typedef enum {
    MODE_TIMER = 0,
    MODE_COUNT,
} operating_mode_t;

typedef enum {
    // Timer mode: start/stop/reverse cycle.
    // Count mode: pause/resume/start-forward.
    CMD_STATE_TOGGLE = 0,
    // Timer mode: cycle speed level (only when stopped)
    // Count mode: mapped to stop->reverse behavior.
    CMD_SPEED_STEP,
    // Switch between timer and count modes (only when fully idle)
    CMD_MODE_SWITCH,
    // Adjust target rotation count (only in count mode, stopped, no accumulated distance)
    CMD_COUNT_INC,
    CMD_COUNT_DEC,
    // Toggle count step size between ×1 and ×10
    CMD_COUNT_STEP_TOGGLE,
    // Emergency brake + immediately start return-to-origin (abort current run)
    CMD_COUNT_ABORT,
    // Toggle forward direction (CW/CCW)
    CMD_FORWARD_DIR_TOGGLE,
} control_cmd_id_t;

typedef struct {
    control_cmd_id_t id;
} control_cmd_t;

typedef struct {
    bool running;
    bool reversing;
    bool paused;            // count mode: motor paused mid-run (forward/reverse)
    bool forward_ccw;       // true: forward=CCW, false: forward=CW
    int16_t target_rpm;
    int64_t run_start_us;
    int64_t last_forward_us;
    int16_t last_forward_rpm;
    int64_t reverse_target_us;
    int32_t sensor_value;
    operating_mode_t mode;
    int16_t target_count;           // count mode: target rotations

    // Count mode: mileage feedback tracking (via 0x74 query)
    // mileage_start: cumulative motor rotations at the beginning of the forward run
    //   (-1 means idle/reset; >= 0 means run in progress or forward completed)
    // mileage_now:  latest polled cumulative rotations
    //   delta = mileage_now - mileage_start
    //   Forward complete when delta >= target_count
    //   Reverse complete when mileage_now <= mileage_start
    int32_t mileage_start;
    int32_t mileage_now;

    // Timer mode: actual RPM feedback from 0x65 response (in RPM, not rpm10 units)
    int16_t actual_rpm;

    // Count mode: step size for ±adjustment (1 or 10)
    int16_t count_step;
} control_state_t;
