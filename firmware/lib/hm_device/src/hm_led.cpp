#include "hm_led.h"

#include "hm_protocol.h"

namespace hm {

void LedState::setSwitch(bool on, uint32_t auto_off_seconds, uint64_t now_ms) {
  power_ = on;
  if (on && auto_off_seconds > 0) {
    auto_off_armed_ = true;
    auto_off_at_ms_ = now_ms + static_cast<uint64_t>(auto_off_seconds) * 1000u;
  } else {
    auto_off_armed_ = false;
  }
}

void LedState::setColor(bool color_off, bool blink, uint64_t now_ms) {
  if (color_off) {
    overlay_ = false;
    power_ = false;
    auto_off_armed_ = false;
    return;
  }
  if (blink) {
    if (!overlay_) blink_start_ms_ = now_ms;  // keep phase if already blinking
    overlay_ = true;
  } else {
    overlay_ = false;
  }
}

bool LedState::tick(uint64_t now_ms) {
  if (auto_off_armed_ && now_ms >= auto_off_at_ms_) {
    auto_off_armed_ = false;
    const bool changed = power_;
    power_ = false;
    return changed;
  }
  return false;
}

bool LedState::outputLevel(uint64_t now_ms) const {
  if (overlay_) {
    const uint64_t elapsed = now_ms >= blink_start_ms_ ? now_ms - blink_start_ms_ : 0;
    return ((elapsed / kLedBlinkHalfPeriodMs) % 2u) == 0u;
  }
  return power_;
}

const char* LedState::pattern() const {
  if (overlay_) return "blink";
  return power_ ? "steady" : "off";
}

}  // namespace hm
