#pragma once
#include <stdint.h>
namespace position_math {
constexpr int64_t units_per_turn = 32768;
inline int64_t oriented_position(int32_t turns, uint16_t angle, bool ccw) {
    const int64_t raw = (int64_t)turns * units_per_turn + angle;
    return ccw ? -raw : raw;
}
// Relative angular displacement; zero crossing is a small step, not a whole turn.
inline int32_t angle_step(uint16_t previous, uint16_t current) {
    int32_t step = (int32_t)current - previous;
    if (step > units_per_turn / 2) step -= units_per_turn;
    else if (step < -units_per_turn / 2) step += units_per_turn;
    return step;
}
inline int32_t hundredths(int64_t delta) {
    return (int32_t)((delta * 100) / units_per_turn);
}
}
