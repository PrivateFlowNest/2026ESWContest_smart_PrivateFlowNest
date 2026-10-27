// Single-axis carriage (the doll moving front/back): one STEP/DIR stepper + one home sensor.
// Hardware-independent: boards and the PC tests plug in an AxisHal. No Arduino headers here.
//
// Coordinates: millimetres along the rail, soft limits 0 .. travel_mm. The home sensor sits at the
// 0 end (home_dir = -1) or at the travel_mm end (home_dir = +1). Internally the position is kept in
// whole steps; mm values are converted with round(mm * steps_per_mm).
//
// States
//   UNHOMED   after boot / ENABLE 0 / aborted homing. Moves are refused (NOT_HOMED)
//   HOMING    SEEK_FAST toward the sensor (gives up after travel + margin -> HOME_FAILED)
//             -> BACKOFF away by home_backoff_mm (sensor must clear, else HOME_FAILED)
//             -> SEEK_SLOW toward the sensor (gives up after 2 x backoff -> HOME_FAILED)
//             -> position := home end exactly where the sensor triggered, state IDLE.
//             If the sensor is already active at HOME, it starts with BACKOFF.
//   IDLE      homed, not moving
//   MOVING    homed, going to a target
//   STOPPING  braking after STOP (from MOVING -> IDLE, from HOMING -> UNHOMED)
//   ERROR     HOME_FAILED, ENDSTOP_HIT, ESTOP or CONFIG_INVALID. Not homed. HOME recovers
//             (except CONFIG_INVALID, which needs a corrected build)
//
// Sensor while MOVING/STOPPING (homed): an active sensor farther than endstop_zone_mm from the home
// end, or a new activation while moving away from home (active again after it was seen clear
// during this move), stops the motor at once -> ERROR ENDSTOP_HIT (lost steps, obstacle, wiring
// fault). Not monitored while not moving.
//
// Every HOME / MOVE ends with exactly one event: HOMED, ARRIVED, STOPPED or ERROR.
#pragma once

#include <stdint.h>

#include "hm_stepper.h"

namespace hm {
namespace axis {

extern const char kAxisLibVersion[];

// Limits enforced by validateConfig().
const float kMaxStepRate = 50000.0f;        // steps/s: what a polling loop can emit reliably
const float kMaxTravelSteps = 8000000.0f;   // keeps all step arithmetic far from int32 limits
const uint32_t kMaxIdleDisableMs = 3600000; // 1 h (microsecond timer arithmetic)

struct AxisConfig {
  float steps_per_mm;         // (motor full steps/rev x microsteps) / mm of travel per rev
  float max_speed_mm_s;
  float accel_mm_s2;
  float travel_mm;            // soft limits 0 .. travel_mm
  int8_t home_dir;            // -1: sensor at the 0 end, +1: sensor at the travel_mm end
  bool invert_dir;            // false: DIR HIGH = toward +mm
  bool endstop_active_high;   // raw pin level that means "sensor triggered"
  bool enable_active_low;     // EN level that enables the driver (A4988/DRV8825/TMC2209: low)
  float home_fast_mm_s;       // first seek
  float home_slow_mm_s;       // re-seek after the backoff (sets the zero)
  float home_backoff_mm;
  float home_margin_mm;       // first seek gives up after travel_mm + home_margin_mm
  float endstop_zone_mm;      // sensor may be active this close to the home end
  uint32_t endstop_debounce_us;  // sensor level must be stable this long (0 = no filter)
  uint32_t idle_disable_ms;   // driver off after this long without motion (0 = never)
  bool unhome_on_idle_disable;   // true: idle disable also forgets the home position
  uint16_t step_pulse_us;     // STEP high time
};

// Returns nullptr when usable, otherwise the name of the first bad field.
const char* validateConfig(const AxisConfig& config);

// Raw pin access. Levels are raw (true = HIGH); polarity is handled by Axis from the config.
class AxisHal {
 public:
  virtual ~AxisHal() {}
  virtual void writeDir(bool high) = 0;
  // One STEP pulse: HIGH for width_us, then LOW.
  virtual void pulseStep(uint16_t width_us) = 0;
  virtual void writeEnable(bool high) = 0;
  virtual bool readEndstop() = 0;
  // Free-running microsecond clock (wraps at 2^32).
  virtual uint32_t nowUs() = 0;
};

enum class AxisState : uint8_t { Unhomed, Homing, Idle, Moving, Stopping, Error };

enum class AxisError : uint8_t {
  None,
  NotHomed,
  OutOfRange,
  Busy,
  HomeFailed,
  EndstopHit,
  Estop,
  ConfigInvalid,
};

// Extra information for HOME_FAILED.
enum class FailDetail : uint8_t { None, NoTrigger, StuckActive };

enum class EventType : uint8_t { Homed, Arrived, Stopped, Error };

struct AxisEvent {
  EventType type;
  AxisError error;
  FailDetail detail;
  int32_t position;  // steps, at the time of the event
};

const char* stateName(AxisState state);
const char* errorName(AxisError error);
const char* failDetailName(FailDetail detail);

class Axis {
 public:
  explicit Axis(AxisHal& hal);

