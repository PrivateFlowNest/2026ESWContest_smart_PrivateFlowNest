#include "clock_offset.hpp"

#include <cstdlib>

namespace hm::clock {

Decision decide(const Sample& sample, std::int64_t max_skew_ms, std::int64_t max_round_trip_ms) {
  Decision decision;
  if (sample.generated_at_ms < kEarliestPlausibleMs) {
    decision.reason = "core clock is not plausible (before 2025): not copied";
    return decision;
  }
  if (sample.round_trip_ms < 0 || sample.round_trip_ms > max_round_trip_ms) {
    decision.outcome = Outcome::too_slow;
    decision.reason = "round trip " + std::to_string(sample.round_trip_ms) + " ms is too slow to set the clock";
    return decision;
  }
  decision.offset_ms = sample.generated_at_ms + sample.round_trip_ms / 2 - sample.local_at_receive_ms;
  if (std::llabs(decision.offset_ms) <= max_skew_ms) {
    decision.outcome = Outcome::within_skew;
    decision.reason = "within " + std::to_string(max_skew_ms) + " ms of core (" + std::to_string(decision.offset_ms) +
                      " ms)";
    return decision;
  }
  decision.outcome = Outcome::set_clock;
  decision.reason = std::to_string(std::llabs(decision.offset_ms)) + " ms " +
                    (decision.offset_ms > 0 ? "behind" : "ahead of") + " core";
  return decision;
}

}  // namespace hm::clock
