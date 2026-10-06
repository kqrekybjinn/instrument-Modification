#pragma once

#include <stdbool.h>
#include <stdint.h>

// motor_driver.h is implemented in C in this project; wrap it to keep C linkage
extern "C" {
#include "motor_driver.h"
}

// UI / control parameters
#define DEFAULT_FORWARD_RPM 100
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
    MODE_FREE = 0,   // Free run: smooth accel only, manual stop, no target count
    MODE_COUNT,      // Count mode: fixed target rotations, smooth accel+decel, auto-stop
} operating_mode_t;

typedef enum {
    // Free run: toggle start/stop. Count mode: pause/resume/start-forward.
    CMD_STATE_TOGGLE = 0,
    // Cycle speed level. Free run: allowed while running. Count mode: only when stopped.
    CMD_SPEED_STEP,
    // Switch between free-run and count modes (only when fully idle)
    CMD_MODE_SWITCH,
    // Adjust target rotation count (only in count mode, stopped, at origin)
    CMD_COUNT_INC,
    CMD_COUNT_DEC,
    // Toggle count step size between ×1 and ×10
    CMD_COUNT_STEP_TOGGLE,
    // Count mode: stop current motion and enter reverse flow
    CMD_COUNT_ABORT,
    // Toggle forward direction (CW/CCW)
    CMD_FORWARD_DIR_TOGGLE,
} control_cmd_id_t;

typedef struct {
    control_cmd_id_t id;
} control_cmd_t;

typedef struct {
    bool feedback_valid; // Latest query succeeded and reported no motor fault.
    bool running;
    bool reversing;
    bool paused;            // count mode: motor paused mid-run (forward/reverse)
    bool forward_ccw;       // true: forward=CCW, false: forward=CW
    int16_t target_rpm;
    int64_t run_start_us;
    int32_t sensor_value;
    operating_mode_t mode;
    int16_t target_count;           // count mode: target rotations

    // Sub-rotation precision tracking using Q15 fixed-point.
    // fixed_pos = mileage_rotations * 32768 + position_raw   (1 rotation = 32768 units)
    // pos_fixed_start: value recorded at run start (INT64_MIN = idle/reset)
    // pos_fixed_now:   latest polled value
    // pos_fixed_peak:  furthest forward position (free mode: used to compute return journey total)
    // delta_fixed = pos_fixed_now - pos_fixed_start
    // Count forward complete when delta_fixed >= target_count * 32768
    // Reverse complete when pos_fixed_now <= pos_fixed_start
    int64_t pos_fixed_start;   // INT64_MIN means idle/reset
    int64_t pos_fixed_now;
    int64_t pos_fixed_peak;    // free mode: furthest position reached, for return distance calc

    // Legacy integer mileage fields kept for reverse-completion check compatibility.
    int32_t mileage_start;
    int32_t mileage_now;

    // Count mode: step size for ±adjustment (1 or 10)
    int16_t count_step;
} control_state_t;
