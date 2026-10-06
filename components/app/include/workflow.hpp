#pragma once
#include <stdint.h>
#include <limits.h>
#include "position_math.hpp"
namespace workflow {
enum class Phase : uint8_t { Ready, Running, Paused, Stopped, Complete, Returning, ReturnPaused, ReturnStopped, Settling };
enum class Action : uint8_t { Toggle, Stop, NewRun, Return, Speed, Mode, Direction, Count, Step };
struct State {
    Phase phase = Phase::Ready;
    Phase settle_destination = Phase::Stopped;
    bool feedback_valid = false;
    bool origin_valid = false;
    bool running = false;
    bool reversing = false;
    bool paused = false;
    bool forward_ccw = false;
    bool settle_smooth = false;
    uint8_t mode = 0;
    int16_t target_rpm = 100;
    int16_t target_count = 100;
    int16_t count_step = 10;
    int64_t run_start_us = 0;
    int32_t sensor_value = 0;
    int64_t pos_fixed_start = INT64_MIN;
    int64_t pos_fixed_now = 0;
    int64_t pos_fixed_peak = 0;
    int32_t mileage_start = 0;
    int32_t mileage_now = 0;
    uint8_t motor_fault = 0;
};
inline bool motion(Phase p) { return p == Phase::Running || p == Phase::Returning; }
inline int64_t displacement(const State &s) { return s.origin_valid ? s.pos_fixed_now - s.pos_fixed_start : 0; }
inline bool parameters_unlocked(const State &s) { return s.phase == Phase::Ready && !s.origin_valid; }
inline bool can_return(const State &s) {
 return s.origin_valid && displacement(s) > 0 &&
 (s.phase == Phase::Stopped || s.phase == Phase::Complete || s.phase == Phase::ReturnStopped);
}
inline void set_phase(State &s, Phase p) {
 s.phase = p;
 s.running = motion(p) || p == Phase::Settling;
 s.paused = p == Phase::Paused || p == Phase::ReturnPaused;
 if (p == Phase::Returning || p == Phase::ReturnPaused || p == Phase::ReturnStopped) s.reversing = true;
 if (p == Phase::Running || p == Phase::Ready || p == Phase::Complete || p == Phase::Stopped) s.reversing = false;
}
inline void prepare_new(State &s) {
 s.origin_valid = false;
 s.pos_fixed_start = INT64_MIN;
 s.pos_fixed_peak = 0;
 s.run_start_us = 0;
 set_phase(s, Phase::Ready);
}
// Determines the destination only; the adapter must acquire a valid origin before Ready -> Running.
inline Phase transition(const State &s, Action a) {
 const Phase p = s.phase;
 if (a == Action::Stop) {
  if (p == Phase::Ready) return p;
  return s.reversing ? Phase::ReturnStopped : Phase::Stopped;
 }
 if (a == Action::NewRun) {
  if (!motion(p) && p != Phase::Settling && p != Phase::Paused && p != Phase::ReturnPaused) return Phase::Ready;
  return p;
 }
 if (a == Action::Return) return can_return(s) ? Phase::Returning : p;
 if (a != Action::Toggle) return p;
 switch (p) {
 case Phase::Ready: return Phase::Running;
 case Phase::Running: return Phase::Paused;
 case Phase::Returning: return Phase::ReturnPaused;
 case Phase::Paused: return s.mode == 1 && displacement(s) >= (int64_t)s.target_count * 32768 ? Phase::Complete : Phase::Running;
 case Phase::ReturnPaused: return displacement(s) <= 0 ? Phase::Ready : Phase::Returning;
 default: return p; // A physical Start press after completion never repeats a run.
 }
}
inline bool valid_speed(int speed) { return speed == 50 || speed == 100 || speed == 200 || speed == 300 || speed == 380; }
}
