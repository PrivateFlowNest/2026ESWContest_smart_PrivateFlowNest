// Non-blocking step generator for one STEP/DIR stepper: trapezoidal (constant acceleration) speed
// profile, decided one step at a time. Pure logic, no hardware access: the caller asks tick(now_us)
// whether a step is due and emits the STEP pulse itself (hm_axis.h does that through its HAL).
//
// Profile, per step (a = acceleration in steps/s^2, v = speed reached at the previous step):
//   v_next = min(max_speed, sqrt(v^2 + 2a), sqrt(2a * (remaining - 1)))   (accelerate / cruise / brake)
//   interval = 1 / mean(v, v_next)    = exact time to cover one step at constant acceleration
// so the carriage never exceeds max_speed, never accelerates or brakes harder than a, and reaches
// speed 0 exactly on the last step. Intervals are rounded UP to whole microseconds, and the next
// interval is counted from the time the step was actually emitted: a late tick() therefore makes
// the motion slightly slower, never faster than planned.
#pragma once

#include <stdint.h>

namespace hm {
namespace axis {

class Stepper {
 public:
  Stepper();

  // Sets the step counter. Stops any motion (no deceleration).
  void setPosition(int32_t steps);
  int32_t position() const { return position_; }
  int32_t target() const { return target_; }
  bool running() const { return running_; }
  // +1 / -1 for the current (or last) move, 0 before the first move.
  int8_t direction() const { return dir_; }
  // Speed reached at the last emitted step, steps/s (0 at rest).
  float speed() const { return speed_; }
  // Wait between the last step (or the move start) and the next step, microseconds.
  uint32_t intervalUs() const { return interval_us_; }

  // Starts a move from rest to the absolute target. max_speed in steps/s, accel in steps/s^2,
  // both > 0. Returns the direction (+1/-1), or 0 when already at the target (nothing to do).
  // Must not be called while running (the caller stops first).
  int8_t moveTo(int32_t target, float max_speed, float accel, uint32_t now_us);
  // Brakes at the configured acceleration and stops as soon as possible (never later than the
  // original target). No-op when not running.
  void decelerate();
  // Stops at once: no further steps. Steps may be lost mechanically if the motor was fast.
  void halt();
  // Returns true when one step must be emitted now; position() already includes that step.
  bool tick(uint32_t now_us);

 private:
  void plan();

  int32_t position_;
  int32_t target_;
  int8_t dir_;
  bool running_;
  bool braking_;
  float max_speed_;
  float accel_;
  float speed_;
  float next_speed_;
  uint32_t last_us_;
  uint32_t interval_us_;
};

}  // namespace axis
}  // namespace hm
