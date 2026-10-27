// Single-colour (R) indicator LED state, following contracts/README.md "펌웨어가 지킬 LED 규칙":
//   set_switch {on}                     power state. Without overlay: on = lit, off = dark
//   set_color {red, pattern:"blink"}    quiet overlay on: slow blink regardless of power state
//   set_color {red, pattern:"steady"}   overlay off, back to showing the power state
//   set_color {red} (no pattern)        same as steady
//   set_color {off}                     overlay off + power off
// Blink period: 1 s on / 1 s off (kLedBlinkHalfPeriodMs), phase starts lit when the overlay starts.
// set_switch may carry auto_off_after_seconds (night_bed_exit uses 15): power goes off after that
// time. Any later set_switch or set_color{off} cancels a pending auto-off.
#pragma once

#include <stdint.h>

namespace hm {

class LedState {
 public:
  LedState() : power_(false), overlay_(false), blink_start_ms_(0), auto_off_armed_(false),
               auto_off_at_ms_(0) {}

  void setSwitch(bool on, uint32_t auto_off_seconds, uint64_t now_ms);
  // pattern: nullptr (absent), "steady" or "blink". color: "off" or a supported colour.
  void setColor(bool color_off, bool blink, uint64_t now_ms);

  // Applies a due auto-off. Returns true if the power state changed.
  bool tick(uint64_t now_ms);

  bool power() const { return power_; }
  bool overlay() const { return overlay_; }
  bool autoOffArmed() const { return auto_off_armed_; }
  uint64_t autoOffAtMs() const { return auto_off_at_ms_; }
  // Physical output level at now_ms (true = GPIO HIGH = lit).
  bool outputLevel(uint64_t now_ms) const;
  // Telemetry light_pattern: "blink" when overlay, else "steady" when powered, else "off".
  const char* pattern() const;

 private:
  bool power_;
  bool overlay_;
  uint64_t blink_start_ms_;
  bool auto_off_armed_;
  uint64_t auto_off_at_ms_;
};

}  // namespace hm
