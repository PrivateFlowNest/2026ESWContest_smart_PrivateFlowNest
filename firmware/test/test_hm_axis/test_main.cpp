// Unit tests for lib/hm_axis (pio test -e native). PC simulation only: virtual microsecond clock
// and a simulated carriage/sensor. No hardware is involved; passing here proves the logic, not the
// motor, driver, sensor or wiring.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include <hm_axis.h>
#include <hm_axis_protocol.h>

using namespace hm::axis;

namespace {

struct StepRecord {
  uint32_t time_us;
  int8_t dir;        // physical direction of the carriage
  int32_t carriage;  // physical position after the step
};

// Simulated driver + carriage + sensor. "wiring" holds the physical polarities (same meaning as the
// AxisConfig fields), so a controller that ignored its config would move the wrong way.
class SimHal : public AxisHal {
 public:
  explicit SimHal(const AxisConfig& wiring)
      : now(1000), dir_high(false), enable_high(false), step_width(0), carriage(0), switch_at(0),
        switch_side(-1), forced(-1), steps_while_disabled(0), wiring_(wiring) {}

  void writeDir(bool high) override { dir_high = high; }
  void pulseStep(uint16_t width_us) override {
    step_width = width_us;
    if (!driverOn()) {
      ++steps_while_disabled;
      return;
    }
    const int8_t dir = (dir_high != wiring_.invert_dir) ? 1 : -1;
    carriage += dir;
    steps.push_back(StepRecord{now, dir, carriage});
  }
  void writeEnable(bool high) override { enable_high = high; }
  bool readEndstop() override { return sensorActive() == wiring_.endstop_active_high; }
  uint32_t nowUs() override { return now; }

  // Sensor blocked: carriage at or beyond the trigger point on the home side, unless forced.
  bool sensorActive() const {
    if (forced >= 0) return forced == 1;
    return switch_side < 0 ? carriage <= switch_at : carriage >= switch_at;
  }
  bool driverOn() const { return enable_high != wiring_.enable_active_low; }

  uint32_t now;
  bool dir_high;
  bool enable_high;
  uint16_t step_width;
  int32_t carriage;
  int32_t switch_at;
  int8_t switch_side;
  int forced;  // -1 real sensor, 0 never active (broken), 1 always active (stuck / glitch)
  int steps_while_disabled;
  std::vector<StepRecord> steps;

