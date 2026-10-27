#include "hm_stepper.h"

#include <math.h>

namespace hm {
namespace axis {

Stepper::Stepper()
    : position_(0), target_(0), dir_(0), running_(false), braking_(false), max_speed_(1.0f),
      accel_(1.0f), speed_(0.0f), next_speed_(0.0f), last_us_(0), interval_us_(0) {}

void Stepper::setPosition(int32_t steps) {
  halt();
  position_ = steps;
  target_ = steps;
}

int8_t Stepper::moveTo(int32_t target, float max_speed, float accel, uint32_t now_us) {
  target_ = target;
  max_speed_ = max_speed;
  accel_ = accel;
  speed_ = 0.0f;
  next_speed_ = 0.0f;
  braking_ = false;
  last_us_ = now_us;
  if (target == position_) {
    running_ = false;
    interval_us_ = 0;
    return 0;
  }
  dir_ = target > position_ ? 1 : -1;
  running_ = true;
  plan();
  return dir_;
}

void Stepper::decelerate() {
  if (!running_) return;
  if (speed_ <= 0.0f) {
    // Not a single step emitted yet: nothing to brake.
    halt();
    return;
  }
  // Steps needed to brake from speed_ to 0 at accel_ (v^2 = 2 a n), at least one.
  int32_t steps = static_cast<int32_t>(ceilf(speed_ * speed_ / (2.0f * accel_) - 1e-4f));
  if (steps < 1) steps = 1;
  const int32_t stop_at = position_ + dir_ * steps;
  if ((dir_ > 0 && stop_at < target_) || (dir_ < 0 && stop_at > target_)) target_ = stop_at;
  braking_ = true;
  plan();
}

void Stepper::halt() {
  running_ = false;
  braking_ = false;
  target_ = position_;
  speed_ = 0.0f;
  next_speed_ = 0.0f;
  interval_us_ = 0;
}

bool Stepper::tick(uint32_t now_us) {
  if (!running_) return false;
  if (static_cast<uint32_t>(now_us - last_us_) < interval_us_) return false;
  position_ += dir_;
  speed_ = next_speed_;
  last_us_ = now_us;
  plan();
  return true;
}

void Stepper::plan() {
  const int32_t remaining = dir_ > 0 ? target_ - position_ : position_ - target_;
  if (remaining <= 0) {
    running_ = false;
    braking_ = false;
    speed_ = 0.0f;
    next_speed_ = 0.0f;
    interval_us_ = 0;
    return;
  }
  const float two_a = 2.0f * accel_;
  float next = sqrtf(speed_ * speed_ + two_a);
  if (next > max_speed_) next = max_speed_;
  const float brake_limit = sqrtf(two_a * static_cast<float>(remaining - 1));
  if (next > brake_limit) next = brake_limit;
  if (braking_ && next > speed_) next = speed_;
  float mean = 0.5f * (speed_ + next);
  // A step that starts and ends at (almost) rest, e.g. a 1-step move: use the mean speed of a
  // one-step move at constant acceleration, but never more than max_speed.
  float floor_mean = 0.5f * sqrtf(two_a);
  if (floor_mean > max_speed_) floor_mean = max_speed_;
  if (mean < floor_mean) mean = floor_mean;
  interval_us_ = static_cast<uint32_t>(ceilf(1000000.0f / mean));
  if (interval_us_ == 0) interval_us_ = 1;
  next_speed_ = next;
}

}  // namespace axis
}  // namespace hm
