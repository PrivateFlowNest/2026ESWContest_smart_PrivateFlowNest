// Clock offset from one core state response (pfn/v1/core/state/response/{client_id}).
#pragma once

#include <cstdint>
#include <string>

namespace hm::clock {

// Earliest plausible core time (2025-01-01 UTC). An older generated_at_ms is a reset clock: never copied.
inline constexpr std::int64_t kEarliestPlausibleMs = 1735689600000LL;

struct Sample {
  std::int64_t generated_at_ms;      // core wall clock when it built the response
  std::int64_t round_trip_ms;        // request published -> response received (steady clock)
  std::int64_t local_at_receive_ms;  // this node's wall clock when the response arrived
};

enum class Outcome { within_skew, set_clock, too_slow, implausible };

struct Decision {
  Outcome outcome = Outcome::implausible;
  std::int64_t offset_ms = 0;  // core minus local: positive = this node is behind
  std::string reason;          // one line for the log
};

// Core built the response about half a round trip before it arrived. The clock is set only when it is more than
// max_skew_ms off; never from a core time older than kEarliestPlausibleMs, and not from a round trip longer than
// max_round_trip_ms (too_slow: ask again).
Decision decide(const Sample& sample, std::int64_t max_skew_ms, std::int64_t max_round_trip_ms);

}  // namespace hm::clock
