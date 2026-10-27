#include <gtest/gtest.h>

#include "clock_offset.hpp"

using hm::clock::decide;
using hm::clock::Outcome;

namespace {
constexpr std::int64_t kCore = 1792829746000LL;  // 2026-10-24
}

TEST(ClockOffset, NodeBehindCoreIsSetForward) {
  // Real Pi5-01 after a power cycle: 154 s behind. Core built the answer half of a 40 ms round trip before it arrived.
  const auto decision = decide({kCore, 40, kCore + 20 - 154000}, 1000, 2000);
  EXPECT_EQ(decision.outcome, Outcome::set_clock);
  EXPECT_EQ(decision.offset_ms, 154000);
  EXPECT_EQ(decision.reason, "154000 ms behind core");
}

TEST(ClockOffset, NodeAheadOfCoreIsSetBack) {
  const auto decision = decide({kCore, 10, kCore + 5 + 3000}, 1000, 2000);
  EXPECT_EQ(decision.outcome, Outcome::set_clock);
  EXPECT_EQ(decision.offset_ms, -3000);
  EXPECT_EQ(decision.reason, "3000 ms ahead of core");
}

TEST(ClockOffset, SmallSkewLeavesTheClock) {
  const auto decision = decide({kCore, 30, kCore + 15 + 800}, 1000, 2000);
  EXPECT_EQ(decision.outcome, Outcome::within_skew);
  EXPECT_EQ(decision.offset_ms, -800);
}

TEST(ClockOffset, SlowAnswerAsksAgain) {
  EXPECT_EQ(decide({kCore, 2500, kCore}, 1000, 2000).outcome, Outcome::too_slow);
  EXPECT_EQ(decide({kCore, -1, kCore}, 1000, 2000).outcome, Outcome::too_slow);
}

TEST(ClockOffset, ResetCoreClockIsNeverCopied) {
  // A Pi4 that lost its time (1970 / 2024) must not drag the nodes back.
  const auto decision = decide({hm::clock::kEarliestPlausibleMs - 1, 10, kCore}, 1000, 2000);
  EXPECT_EQ(decision.outcome, Outcome::implausible);
  EXPECT_EQ(decision.offset_ms, 0);
}
