#include "hm_axis.h"

#include <math.h>
#include <stddef.h>

namespace hm {
namespace axis {

const char kAxisLibVersion[] = "1.0.0";

namespace {

// > 0 and finite (also rejects NaN).
bool positive(float value) { return value > 0.0f && value < 1e30f; }

}  // namespace

const char* validateConfig(const AxisConfig& c) {
  if (!positive(c.steps_per_mm) || c.steps_per_mm > 10000.0f) return "steps_per_mm";
  if (!positive(c.max_speed_mm_s) || c.max_speed_mm_s * c.steps_per_mm > kMaxStepRate) {
    return "max_speed_mm_s";
  }
  if (!positive(c.accel_mm_s2) || c.accel_mm_s2 * c.steps_per_mm > 1e7f) return "accel_mm_s2";
  if (!positive(c.travel_mm) || c.travel_mm * c.steps_per_mm < 1.0f ||
      c.travel_mm * c.steps_per_mm > kMaxTravelSteps) {
    return "travel_mm";
  }
  if (c.home_dir != -1 && c.home_dir != 1) return "home_dir";
  if (!positive(c.home_fast_mm_s) || c.home_fast_mm_s > c.max_speed_mm_s) return "home_fast_mm_s";
  if (!positive(c.home_slow_mm_s) || c.home_slow_mm_s > c.home_fast_mm_s) return "home_slow_mm_s";
  if (!positive(c.home_backoff_mm) || c.home_backoff_mm * c.steps_per_mm < 1.0f ||
      c.home_backoff_mm * 2.0f > c.travel_mm) {
    return "home_backoff_mm";
  }
  if (!(c.home_margin_mm >= 0.0f) || c.home_margin_mm > c.travel_mm) return "home_margin_mm";
  // The sensor clears within the backoff distance (homing checks that), so anything active
  // farther than the backoff from the home end is unexpected: the zone must cover the backoff.
  if (!(c.endstop_zone_mm >= c.home_backoff_mm) || c.endstop_zone_mm >= c.travel_mm) {
    return "endstop_zone_mm";
  }
  if (c.endstop_debounce_us > 100000u) return "endstop_debounce_us";
  if (c.idle_disable_ms > kMaxIdleDisableMs) return "idle_disable_ms";
  if (c.step_pulse_us < 1 || c.step_pulse_us > 100) return "step_pulse_us";
  // At full speed the STEP line must still spend at least as long LOW as HIGH.
  if (2.0f * static_cast<float>(c.step_pulse_us) * c.max_speed_mm_s * c.steps_per_mm > 1e6f) {
    return "step_pulse_us";
  }
  return nullptr;
}

const char* stateName(AxisState state) {
  switch (state) {
    case AxisState::Unhomed: return "UNHOMED";
    case AxisState::Homing: return "HOMING";
    case AxisState::Idle: return "IDLE";
    case AxisState::Moving: return "MOVING";
    case AxisState::Stopping: return "STOPPING";
    case AxisState::Error: return "ERROR";
  }
  return "ERROR";
}

const char* errorName(AxisError error) {
  switch (error) {
    case AxisError::None: return "NONE";
    case AxisError::NotHomed: return "NOT_HOMED";
    case AxisError::OutOfRange: return "OUT_OF_RANGE";
    case AxisError::Busy: return "BUSY";
    case AxisError::HomeFailed: return "HOME_FAILED";
    case AxisError::EndstopHit: return "ENDSTOP_HIT";
    case AxisError::Estop: return "ESTOP";
    case AxisError::ConfigInvalid: return "CONFIG_INVALID";
  }
  return "CONFIG_INVALID";
}

const char* failDetailName(FailDetail detail) {
  switch (detail) {
    case FailDetail::None: return "";
    case FailDetail::NoTrigger: return "no_trigger";
    case FailDetail::StuckActive: return "stuck_active";
  }
  return "";
}

Axis::Axis(AxisHal& hal)
    : hal_(hal), config_(), problem_("not_started"), stepper_(), state_(AxisState::Error),
      phase_(HomePhase::None), last_error_(AxisError::ConfigInvalid), homed_(false),
      enabled_(false), homing_aborted_(false), endstop_raw_(false), endstop_active_(false),
      clear_seen_(false), endstop_change_us_(0), last_activity_us_(0), idle_disable_us_(0),
      travel_steps_(0), home_steps_(0), zone_steps_(0), backoff_steps_(1), seek_limit_steps_(0),
      max_speed_steps_(1.0f), accel_steps_(1.0f), home_fast_steps_(1.0f), home_slow_steps_(1.0f),
      events_(), event_head_(0), event_count_(0) {}

bool Axis::begin(const AxisConfig& config) {
  config_ = config;
  problem_ = validateConfig(config_);
  stepper_.setPosition(0);
  phase_ = HomePhase::None;
  homed_ = false;
  homing_aborted_ = false;
  event_head_ = 0;
  event_count_ = 0;
  driveEnable(false);
  const uint32_t now = hal_.nowUs();
  endstop_raw_ = hal_.readEndstop() == config_.endstop_active_high;
  endstop_active_ = endstop_raw_;
  clear_seen_ = false;
  endstop_change_us_ = now;
  last_activity_us_ = now;
  if (problem_ != nullptr) {
    state_ = AxisState::Error;
    last_error_ = AxisError::ConfigInvalid;
    return false;
  }
  const float spm = config_.steps_per_mm;
  travel_steps_ = static_cast<int32_t>(lroundf(config_.travel_mm * spm));
  home_steps_ = config_.home_dir < 0 ? 0 : travel_steps_;
  zone_steps_ = static_cast<int32_t>(lroundf(config_.endstop_zone_mm * spm));
  backoff_steps_ = static_cast<int32_t>(lroundf(config_.home_backoff_mm * spm));
  if (backoff_steps_ < 1) backoff_steps_ = 1;
  seek_limit_steps_ =
      static_cast<int32_t>(lroundf((config_.travel_mm + config_.home_margin_mm) * spm));
  max_speed_steps_ = config_.max_speed_mm_s * spm;
  accel_steps_ = config_.accel_mm_s2 * spm;
  home_fast_steps_ = config_.home_fast_mm_s * spm;
  home_slow_steps_ = config_.home_slow_mm_s * spm;
  idle_disable_us_ = config_.idle_disable_ms * 1000u;
  state_ = AxisState::Unhomed;
  last_error_ = AxisError::None;
  return true;
}

void Axis::invalidateConfig(const char* reason) {
  problem_ = reason != nullptr ? reason : "config";
  stepper_.halt();
  driveEnable(false);
  homed_ = false;
  phase_ = HomePhase::None;
  state_ = AxisState::Error;
  last_error_ = AxisError::ConfigInvalid;
}

bool Axis::moving() const {
  return state_ == AxisState::Homing || state_ == AxisState::Moving ||
         state_ == AxisState::Stopping;
}

void Axis::tick() {
  const uint32_t now = hal_.nowUs();
  updateEndstop(now);
  if (moving()) beforeStep(now);
  if (stepper_.running() && stepper_.tick(now)) {
    hal_.pulseStep(config_.step_pulse_us);
    last_activity_us_ = now;
  }
  if (moving() && !stepper_.running()) onMotionDone(now);
  if (!moving() && enabled_ && idle_disable_us_ > 0 &&
      static_cast<uint32_t>(now - last_activity_us_) >= idle_disable_us_) {
    driveEnable(false);
    if (config_.unhome_on_idle_disable && homed_) {
      homed_ = false;
      if (state_ == AxisState::Idle) state_ = AxisState::Unhomed;
    }
  }
}

void Axis::updateEndstop(uint32_t now_us) {
  const bool raw = hal_.readEndstop() == config_.endstop_active_high;
  if (raw != endstop_raw_) {
    endstop_raw_ = raw;
    endstop_change_us_ = now_us;
  }
  if (raw != endstop_active_ &&
      static_cast<uint32_t>(now_us - endstop_change_us_) >= config_.endstop_debounce_us) {
    endstop_active_ = raw;
  }
}

void Axis::beforeStep(uint32_t now_us) {
  if (state_ == AxisState::Homing) {
    if (!endstop_active_) return;
    if (phase_ == HomePhase::SeekFast) {
      stepper_.halt();
      startPhase(HomePhase::Backoff, now_us);
    } else if (phase_ == HomePhase::SeekSlow) {
      // The zero is exactly the step at which the (filtered) sensor reported the trigger.
      stepper_.halt();
      stepper_.setPosition(home_steps_);
      homed_ = true;
      phase_ = HomePhase::None;
      state_ = AxisState::Idle;
      last_error_ = AxisError::None;
      last_activity_us_ = now_us;
      pushEvent(EventType::Homed, AxisError::None, FailDetail::None);
    }
    return;
  }
  // MOVING or STOPPING.
  if (!endstop_raw_ && !endstop_active_) clear_seen_ = true;
  if (!endstop_active_) return;
  if (!homed_) {
    // Braking after an aborted homing: reaching the sensor is fine, stop right there.
    if (stepper_.direction() == config_.home_dir) stepper_.halt();
    return;
  }
  const bool away_from_home = stepper_.direction() == -config_.home_dir;
  if (distanceFromHome(stepper_.position()) > zone_steps_ || (away_from_home && clear_seen_)) {
    fail(AxisError::EndstopHit, FailDetail::None);
  }
}

void Axis::onMotionDone(uint32_t now_us) {
  last_activity_us_ = now_us;
  switch (state_) {
    case AxisState::Homing:
      if (phase_ == HomePhase::Backoff) {
        // Conservative: a sensor that is still (or again) active after the backoff fails homing.
        if (endstop_active_ || endstop_raw_) {
          fail(AxisError::HomeFailed, FailDetail::StuckActive);
        } else {
          startPhase(HomePhase::SeekSlow, now_us);
        }
      } else {
        // SEEK_FAST or SEEK_SLOW ran its whole distance without a trigger.
        fail(AxisError::HomeFailed, FailDetail::NoTrigger);
      }
      break;
    case AxisState::Moving:
      state_ = AxisState::Idle;
      pushEvent(EventType::Arrived, AxisError::None, FailDetail::None);
      break;
    case AxisState::Stopping:
      state_ = homing_aborted_ ? AxisState::Unhomed : AxisState::Idle;
      homing_aborted_ = false;
      pushEvent(EventType::Stopped, AxisError::None, FailDetail::None);
      break;
    default:
      break;
  }
}

void Axis::startMotion(int32_t target, float speed, uint32_t now_us) {
  if (!enabled_) driveEnable(true);
  last_activity_us_ = now_us;
  clear_seen_ = false;
  const int8_t dir = stepper_.moveTo(target, speed, accel_steps_, now_us);
  // DIR is set here, at least one step interval (milliseconds) before the first STEP pulse.
  if (dir != 0) hal_.writeDir((dir > 0) != config_.invert_dir);
}

void Axis::startPhase(HomePhase phase, uint32_t now_us) {
  phase_ = phase;
  const int32_t position = stepper_.position();
  const int32_t toward = config_.home_dir;
  switch (phase) {
    case HomePhase::SeekFast:
      startMotion(position + toward * seek_limit_steps_, home_fast_steps_, now_us);
      break;
    case HomePhase::Backoff:
      startMotion(position - toward * backoff_steps_, home_fast_steps_, now_us);
      break;
    case HomePhase::SeekSlow:
      startMotion(position + toward * 2 * backoff_steps_, home_slow_steps_, now_us);
      break;
    case HomePhase::None:
      break;
  }
}

AxisError Axis::home() {
  if (problem_ != nullptr) return AxisError::ConfigInvalid;
  if (moving()) return AxisError::Busy;
  const uint32_t now = hal_.nowUs();
  updateEndstop(now);  // a trigger since the last tick decides the first phase too
  // An unhomed counter means nothing: restart it so repeated attempts cannot drift without bound.
  if (!homed_) stepper_.setPosition(0);
  homed_ = false;
  homing_aborted_ = false;
  state_ = AxisState::Homing;
  last_error_ = AxisError::None;
  startPhase(endstop_active_ ? HomePhase::Backoff : HomePhase::SeekFast, now);
  return AxisError::None;
}

AxisError Axis::canMove() const {
  if (problem_ != nullptr) return AxisError::ConfigInvalid;
  if (moving()) return AxisError::Busy;
  if (!homed_) return AxisError::NotHomed;
  return AxisError::None;
}

bool Axis::targetForMm(float mm, int32_t* steps) const {
  if (problem_ != nullptr) return false;
  // Rough bound first so the conversion below can never overflow.
  if (!(mm > -1.0f && mm < config_.travel_mm + 1.0f)) return false;
  const int32_t value = static_cast<int32_t>(lroundf(mm * config_.steps_per_mm));
  if (value < 0 || value > travel_steps_) return false;
  if (steps != nullptr) *steps = value;
  return true;
}

AxisError Axis::moveTo(int32_t target_steps) {
  const AxisError allowed = canMove();
  if (allowed != AxisError::None) return allowed;
  if (target_steps < 0 || target_steps > travel_steps_) return AxisError::OutOfRange;
  state_ = AxisState::Moving;
  startMotion(target_steps, max_speed_steps_, hal_.nowUs());
  return AxisError::None;
}

void Axis::stop() {
  if (state_ != AxisState::Moving && state_ != AxisState::Homing) return;
  homing_aborted_ = state_ == AxisState::Homing;
  phase_ = HomePhase::None;
  state_ = AxisState::Stopping;
  stepper_.decelerate();
}

void Axis::estop() {
  const bool was_moving = moving();
  stepper_.halt();
  driveEnable(false);
  homed_ = false;
  homing_aborted_ = false;
  phase_ = HomePhase::None;
  state_ = AxisState::Error;
  if (problem_ == nullptr) last_error_ = AxisError::Estop;
  if (was_moving) pushEvent(EventType::Error, AxisError::Estop, FailDetail::None);
}

AxisError Axis::setEnabled(bool on) {
  if (moving()) return AxisError::Busy;
  if (!on) {
    // Without holding torque the carriage can be pushed: the home position is no longer trusted.
    driveEnable(false);
    if (homed_) {
      homed_ = false;
      if (state_ == AxisState::Idle) state_ = AxisState::Unhomed;
    }
    return AxisError::None;
  }
  if (problem_ != nullptr) return AxisError::ConfigInvalid;
  driveEnable(true);
  last_activity_us_ = hal_.nowUs();
  return AxisError::None;
}

bool Axis::takeEvent(AxisEvent* event) {
  if (event_count_ == 0) return false;
  if (event != nullptr) *event = events_[event_head_];
  event_head_ = static_cast<uint8_t>((event_head_ + 1) % kEventCapacity);
  --event_count_;
  return true;
}

long Axis::stepsToCentiMm(int32_t steps) const {
  const double spm = positive(config_.steps_per_mm) ? config_.steps_per_mm : 1.0;
  return lround(static_cast<double>(steps) * 100.0 / spm);
}

void Axis::fail(AxisError error, FailDetail detail) {
  stepper_.halt();
  homed_ = false;
  homing_aborted_ = false;
  phase_ = HomePhase::None;
  state_ = AxisState::Error;
  last_error_ = error;
  pushEvent(EventType::Error, error, detail);
}

void Axis::driveEnable(bool on) {
  hal_.writeEnable(on != config_.enable_active_low);
  enabled_ = on;
}

void Axis::pushEvent(EventType type, AxisError error, FailDetail detail) {
  if (event_count_ == kEventCapacity) {
    // Full (nobody polled): drop the oldest.
    event_head_ = static_cast<uint8_t>((event_head_ + 1) % kEventCapacity);
    --event_count_;
  }
  AxisEvent& slot = events_[(event_head_ + event_count_) % kEventCapacity];
  slot.type = type;
  slot.error = error;
  slot.detail = detail;
  slot.position = stepper_.position();
  ++event_count_;
}

int32_t Axis::distanceFromHome(int32_t steps) const {
  return config_.home_dir < 0 ? steps - home_steps_ : home_steps_ - steps;
}

}  // namespace axis
}  // namespace hm
