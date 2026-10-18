#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <algorithm>

namespace position_math {
// The motor mileage boundary need not be the single-turn angle's zero.
// Learn their phase relation from unambiguous consecutive samples, then use
// mileage for whole-turn recovery across long gaps. No fixed boundary angle.
class PhasePosition {
public:
 static constexpr int64_t turn = 32768;
 void reset(int32_t mileage, uint16_t angle, int64_t time_us) {
  position_ = (int64_t)mileage * turn + angle;
  time_us_ = time_us;
  initialized_ = true;
  two_classes_ = false;
  low_class_ = high_class_ = 0;
  low_max_ = high_min_ = angle;
 }
 bool initialized() const { return initialized_; }
 int64_t position() const { return position_; }
 bool calibrated() const { return two_classes_; }
 bool update(int32_t mileage, uint16_t angle, int64_t time_us, int max_rpm) {
  if (!initialized_) { reset(mileage,angle,time_us); return true; }
  const int64_t dt = time_us - time_us_;
  if (dt < 0) return false;
  const int64_t bound = dt * std::max(1,max_rpm) * turn / 60000000 + 128;
  int64_t next;
  if (bound < turn/2) {
   // Short samples uniquely identify which whole-turn branch the angle is on.
   int64_t step = (int64_t)angle - angle_();
   if (step > turn/2) step -= turn;
   if (step < -turn/2) step += turn;
   if (llabs(step) > bound) return false;
   next = position_ + step;
  } else {
   // Unknown boundary region: wait for a resolvable sample, never guess a turn.
   int64_t alignment;
   if (!two_classes_) {
    if (angle < high_min_ || angle > low_max_) return false;
    alignment = low_class_;
   } else if (angle <= low_max_) alignment = low_class_;
   else if (angle >= high_min_) alignment = high_class_;
   else return false;
   next = ((int64_t)mileage-alignment)*turn+angle;
   if (llabs(next-position_) > bound) return false;
  }
  position_ = next;
  time_us_ = time_us;
  learn(mileage,angle);
  return true;
 }
private:
 int64_t position_ = 0, time_us_ = 0;
 int64_t low_class_ = 0, high_class_ = 0;
 uint16_t low_max_ = 0, high_min_ = 0;
 bool initialized_ = false, two_classes_ = false;
 uint16_t angle_() const {
  int64_t a = position_ % turn;
  return (uint16_t)(a < 0 ? a+turn : a);
 }
 void learn(int32_t mileage, uint16_t angle) {
  const int64_t alignment = (int64_t)mileage - (position_ - angle)/turn;
  if (!two_classes_) {
   if (alignment == low_class_) {
    low_max_ = std::max(low_max_,angle);
    high_min_ = std::min(high_min_,angle);
    return;
   }
   if (alignment == low_class_+1) {
    high_class_=alignment; high_min_=angle; two_classes_=true;
   } else if (alignment == low_class_-1) {
    high_class_=low_class_; low_class_=alignment; low_max_=angle; two_classes_=true;
   } else { low_class_=high_class_=alignment; low_max_=high_min_=angle; return; }
  } else if (alignment == low_class_) low_max_=std::max(low_max_,angle);
  else if (alignment == high_class_) high_min_=std::min(high_min_,angle);
  else { low_class_=high_class_=alignment; low_max_=high_min_=angle; two_classes_=false; }
  // A changing boundary or non-coherent motor fields invalidate gap recovery,
  // but do not inject an integer jump into already tracked short samples.
  if (two_classes_ && low_max_ >= high_min_) {
   low_class_=high_class_=alignment;low_max_=high_min_=angle;two_classes_=false;
  }
 }
};
}
