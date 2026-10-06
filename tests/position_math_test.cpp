#include "position_math.hpp"
#include <cassert>
#include <climits>
int main() {
 using namespace position_math;
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
