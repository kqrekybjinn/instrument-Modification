#include "position_math.hpp"
#include <cassert>
#include <climits>
int main() {
 using namespace position_math;
 // A one-count mileage change plus 0.8-turn angle change is displayed as 1.8.
 // There is no extra +1 in the display; raw field semantics must be checked on hardware.
 assert(hundredths(oriented_position(8,31214,false)-oriented_position(7,5000,false))==179);
 // Arbitrary starting angle: crossing the zero angle is not a full turn.
 auto start = oriented_position(7, 30000, false);
 assert(oriented_position(8, 2000, false) - start == 4768);
 assert(oriented_position(17, 30000, false) - start == 10 * units_per_turn);
 // Negative commanded speed must still produce positive forward progress.
 start = oriented_position(7, 30000, true);
 assert(oriented_position(-3, 30000, true) - start == 10 * units_per_turn);
 assert(oriented_position(7, 30000, true) == start);
 assert(hundredths(units_per_turn * 10 + units_per_turn / 4) == 1025);
 assert(hundredths(-units_per_turn / 2) == -50);
 assert(oriented_position(INT32_MIN, 0, true) > 0);
}
