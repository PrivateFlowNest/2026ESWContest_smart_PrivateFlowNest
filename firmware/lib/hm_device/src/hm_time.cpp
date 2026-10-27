#include "hm_time.h"

#include "hm_protocol.h"

namespace hm {

void CommandClock::observe(int64_t issued_at_ms, uint64_t received_mono_ms) {
  if (!valid_) {
    valid_ = true;
    floor_issued_ms_ = issued_at_ms;
    floor_mono_ms_ = received_mono_ms;
    return;
  }
  // Commands are processed in arrival order, so received_mono_ms does not go backwards.
  const int64_t current = floorAt(received_mono_ms);
  floor_issued_ms_ = issued_at_ms > current ? issued_at_ms : current;
  floor_mono_ms_ = received_mono_ms;
}

int64_t CommandClock::floorAt(uint64_t mono_ms) const {
  const int64_t elapsed =
      mono_ms >= floor_mono_ms_ ? static_cast<int64_t>(mono_ms - floor_mono_ms_) : 0;
  return floor_issued_ms_ + elapsed;
}

ExpiryReason CommandClock::check(int64_t issued_at_ms, int64_t expires_at_ms,
                                 uint64_t received_mono_ms, const TimeNow& now) const {
  int64_t ttl = expires_at_ms - issued_at_ms;
  if (ttl > static_cast<int64_t>(kCommandTtlMs)) ttl = kCommandTtlMs;
  if (ttl < 0) ttl = 0;
  const int64_t deadline = issued_at_ms + ttl;

  if (now.wall_valid && now.wall_ms >= deadline) return ExpiryReason::WallClock;

  const int64_t held =
      now.mono_ms >= received_mono_ms ? static_cast<int64_t>(now.mono_ms - received_mono_ms) : 0;
  if (held >= ttl) return ExpiryReason::HeldTooLong;

  if (valid_ && floorAt(now.mono_ms) >= deadline) return ExpiryReason::CoreClockFloor;
  return ExpiryReason::NotExpired;
}

}  // namespace hm