  // Stores and validates the config, disables the driver, state UNHOMED. With an unusable config
  // the state is ERROR / CONFIG_INVALID and nothing ever moves; returns false.
  bool begin(const AxisConfig& config);
  // Marks the build unusable (e.g. invalid default positions): ERROR / CONFIG_INVALID.
  void invalidateConfig(const char* reason);

  // Call as often as possible (every loop iteration).
  void tick();

  AxisError home();
  // BUSY / NOT_HOMED / CONFIG_INVALID check of a MOVE, before its target is looked at.
  AxisError canMove() const;
  // Converts mm to steps; false when outside the soft limits 0 .. travel_mm.
  bool targetForMm(float mm, int32_t* steps) const;
  AxisError moveTo(int32_t target_steps);
  void stop();
  void estop();
  AxisError setEnabled(bool on);

  // Next pending event (oldest first). Returns false when there is none.
  bool takeEvent(AxisEvent* event);

  AxisState state() const { return state_; }
  bool homed() const { return homed_; }
  bool enabled() const { return enabled_; }
  bool endstopActive() const { return endstop_active_; }
  bool moving() const;
  int32_t position() const { return stepper_.position(); }
  AxisError lastError() const { return last_error_; }
  const char* configProblem() const { return problem_; }
  const AxisConfig& config() const { return config_; }
  int32_t travelSteps() const { return travel_steps_; }
  // Position in hundredths of a mm, rounded.
  long stepsToCentiMm(int32_t steps) const;

 private:
  enum class HomePhase : uint8_t { None, SeekFast, Backoff, SeekSlow };

  void updateEndstop(uint32_t now_us);
  void beforeStep(uint32_t now_us);
  void onMotionDone(uint32_t now_us);
  void startMotion(int32_t target, float speed, uint32_t now_us);
  void startPhase(HomePhase phase, uint32_t now_us);
  void fail(AxisError error, FailDetail detail);
  void driveEnable(bool on);
  void pushEvent(EventType type, AxisError error, FailDetail detail);
  int32_t distanceFromHome(int32_t steps) const;

  static const uint8_t kEventCapacity = 4;

  AxisHal& hal_;
  AxisConfig config_;
  const char* problem_;
  Stepper stepper_;
  AxisState state_;
  HomePhase phase_;
  AxisError last_error_;
  bool homed_;
  bool enabled_;
  bool homing_aborted_;
  bool endstop_raw_;
  bool endstop_active_;
  bool clear_seen_;  // sensor seen inactive (raw and filtered) since the motion started
  uint32_t endstop_change_us_;
  uint32_t last_activity_us_;
  uint32_t idle_disable_us_;
  int32_t travel_steps_;
  int32_t home_steps_;
  int32_t zone_steps_;
  int32_t backoff_steps_;
  int32_t seek_limit_steps_;
  float max_speed_steps_;
  float accel_steps_;
  float home_fast_steps_;
  float home_slow_steps_;
  AxisEvent events_[kEventCapacity];
  uint8_t event_head_;
  uint8_t event_count_;
};

}  // namespace axis
}  // namespace hm