 private:
  AxisConfig wiring_;
};

class LineCapture : public LineSink {
 public:
  void writeLine(const char* line) override { lines.push_back(line); }
  std::vector<std::string> lines;
};

struct Rig {
  explicit Rig(const AxisConfig& c)
      : config(c), hal(c), axis(hal), sink(), controller(axis, sink, "test_axis") {}
  AxisConfig config;
  SimHal hal;
  Axis axis;
  LineCapture sink;
  Controller controller;
};

const float kStepsPerMm = 80.0f;
const int32_t kSwitchAt = 1000;  // physical trigger point (steps, arbitrary origin)
const NamedPosition kPositions[] = {{"entrance", 0.0f}, {"bed", 120.0f}, {"bathroom", 250.5f}};

AxisConfig testConfig() {
  AxisConfig c;
  c.steps_per_mm = kStepsPerMm;
  c.max_speed_mm_s = 150.0f;   // 12000 steps/s
  c.accel_mm_s2 = 1000.0f;     // 80000 steps/s^2
  c.travel_mm = 400.0f;        // 32000 steps
  c.home_dir = -1;
  c.invert_dir = false;
  c.endstop_active_high = true;
  c.enable_active_low = true;
  c.home_fast_mm_s = 40.0f;    // 3200 steps/s
  c.home_slow_mm_s = 4.0f;     // 320 steps/s
  c.home_backoff_mm = 3.0f;    // 240 steps
  c.home_margin_mm = 20.0f;
  c.endstop_zone_mm = 5.0f;
  c.endstop_debounce_us = 0;
  c.idle_disable_ms = 0;
  c.unhome_on_idle_disable = false;
  c.step_pulse_us = 2;
  return c;
}

int32_t steps(float mm) { return static_cast<int32_t>(lroundf(mm * kStepsPerMm)); }

Rig* rig = nullptr;

// New rig; the carriage starts start_mm away from the trigger point on the travel side.
void boot(const AxisConfig& c, float start_mm = 100.0f, const NamedPosition* positions = kPositions,
          uint8_t count = 3, bool keep_boot_lines = false) {
  delete rig;
  rig = new Rig(c);
  rig->hal.switch_at = kSwitchAt;
  rig->hal.switch_side = c.home_dir;
  rig->hal.carriage = kSwitchAt - c.home_dir * steps(start_mm);
  rig->axis.begin(c);
  rig->controller.begin(positions, count);
  if (!keep_boot_lines) rig->sink.lines.clear();
}

void tickOnce() {
  rig->hal.now += 1;
  rig->axis.tick();
  rig->controller.poll();
}

void tickFor(uint32_t us) {
  for (uint32_t index = 0; index < us; ++index) tickOnce();
}

// Sends one line and returns the reply (every command line gets exactly one reply line).
std::string command(const char* line) {
  const size_t before = rig->sink.lines.size();
  rig->controller.feed(line, strlen(line));
  rig->controller.feed('\n');
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(before + 1, rig->sink.lines.size(), line);
  return rig->sink.lines.back();
}

// Ticks until a new "EVT " line appears; returns it ("" on timeout).
std::string waitEvent(uint32_t max_us) {
  size_t seen = rig->sink.lines.size();
  for (uint32_t index = 0; index < max_us; ++index) {
    tickOnce();
    while (seen < rig->sink.lines.size()) {
      const std::string& line = rig->sink.lines[seen++];
      if (line.compare(0, 4, "EVT ") == 0) return line;
    }
  }
  return "";
}

void tickUntilSteps(size_t count, uint32_t max_us) {
  for (uint32_t index = 0; index < max_us && rig->hal.steps.size() < count; ++index) tickOnce();
  TEST_ASSERT_EQUAL_UINT32(count, rig->hal.steps.size());
}

void homeOk() {
  TEST_ASSERT_EQUAL_STRING("OK HOME", command("HOME").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT HOMED", waitEvent(30000000).c_str());
  TEST_ASSERT_TRUE(rig->axis.homed());
}

// Firmware position and physical carriage agree (home_dir -1: firmware 0 = trigger point).
void expectPhysical(int32_t firmware_steps) {
  TEST_ASSERT_EQUAL_INT32(firmware_steps, rig->axis.position());
  TEST_ASSERT_EQUAL_INT32(kSwitchAt + firmware_steps, rig->hal.carriage);
}

struct Run {
  int8_t dir;
  size_t first;
  size_t count;
};

std::vector<Run> runs(const std::vector<StepRecord>& s) {
  std::vector<Run> out;
  for (size_t index = 0; index < s.size(); ++index) {
    if (out.empty() || out.back().dir != s[index].dir) {
      out.push_back(Run{s[index].dir, index, 0});
    }
    ++out.back().count;
  }
  return out;
}

uint32_t interval(size_t index) {
  return rig->hal.steps[index].time_us - rig->hal.steps[index - 1].time_us;
}

// Every interval in [first+1, last] respects max_speed (steps/s).
void expectMaxSpeed(size_t first, size_t last, double max_speed) {
  for (size_t index = first + 1; index <= last; ++index) {
    if (static_cast<double>(interval(index)) * max_speed < 1e6) {
      char message[96];
      snprintf(message, sizeof(message), "step %u interval %u us exceeds %.0f steps/s",
               static_cast<unsigned>(index), static_cast<unsigned>(interval(index)), max_speed);
      TEST_FAIL_MESSAGE(message);
    }
  }
}

// Largest acceleration seen in [first, last], from the mean speeds of consecutive windows of
// `window` steps (the mean speed over a window is the speed at its time midpoint for constant
// acceleration, and a bound for the cruise/brake transitions). Integer-microsecond timing adds a
// few percent of noise.
double maxAcceleration(size_t first, size_t last, size_t window) {
  double worst = 0.0;
  const std::vector<StepRecord>& s = rig->hal.steps;
  for (size_t i = first; i + 2 * window <= last; ++i) {
    const double t0 = s[i].time_us;
    const double t1 = s[i + window].time_us;
    const double t2 = s[i + 2 * window].time_us;
    const double v1 = window * 1e6 / (t1 - t0);
    const double v2 = window * 1e6 / (t2 - t1);
    const double a = fabs(v2 - v1) / ((t2 - t0) * 0.5e-6);
    if (a > worst) worst = a;
  }
  return worst;
}

bool startsWith(const std::string& text, const char* prefix) {
  return text.compare(0, strlen(prefix), prefix) == 0;
}

}  // namespace

void setUp() {}

void tearDown() {
  delete rig;
  rig = nullptr;
}

// ---------------------------------------------------------------- boot / config

void test_boot_disables_driver_and_announces() {
  boot(testConfig(), 100.0f, kPositions, 3, true);
  TEST_ASSERT_FALSE(rig->hal.driverOn());
  TEST_ASSERT_EQUAL_UINT32(1, rig->sink.lines.size());
  TEST_ASSERT_EQUAL_STRING("EVT BOOT test_axis hm_axis/1.0.0", rig->sink.lines[0].c_str());
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 UNHOMED 0", command("POS?").c_str());
  tickFor(100000);
  TEST_ASSERT_EQUAL_UINT32(0, rig->hal.steps.size());
}

void test_config_validation() {
  TEST_ASSERT_NULL(validateConfig(testConfig()));
  AxisConfig c = testConfig();
  c.steps_per_mm = 0.0f;
  TEST_ASSERT_EQUAL_STRING("steps_per_mm", validateConfig(c));
  c.steps_per_mm = NAN;
  TEST_ASSERT_EQUAL_STRING("steps_per_mm", validateConfig(c));
  c = testConfig();
  c.max_speed_mm_s = 1000.0f;  // 80000 steps/s > kMaxStepRate
  TEST_ASSERT_EQUAL_STRING("max_speed_mm_s", validateConfig(c));
  c = testConfig();
  c.home_dir = 0;
  TEST_ASSERT_EQUAL_STRING("home_dir", validateConfig(c));
  c = testConfig();
  c.home_fast_mm_s = 200.0f;  // faster than max speed
  TEST_ASSERT_EQUAL_STRING("home_fast_mm_s", validateConfig(c));
  c = testConfig();
  c.endstop_zone_mm = 2.0f;  // smaller than the backoff
  TEST_ASSERT_EQUAL_STRING("endstop_zone_mm", validateConfig(c));
  c = testConfig();
  c.step_pulse_us = 50;  // no LOW gap left at 12000 steps/s
  TEST_ASSERT_EQUAL_STRING("step_pulse_us", validateConfig(c));
  c = testConfig();
  c.travel_mm = -1.0f;
  TEST_ASSERT_EQUAL_STRING("travel_mm", validateConfig(c));
}

void test_invalid_config_refuses_everything() {
  AxisConfig c = testConfig();
  c.accel_mm_s2 = 0.0f;
  boot(c, 100.0f, kPositions, 3, true);
  TEST_ASSERT_EQUAL_UINT32(2, rig->sink.lines.size());
  TEST_ASSERT_EQUAL_STRING("EVT ERROR CONFIG_INVALID accel_mm_s2", rig->sink.lines[1].c_str());
  TEST_ASSERT_EQUAL_STRING("ERR CONFIG_INVALID accel_mm_s2", command("HOME").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR CONFIG_INVALID accel_mm_s2", command("MOVE 10").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR CONFIG_INVALID accel_mm_s2", command("ENABLE 1").c_str());
  TEST_ASSERT_EQUAL_STRING("OK ESTOP", command("ESTOP").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR CONFIG_INVALID accel_mm_s2", command("HOME").c_str());
  tickFor(10000);
  TEST_ASSERT_FALSE(rig->hal.driverOn());
  TEST_ASSERT_EQUAL_UINT32(0, rig->hal.steps.size());
}

void test_invalid_default_positions_are_config_errors() {
  const NamedPosition upper_case[] = {{"Bed", 10.0f}};
  boot(testConfig(), 100.0f, upper_case, 1, true);
  TEST_ASSERT_EQUAL_STRING("EVT ERROR CONFIG_INVALID positions", rig->sink.lines.back().c_str());
  TEST_ASSERT_EQUAL_STRING("ERR CONFIG_INVALID positions", command("HOME").c_str());
  const NamedPosition too_far[] = {{"bed", 10.0f}, {"far", 400.5f}};
  boot(testConfig(), 100.0f, too_far, 2, true);
  TEST_ASSERT_EQUAL_STRING("EVT ERROR CONFIG_INVALID positions", rig->sink.lines.back().c_str());
}

// ---------------------------------------------------------------- homing

void test_homing_finds_switch_and_sets_zero_after_backoff_and_reseek() {
  boot(testConfig(), 100.0f);
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 IDLE 1", command("POS?").c_str());
  expectPhysical(0);  // zero = trigger point of the slow re-seek
  const std::vector<Run> r = runs(rig->hal.steps);
  TEST_ASSERT_EQUAL_UINT32(3, r.size());
  TEST_ASSERT_EQUAL_INT(-1, r[0].dir);  // fast seek toward the sensor
  TEST_ASSERT_EQUAL_UINT32(steps(100.0f), r[0].count);
  TEST_ASSERT_EQUAL_INT(1, r[1].dir);  // backoff
  TEST_ASSERT_EQUAL_UINT32(steps(3.0f), r[1].count);
  TEST_ASSERT_EQUAL_INT(-1, r[2].dir);  // slow re-seek
  TEST_ASSERT_EQUAL_UINT32(steps(3.0f), r[2].count);
  expectMaxSpeed(r[0].first, r[0].first + r[0].count - 1, 40.0 * kStepsPerMm);
  expectMaxSpeed(r[1].first, r[1].first + r[1].count - 1, 40.0 * kStepsPerMm);
  expectMaxSpeed(r[2].first, r[2].first + r[2].count - 1, 4.0 * kStepsPerMm);
  TEST_ASSERT_EQUAL_INT(2, rig->hal.step_width);
  TEST_ASSERT_EQUAL_INT(0, rig->hal.steps_while_disabled);
}

void test_homing_when_already_on_switch_backs_off_first() {
  boot(testConfig(), -1.0f);  // 1 mm past the trigger point: sensor active at HOME
  TEST_ASSERT_TRUE(rig->hal.sensorActive());
  homeOk();
  const std::vector<Run> r = runs(rig->hal.steps);
  TEST_ASSERT_EQUAL_UINT32(2, r.size());
  TEST_ASSERT_EQUAL_INT(1, r[0].dir);
  TEST_ASSERT_EQUAL_UINT32(steps(3.0f), r[0].count);
  TEST_ASSERT_EQUAL_INT(-1, r[1].dir);
  TEST_ASSERT_EQUAL_UINT32(steps(2.0f), r[1].count);
  expectPhysical(0);
}

void test_homing_at_far_end_with_inverted_polarities() {
  AxisConfig c = testConfig();
  c.home_dir = 1;  // sensor at the travel_mm end
  c.invert_dir = true;
  c.endstop_active_high = false;
  c.enable_active_low = false;
  boot(c, 50.0f);  // 50 mm before the far-end trigger point
  TEST_ASSERT_FALSE(rig->hal.driverOn());
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK POS 400.00 IDLE 1", command("POS?").c_str());
  TEST_ASSERT_EQUAL_INT32(kSwitchAt, rig->hal.carriage);
  TEST_ASSERT_EQUAL_STRING("OK MOVE bed 120.00", command("MOVE bed").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED bed 120.00", waitEvent(10000000).c_str());
  TEST_ASSERT_EQUAL_INT32(kSwitchAt - steps(280.0f), rig->hal.carriage);
}

void test_broken_sensor_fails_homing_after_travel_plus_margin() {
  AxisConfig c = testConfig();
  c.travel_mm = 100.0f;
  c.home_margin_mm = 10.0f;
  const NamedPosition near[] = {{"bed", 50.0f}};
  boot(c, 50.0f, near, 1);
  rig->hal.forced = 0;  // never triggers
  TEST_ASSERT_EQUAL_STRING("OK HOME", command("HOME").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ERROR HOME_FAILED no_trigger", waitEvent(10000000).c_str());
  TEST_ASSERT_EQUAL_UINT32(steps(110.0f), rig->hal.steps.size());
  const std::vector<Run> r = runs(rig->hal.steps);
  TEST_ASSERT_EQUAL_UINT32(1, r.size());
  TEST_ASSERT_EQUAL_STRING("OK POS -110.00 ERROR 0", command("POS?").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR NOT_HOMED send HOME first", command("MOVE bed").c_str());
  tickFor(100000);
  TEST_ASSERT_EQUAL_UINT32(steps(110.0f), rig->hal.steps.size());
}

void test_stuck_sensor_fails_homing_after_backoff() {
  boot(testConfig(), 100.0f);
  rig->hal.forced = 1;  // always active
  tickOnce();
  TEST_ASSERT_EQUAL_STRING("OK HOME", command("HOME").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ERROR HOME_FAILED stuck_active", waitEvent(10000000).c_str());
  const std::vector<Run> r = runs(rig->hal.steps);
  TEST_ASSERT_EQUAL_UINT32(1, r.size());
  TEST_ASSERT_EQUAL_INT(1, r[0].dir);  // moved away only, by the backoff distance
  TEST_ASSERT_EQUAL_UINT32(steps(3.0f), r[0].count);
  TEST_ASSERT_EQUAL(AxisState::Error, rig->axis.state());
  TEST_ASSERT_EQUAL(AxisError::HomeFailed, rig->axis.lastError());
}

void test_stop_during_homing_leaves_unhomed() {
  boot(testConfig(), 100.0f);
  TEST_ASSERT_EQUAL_STRING("OK HOME", command("HOME").c_str());
  tickFor(500000);
  TEST_ASSERT_EQUAL_STRING("OK STOP", command("STOP").c_str());
  const std::string event = waitEvent(1000000);
  TEST_ASSERT_TRUE(startsWith(event, "EVT STOPPED "));
  TEST_ASSERT_EQUAL(AxisState::Unhomed, rig->axis.state());
  TEST_ASSERT_FALSE(rig->axis.homed());
  TEST_ASSERT_EQUAL_STRING("ERR NOT_HOMED send HOME first", command("MOVE 10").c_str());
  homeOk();  // can home again
  expectPhysical(0);
}

void test_homing_across_timer_wraparound() {
  boot(testConfig(), 100.0f);
  rig->hal.now = 0xFFFFFFFFu - 1000000u;  // the 32-bit microsecond clock wraps during homing
  homeOk();
  expectPhysical(0);
  TEST_ASSERT_EQUAL_STRING("OK MOVE bathroom 250.50", command("MOVE bathroom").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED bathroom 250.50", waitEvent(10000000).c_str());
  expectPhysical(steps(250.5f));
}

// ---------------------------------------------------------------- moves

void test_move_to_named_positions_lands_on_exact_steps() {
  boot(testConfig(), 100.0f);
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK MOVE bed 120.00", command("MOVE bed").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED bed 120.00", waitEvent(10000000).c_str());
  expectPhysical(9600);
  TEST_ASSERT_EQUAL_STRING("OK MOVE bathroom 250.50", command("move BATHROOM").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED bathroom 250.50", waitEvent(10000000).c_str());
  expectPhysical(20040);
  TEST_ASSERT_EQUAL_STRING("OK MOVE entrance 0.00", command("MOVE entrance").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED entrance 0.00", waitEvent(10000000).c_str());
  expectPhysical(0);  // sensor active at 0 is expected (inside the zone)
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 IDLE 1", command("POS?").c_str());
  TEST_ASSERT_EQUAL_STRING("OK MOVE 12.50 12.50", command("MOVE 12.5").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 12.50 12.50", waitEvent(10000000).c_str());
  expectPhysical(1000);
  TEST_ASSERT_EQUAL_STRING("OK MOVE 55.55 55.55", command("MOVE +55.555").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 55.55 55.55", waitEvent(10000000).c_str());
  expectPhysical(4444);  // round(55.555 * 80)
  TEST_ASSERT_EQUAL_INT(0, rig->hal.steps_while_disabled);
}

void test_move_to_current_position_arrives_without_steps() {
  boot(testConfig(), 100.0f);
  homeOk();
  rig->hal.steps.clear();
  TEST_ASSERT_EQUAL_STRING("OK MOVE entrance 0.00", command("MOVE entrance").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED entrance 0.00", waitEvent(1000).c_str());
  TEST_ASSERT_EQUAL_UINT32(0, rig->hal.steps.size());
}

void test_long_move_is_trapezoidal_within_speed_and_acceleration() {
  boot(testConfig(), 100.0f);
  homeOk();
  rig->hal.steps.clear();
  const uint32_t start = rig->hal.now;
  TEST_ASSERT_EQUAL_STRING("OK MOVE 300.00 300.00", command("MOVE 300").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 300.00 300.00", waitEvent(10000000).c_str());
  const size_t count = rig->hal.steps.size();
  TEST_ASSERT_EQUAL_UINT32(24000, count);
  expectPhysical(24000);
  const double vmax = 150.0 * kStepsPerMm;
  const double accel = 1000.0 * kStepsPerMm;
  expectMaxSpeed(0, count - 1, vmax);
  // Cruise at max speed (interval = ceil(1e6 / 12000) = 84 us) for most of the move.
  uint32_t shortest = 0xFFFFFFFFu;
  size_t at_cruise = 0;
  for (size_t index = 1; index < count; ++index) {
    if (interval(index) < shortest) shortest = interval(index);
    if (interval(index) == 84) ++at_cruise;
  }
  TEST_ASSERT_EQUAL_UINT32(84, shortest);
  TEST_ASSERT_TRUE(at_cruise > 20000);
  // Accelerates and brakes at (not above) the configured rate.
  const double worst = maxAcceleration(0, count - 1, 32);
  TEST_ASSERT_TRUE_MESSAGE(worst <= accel * 1.10, "acceleration above the limit");
  TEST_ASSERT_TRUE_MESSAGE(worst >= accel * 0.80, "acceleration far below the limit");
  // Brake: intervals only grow over the last 900 steps (v^2 / 2a = 900).
  for (size_t index = count - 899; index < count; ++index) {
    TEST_ASSERT_TRUE(interval(index) >= interval(index - 1));
  }
  // Duration close to the ideal trapezoid: 2 * v/a + (d - v^2/a) / v = 2.15 s.
  const double ideal = 2.0 * vmax / accel + (24000.0 - vmax * vmax / accel) / vmax;
  const double actual = (rig->hal.steps.back().time_us - start) * 1e-6;
  TEST_ASSERT_TRUE(actual >= ideal * 0.995);
  TEST_ASSERT_TRUE(actual <= ideal * 1.02);
}

void test_short_move_is_triangular() {
  boot(testConfig(), 100.0f);
  homeOk();
  rig->hal.steps.clear();
  const uint32_t start = rig->hal.now;
  TEST_ASSERT_EQUAL_STRING("OK MOVE 2.00 2.00", command("MOVE 2").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 2.00 2.00", waitEvent(1000000).c_str());
  TEST_ASSERT_EQUAL_UINT32(160, rig->hal.steps.size());
  expectPhysical(160);
  // Peak speed sqrt(a * d) = 3578 steps/s, far below max speed.
  uint32_t shortest = 0xFFFFFFFFu;
  for (size_t index = 1; index < 160; ++index) {
    if (interval(index) < shortest) shortest = interval(index);
  }
  TEST_ASSERT_TRUE(shortest >= 1e6 / 3600.0);
  TEST_ASSERT_TRUE(shortest <= 1e6 / 3400.0);
  TEST_ASSERT_TRUE(maxAcceleration(0, 159, 16) <= 1000.0 * kStepsPerMm * 1.10);
  // Ideal 2 * sqrt(d / a) = 89.4 ms.
  const double actual = (rig->hal.steps.back().time_us - start) * 1e-6;
  TEST_ASSERT_TRUE(actual >= 0.0894 * 0.99 && actual <= 0.0894 * 1.03);
}

void test_single_step_move() {
  boot(testConfig(), 100.0f);
  homeOk();
  rig->hal.steps.clear();
  TEST_ASSERT_EQUAL_STRING("OK MOVE 0.01 0.01", command("MOVE 0.0125").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 0.01 0.01", waitEvent(100000).c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig->hal.steps.size());
  expectPhysical(1);
}

void test_soft_limits() {
  boot(testConfig(), 100.0f);
  homeOk();
  rig->hal.steps.clear();
  TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE allowed 0.00..400.00", command("MOVE -1").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE allowed 0.00..400.00", command("MOVE -0.01").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE allowed 0.00..400.00", command("MOVE 400.01").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE allowed 0.00..400.00", command("MOVE 999999").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE allowed 0.00..400.00", command("SET far 400.5").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE allowed 0.00..400.00", command("SET far -3").c_str());
  tickFor(10000);
  TEST_ASSERT_EQUAL_UINT32(0, rig->hal.steps.size());
  TEST_ASSERT_EQUAL_STRING("OK MOVE 400.00 400.00", command("MOVE 400").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 400.00 400.00", waitEvent(10000000).c_str());
  expectPhysical(32000);
}

void test_moves_refused_before_homing() {
  boot(testConfig(), 100.0f);
  TEST_ASSERT_EQUAL_STRING("ERR NOT_HOMED send HOME first", command("MOVE bed").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR NOT_HOMED send HOME first", command("MOVE 10").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR NOT_HOMED send HOME first", command("MOVE\tbed").c_str());
  tickFor(100000);
  TEST_ASSERT_EQUAL_UINT32(0, rig->hal.steps.size());
  TEST_ASSERT_FALSE(rig->hal.driverOn());
}

void test_busy_while_moving() {
  boot(testConfig(), 100.0f);
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK MOVE bathroom 250.50", command("MOVE bathroom").c_str());
  tickFor(200000);
  TEST_ASSERT_EQUAL_STRING("ERR BUSY MOVING", command("MOVE bed").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BUSY MOVING", command("HOME").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BUSY MOVING", command("ENABLE 0").c_str());
  TEST_ASSERT_EQUAL_STRING("OK SET kitchen 30.00", command("SET kitchen 30").c_str());
  TEST_ASSERT_TRUE(startsWith(command("POS?"), "OK POS "));
  TEST_ASSERT_TRUE(command("POS?").find(" MOVING 1") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED bathroom 250.50", waitEvent(10000000).c_str());
  expectPhysical(20040);
}

void test_stop_decelerates_at_configured_rate() {
  boot(testConfig(), 100.0f);
  homeOk();
  rig->hal.steps.clear();
  TEST_ASSERT_EQUAL_STRING("OK MOVE 300.00 300.00", command("MOVE 300").c_str());
  tickUntilSteps(5000, 2000000);  // well into the cruise at 12000 steps/s
  const uint32_t stop_time = rig->hal.now;
  TEST_ASSERT_EQUAL_STRING("OK STOP", command("STOP").c_str());
  TEST_ASSERT_EQUAL(AxisState::Stopping, rig->axis.state());
  TEST_ASSERT_EQUAL_STRING("EVT STOPPED 73.75", waitEvent(1000000).c_str());
  // Braking distance v^2 / 2a = 12000^2 / 160000 = 900 steps, taking v / a = 0.15 s.
  const size_t count = rig->hal.steps.size();
  TEST_ASSERT_EQUAL_UINT32(5900, count);
  for (size_t index = 5001; index < count; ++index) {
    TEST_ASSERT_TRUE(interval(index) >= interval(index - 1));
  }
  TEST_ASSERT_TRUE(maxAcceleration(4800, count - 1, 32) <= 1000.0 * kStepsPerMm * 1.10);
  const double braking = (rig->hal.steps.back().time_us - stop_time) * 1e-6;
  TEST_ASSERT_TRUE(braking >= 0.15 * 0.98 && braking <= 0.15 * 1.05);
  TEST_ASSERT_EQUAL(AxisState::Idle, rig->axis.state());
  TEST_ASSERT_TRUE(rig->axis.homed());
  expectPhysical(5900);
  tickFor(100000);
  TEST_ASSERT_EQUAL_UINT32(5900, rig->hal.steps.size());
  TEST_ASSERT_EQUAL_STRING("OK STOP", command("STOP").c_str());  // idle: harmless, no event
  tickFor(1000);
  TEST_ASSERT_TRUE(startsWith(rig->sink.lines.back(), "OK STOP"));
}

void test_estop_stops_at_once_and_disables() {
  boot(testConfig(), 100.0f);
  homeOk();
  rig->hal.steps.clear();
  TEST_ASSERT_EQUAL_STRING("OK MOVE 300.00 300.00", command("MOVE 300").c_str());
  tickUntilSteps(3000, 2000000);
  TEST_ASSERT_EQUAL_STRING("OK ESTOP", command("ESTOP now").c_str());
  TEST_ASSERT_FALSE(rig->hal.driverOn());
  TEST_ASSERT_EQUAL_STRING("EVT ERROR ESTOP", waitEvent(1000).c_str());
  tickFor(500000);
  TEST_ASSERT_EQUAL_UINT32(3000, rig->hal.steps.size());  // not a single step after ESTOP
  TEST_ASSERT_EQUAL_STRING("OK POS 37.50 ERROR 0", command("POS?").c_str());
  TEST_ASSERT_EQUAL_STRING("OK STATUS state=ERROR homed=0 pos=37.50 endstop=0 enabled=0 error=ESTOP",
                           command("STATUS?").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR NOT_HOMED send HOME first", command("MOVE bed").c_str());
  homeOk();  // HOME recovers
  expectPhysical(0);
}

void test_estop_when_idle_has_no_event() {
  boot(testConfig(), 100.0f);
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK ESTOP", command("ESTOP").c_str());
  TEST_ASSERT_EQUAL_STRING("", waitEvent(10000).c_str());
  TEST_ASSERT_FALSE(rig->axis.homed());
  TEST_ASSERT_FALSE(rig->hal.driverOn());
}

// ---------------------------------------------------------------- sensor supervision

void test_endstop_while_moving_away_is_an_error() {
  boot(testConfig(), 100.0f);
  homeOk();
  rig->hal.steps.clear();
  TEST_ASSERT_EQUAL_STRING("OK MOVE 300.00 300.00", command("MOVE 300").c_str());
  tickUntilSteps(2000, 2000000);
  rig->hal.forced = 1;  // obstacle / wiring fault
  TEST_ASSERT_EQUAL_STRING("EVT ERROR ENDSTOP_HIT 25.00", waitEvent(1000).c_str());
  tickFor(100000);
  TEST_ASSERT_EQUAL_UINT32(2000, rig->hal.steps.size());
  TEST_ASSERT_EQUAL(AxisState::Error, rig->axis.state());
  TEST_ASSERT_EQUAL(AxisError::EndstopHit, rig->axis.lastError());
  TEST_ASSERT_EQUAL_STRING("ERR NOT_HOMED send HOME first", command("MOVE bed").c_str());
}

void test_sensor_far_from_home_while_moving_toward_it_is_an_error() {
  boot(testConfig(), 100.0f);
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK MOVE 200.00 200.00", command("MOVE 200").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 200.00 200.00", waitEvent(10000000).c_str());
  rig->hal.carriage -= steps(100.0f);  // 100 mm of lost steps: the sensor comes 100 mm early
  TEST_ASSERT_EQUAL_STRING("OK MOVE entrance 0.00", command("MOVE entrance").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ERROR ENDSTOP_HIT 100.00", waitEvent(10000000).c_str());
  TEST_ASSERT_EQUAL_INT32(kSwitchAt, rig->hal.carriage);  // stopped right at the trigger point
}

void test_sensor_inside_zone_is_tolerated() {
  boot(testConfig(), 100.0f);
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK MOVE 200.00 200.00", command("MOVE 200").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 200.00 200.00", waitEvent(10000000).c_str());
  rig->hal.carriage -= steps(2.0f);  // sensor triggers 2 mm early (< 5 mm zone)
  TEST_ASSERT_EQUAL_STRING("OK MOVE entrance 0.00", command("MOVE entrance").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED entrance 0.00", waitEvent(10000000).c_str());
  TEST_ASSERT_TRUE(rig->hal.sensorActive());
  TEST_ASSERT_EQUAL_STRING("OK MOVE bed 120.00", command("MOVE bed").c_str());  // leaves the sensor
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED bed 120.00", waitEvent(10000000).c_str());
}

void test_debounce_ignores_short_glitch() {
  AxisConfig c = testConfig();
  c.endstop_debounce_us = 200;
  boot(c, 100.0f);
  homeOk();
  expectPhysical(0);
  rig->hal.steps.clear();
  TEST_ASSERT_EQUAL_STRING("OK MOVE 300.00 300.00", command("MOVE 300").c_str());
  tickUntilSteps(3000, 2000000);
  rig->hal.forced = 1;
  tickFor(150);  // shorter than the debounce
  rig->hal.forced = -1;
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED 300.00 300.00", waitEvent(10000000).c_str());
  // Arrive on the sensor and leave at once: its activation is still being debounced when the next
  // move starts, which must not count as a new activation while moving away.
  TEST_ASSERT_EQUAL_STRING("OK MOVE entrance 0.00", command("MOVE entrance").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED entrance 0.00", waitEvent(10000000).c_str());
  TEST_ASSERT_EQUAL_STRING("OK MOVE 100.00 100.00", command("MOVE 100").c_str());
  tickFor(300000);  // mid-move (the move takes about 0.82 s), sensor long cleared
  TEST_ASSERT_EQUAL(AxisState::Moving, rig->axis.state());
  rig->hal.forced = 1;  // a real (lasting) activation
  TEST_ASSERT_EQUAL_STRING("ERROR", waitEvent(1000).substr(4, 5).c_str());
  TEST_ASSERT_EQUAL(AxisError::EndstopHit, rig->axis.lastError());
}

// ---------------------------------------------------------------- driver enable

void test_idle_disable_keeps_home_and_reenables_on_move() {
  AxisConfig c = testConfig();
  c.idle_disable_ms = 2000;
  boot(c, 100.0f);
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK MOVE bed 120.00", command("MOVE bed").c_str());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED bed 120.00", waitEvent(10000000).c_str());
  tickFor(1999000);
  TEST_ASSERT_TRUE(rig->hal.driverOn());
  tickFor(1001);
  TEST_ASSERT_FALSE(rig->hal.driverOn());
  TEST_ASSERT_EQUAL_STRING("OK POS 120.00 IDLE 1", command("POS?").c_str());
  TEST_ASSERT_EQUAL_STRING("OK MOVE entrance 0.00", command("MOVE entrance").c_str());
  TEST_ASSERT_TRUE(rig->hal.driverOn());
  TEST_ASSERT_EQUAL_STRING("EVT ARRIVED entrance 0.00", waitEvent(10000000).c_str());
  expectPhysical(0);
  TEST_ASSERT_EQUAL_INT(0, rig->hal.steps_while_disabled);
}

void test_idle_disable_can_forget_home() {
  AxisConfig c = testConfig();
  c.idle_disable_ms = 1000;
  c.unhome_on_idle_disable = true;
  boot(c, 100.0f);
  homeOk();
  tickFor(1000100);
  TEST_ASSERT_FALSE(rig->hal.driverOn());
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 UNHOMED 0", command("POS?").c_str());
}

void test_enable_command() {
  boot(testConfig(), 100.0f);
  TEST_ASSERT_EQUAL_STRING("OK ENABLE 1", command("ENABLE 1").c_str());
  TEST_ASSERT_TRUE(rig->hal.driverOn());
  TEST_ASSERT_EQUAL_STRING("OK ENABLE 0", command("enable 0").c_str());
  TEST_ASSERT_FALSE(rig->hal.driverOn());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: ENABLE 0|1", command("ENABLE 2").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: ENABLE 0|1", command("ENABLE").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: ENABLE 0|1", command("ENABLE on").c_str());
  homeOk();
  TEST_ASSERT_EQUAL_STRING("OK ENABLE 0", command("ENABLE 0").c_str());  // carriage now free
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 UNHOMED 0", command("POS?").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR NOT_HOMED send HOME first", command("MOVE bed").c_str());
}

// ---------------------------------------------------------------- protocol

void test_protocol_queries_and_named_positions() {
  boot(testConfig(), 100.0f);
  TEST_ASSERT_EQUAL_STRING("OK VERSION test_axis hm_axis/1.0.0", command("VERSION?").c_str());
  TEST_ASSERT_EQUAL_STRING("OK VERSION test_axis hm_axis/1.0.0", command("version").c_str());
  TEST_ASSERT_EQUAL_STRING("OK LIST 3 entrance=0.00 bed=120.00 bathroom=250.50",
                           command("LIST").c_str());
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 UNHOMED 0", command("pos?").c_str());
  TEST_ASSERT_EQUAL_STRING("OK STATUS state=UNHOMED homed=0 pos=0.00 endstop=0 enabled=0 error=NONE",
                           command("Status?").c_str());
  TEST_ASSERT_TRUE(startsWith(command("HELP"), "OK HELP "));
  TEST_ASSERT_EQUAL_STRING("OK SET kitchen 33.30", command("SET Kitchen 33.3").c_str());
  TEST_ASSERT_EQUAL_STRING("OK SET bed 121.00", command("set bed 121").c_str());  // replaces
  TEST_ASSERT_EQUAL_STRING("OK LIST 4 entrance=0.00 bed=121.00 bathroom=250.50 kitchen=33.30",
                           command("list").c_str());
  TEST_ASSERT_EQUAL_STRING("OK SET a_16_chars_name_ 1.00", command("SET a_16_chars_name_ 1").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_NAME use [a-z0-9_]{1,16}, not only digits",
                           command("SET a_17_chars_name__ 1").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_NAME use [a-z0-9_]{1,16}, not only digits",
                           command("SET bad-name 1").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_NAME use [a-z0-9_]{1,16}, not only digits",
                           command("SET 123 1").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: SET <name> <mm>", command("SET x 1e3").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: SET <name> <mm>", command("SET x").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: SET <name> <mm>", command("SET x nan").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: SET <name> <mm>", command("SET x 1 2").c_str());
  TEST_ASSERT_EQUAL_STRING("OK SET p6 6.00", command("SET p6 6").c_str());
  TEST_ASSERT_EQUAL_STRING("OK SET p7 7.00", command("SET p7 7").c_str());
  TEST_ASSERT_EQUAL_STRING("OK SET p8 8.00", command("SET p8 8").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR TABLE_FULL max 8 names", command("SET p9 9").c_str());
  TEST_ASSERT_EQUAL_STRING("OK SET p8 8.50", command("SET p8 8.5").c_str());  // update still works
  TEST_ASSERT_EQUAL_UINT32(8, rig->controller.positions().count());
}

void test_protocol_argument_errors() {
  boot(testConfig(), 100.0f);
  TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_COMMAND FOO", command("FOO").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_COMMAND abcdefghijklmnop", command("abcdefghijklmnopqrstuvwxyz").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_COMMAND ESTOP!", command("ESTOP!").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: MOVE <name|mm>", command("MOVE").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: MOVE <name|mm>", command("MOVE bed now").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: MOVE <name|mm>", command("MOVE 1.2.3").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: MOVE <name|mm>", command("MOVE -").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: MOVE <name|mm>", command("MOVE 1234567").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_POSITION kitchen2", command("MOVE Kitchen2").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_POSITION 1e3", command("MOVE 1e3").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: HOME", command("HOME now").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: POS?", command("POS? x").c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_ARG usage: LIST", command("LIST a b c d e").c_str());
  TEST_ASSERT_EQUAL_STRING("OK STOP", command("stop please").c_str());
  float mm = 0.0f;
  TEST_ASSERT_TRUE(parseMm("12", &mm));
  TEST_ASSERT_EQUAL_FLOAT(12.0f, mm);
  TEST_ASSERT_TRUE(parseMm("-0.5", &mm));
  TEST_ASSERT_EQUAL_FLOAT(-0.5f, mm);
  TEST_ASSERT_TRUE(parseMm(".25", &mm));
  TEST_ASSERT_EQUAL_FLOAT(0.25f, mm);
  TEST_ASSERT_TRUE(parseMm("7.", &mm));
  TEST_ASSERT_EQUAL_FLOAT(7.0f, mm);
  TEST_ASSERT_TRUE(parseMm("120.000000", &mm));
  TEST_ASSERT_FALSE(parseMm("", &mm));
  TEST_ASSERT_FALSE(parseMm(".", &mm));
  TEST_ASSERT_FALSE(parseMm("+", &mm));
  TEST_ASSERT_FALSE(parseMm("1,5", &mm));
  TEST_ASSERT_FALSE(parseMm("inf", &mm));
  TEST_ASSERT_FALSE(parseMm("1.0000001", &mm));
  TEST_ASSERT_FALSE(parseMm(" 1", &mm));
}

void test_protocol_line_handling_and_garbage() {
  boot(testConfig(), 100.0f);
  // Blank lines, CR, CRLF: no reply for empty lines, one reply per command.
  const char blanks[] = "\n\r\n   \n\t\n";
  rig->controller.feed(blanks, strlen(blanks));
  TEST_ASSERT_EQUAL_UINT32(0, rig->sink.lines.size());
  const char three[] = "POS?\r\nLIST\rVERSION?\n";
  rig->controller.feed(three, strlen(three));
  TEST_ASSERT_EQUAL_UINT32(3, rig->sink.lines.size());
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 UNHOMED 0", rig->sink.lines[0].c_str());
  TEST_ASSERT_TRUE(startsWith(rig->sink.lines[1], "OK LIST 3 "));
  TEST_ASSERT_TRUE(startsWith(rig->sink.lines[2], "OK VERSION "));
  rig->sink.lines.clear();
  // Over-long line: one error at its end, nothing executed, the next line works.
  std::string longest(kLineCapacity - 1, 'x');
  TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_COMMAND xxxxxxxxxxxxxxxx", command(longest.c_str()).c_str());
  std::string too_long = "HOME " + std::string(300, 'y');
  TEST_ASSERT_EQUAL_STRING("ERR LINE_TOO_LONG max 95 chars", command(too_long.c_str()).c_str());
  tickFor(10000);
  TEST_ASSERT_EQUAL_UINT32(0, rig->hal.steps.size());  // the HOME in it was not executed
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 UNHOMED 0", command("POS?").c_str());
  // Control / non-ASCII bytes reject the whole line.
  const char bad[] = {'H', 'O', 'M', 'E', '\x00', '\n'};
  rig->controller.feed(bad, sizeof(bad));
  TEST_ASSERT_EQUAL_STRING("ERR BAD_LINE non-ASCII or control byte", rig->sink.lines.back().c_str());
  const char high[] = {'P', 'O', 'S', '\xFF', '?', '\n'};
  rig->controller.feed(high, sizeof(high));
  TEST_ASSERT_EQUAL_STRING("ERR BAD_LINE non-ASCII or control byte", rig->sink.lines.back().c_str());
  TEST_ASSERT_EQUAL_STRING("ERR BAD_LINE non-ASCII or control byte", command("POS\x1b[A").c_str());
  TEST_ASSERT_EQUAL_STRING("OK POS 0.00 UNHOMED 0", command("POS?").c_str());
}

void test_protocol_survives_random_bytes() {
  boot(testConfig(), 100.0f);
  uint32_t state = 0x2468ACE1u;
  for (int index = 0; index < 20000; ++index) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    // Mostly printable noise with some separators and control bytes.
    const uint32_t pick = state % 100;
    char byte;
    if (pick < 6) {
      byte = '\n';
    } else if (pick < 12) {
      byte = ' ';
    } else if (pick < 15) {
      byte = static_cast<char>(state >> 8);
    } else {
      byte = static_cast<char>(0x21 + (state >> 8) % 94);
    }
    rig->controller.feed(byte);
    if (index % 64 == 0) tickOnce();
  }
  for (size_t index = 0; index < rig->sink.lines.size(); ++index) {
    const std::string& line = rig->sink.lines[index];
    TEST_ASSERT_TRUE(startsWith(line, "OK ") || startsWith(line, "ERR ") || startsWith(line, "EVT "));
    TEST_ASSERT_TRUE(line.size() < kOutputCapacity);
  }
  TEST_ASSERT_TRUE(rig->sink.lines.size() > 100);
  rig->controller.feed('\n');
  TEST_ASSERT_TRUE(startsWith(command("POS?"), "OK POS "));
  TEST_ASSERT_EQUAL_UINT32(0, rig->hal.steps.size());  // noise never moved the unhomed axis
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_disables_driver_and_announces);
  RUN_TEST(test_config_validation);
  RUN_TEST(test_invalid_config_refuses_everything);
  RUN_TEST(test_invalid_default_positions_are_config_errors);
  RUN_TEST(test_homing_finds_switch_and_sets_zero_after_backoff_and_reseek);
  RUN_TEST(test_homing_when_already_on_switch_backs_off_first);
  RUN_TEST(test_homing_at_far_end_with_inverted_polarities);
  RUN_TEST(test_broken_sensor_fails_homing_after_travel_plus_margin);
  RUN_TEST(test_stuck_sensor_fails_homing_after_backoff);
  RUN_TEST(test_stop_during_homing_leaves_unhomed);
  RUN_TEST(test_homing_across_timer_wraparound);
  RUN_TEST(test_move_to_named_positions_lands_on_exact_steps);
  RUN_TEST(test_move_to_current_position_arrives_without_steps);
  RUN_TEST(test_long_move_is_trapezoidal_within_speed_and_acceleration);
  RUN_TEST(test_short_move_is_triangular);
  RUN_TEST(test_single_step_move);
  RUN_TEST(test_soft_limits);
  RUN_TEST(test_moves_refused_before_homing);
  RUN_TEST(test_busy_while_moving);
  RUN_TEST(test_stop_decelerates_at_configured_rate);
  RUN_TEST(test_estop_stops_at_once_and_disables);
  RUN_TEST(test_estop_when_idle_has_no_event);
  RUN_TEST(test_endstop_while_moving_away_is_an_error);
  RUN_TEST(test_sensor_far_from_home_while_moving_toward_it_is_an_error);
  RUN_TEST(test_sensor_inside_zone_is_tolerated);
  RUN_TEST(test_debounce_ignores_short_glitch);
  RUN_TEST(test_idle_disable_keeps_home_and_reenables_on_move);
  RUN_TEST(test_idle_disable_can_forget_home);
  RUN_TEST(test_enable_command);
  RUN_TEST(test_protocol_queries_and_named_positions);
  RUN_TEST(test_protocol_argument_errors);
  RUN_TEST(test_protocol_line_handling_and_garbage);
  RUN_TEST(test_protocol_survives_random_bytes);
  return UNITY_END();
}
