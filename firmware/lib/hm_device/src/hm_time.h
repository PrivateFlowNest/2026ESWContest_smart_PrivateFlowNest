// Time and command-expiry rule.
//
// Controllers may run without internet, so an SNTP wall clock is optional (secrets.h HM_NTP_SERVER).
// A command is EXPIRED (never executed, ACK rejected_expired) if ANY of these holds:
//   ttl       = min(expires_at_ms - issued_at_ms, protocol command_ttl_ms = 10000)
//   deadline  = issued_at_ms + ttl                         (core clock frame)
//   1. wall clock synced (SNTP) and wall_now_ms >= deadline
//   2. the command has been held on this controller for >= ttl since it was received
//   3. "core clock floor": the newest issued_at_ms seen in this MQTT session, advanced by local
//      monotonic time since it was received, is >= deadline. issued_at_ms is core's own clock, so
//      the floor is a lower bound of core's current time even without SNTP. It catches a stale
//      command that arrives after a newer one. The floor is reset on every MQTT (re)connect so a
//      Pi4 reboot with a clock jump cannot block later commands.
// Without SNTP the controller cannot know how long a command spent in flight before it was
// received; rules 2 and 3 are the best available bound (clean MQTT session, so no backlog).
#pragma once

#include <stdint.h>

namespace hm {

struct TimeNow {
  uint64_t mono_ms;    // monotonic milliseconds since boot (uptime)
  bool wall_valid;     // true once SNTP has set the clock
  int64_t wall_ms;     // UTC epoch ms, only meaningful when wall_valid
};

enum class ExpiryReason : uint8_t { NotExpired, WallClock, HeldTooLong, CoreClockFloor };

class CommandClock {
 public:
  CommandClock() : valid_(false), floor_issued_ms_(0), floor_mono_ms_(0) {}

  void reset() { valid_ = false; }

  // Record a core-issued timestamp received at local monotonic time received_mono_ms.
  void observe(int64_t issued_at_ms, uint64_t received_mono_ms);

  bool valid() const { return valid_; }
  // Lower bound of core's clock at local monotonic time mono_ms. Only when valid().
  int64_t floorAt(uint64_t mono_ms) const;

  // Applies the rule above. Call observe() for this command first.
  ExpiryReason check(int64_t issued_at_ms, int64_t expires_at_ms, uint64_t received_mono_ms,
                     const TimeNow& now) const;

 private:
  bool valid_;
  int64_t floor_issued_ms_;
  uint64_t floor_mono_ms_;
};

// Value for *_at_ms fields we publish: UTC epoch ms when synced, otherwise 0
// (the controller does not invent a date; 0 means "clock not synced").
inline int64_t publishedTimeMs(const TimeNow& now) { return now.wall_valid ? now.wall_ms : 0; }

}  // namespace hm
