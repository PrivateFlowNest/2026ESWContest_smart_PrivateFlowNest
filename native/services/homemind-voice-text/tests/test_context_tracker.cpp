#include <gtest/gtest.h>

#include "context_tracker.hpp"

namespace {

hm::json perception(const char* type, std::int64_t valid_until_ms, const char* zone = "desk") {
  return {{"observation_type", type},
          {"zone", zone},
          {"confidence", 0.91},
          {"attributes",
           {{"activity", "MEETING"}, {"audio_context", "ZOOM_SPEAKER"}, {"quiet_required", true},
            {"transition", "ACTIVATED"}, {"evidence", {"AUDIO_CONTEXT:ZOOM_SPEAKER"}}, {"valid_until_ms", valid_until_ms},
            {"model_version", "v6"}}}};
}

TEST(ContextTracker, LatestActivityContextUntilItExpires) {
  hm::ContextTracker tracker;
  EXPECT_TRUE(tracker.current().is_null());
  tracker.update("pfn/v1/ai/perception", perception("FIGURE_PRESENCE", hm::now_ms() + 60000));
  EXPECT_TRUE(tracker.current().is_null());
  tracker.update("pfn/v1/ai/perception", perception("ACTIVITY_CONTEXT", hm::now_ms() + 60000));
  const hm::json expected = {{"presence_zone", "DESK"}, {"activity", "MEETING"}, {"audio_context", "ZOOM_SPEAKER"},
                             {"quiet_required", true}, {"confidence", 0.91}};
  EXPECT_EQ(tracker.current(), expected);
  tracker.update("pfn/v1/ai/perception", perception("ACTIVITY_CONTEXT", hm::now_ms() - 1, "living"));
  EXPECT_TRUE(tracker.current().is_null());
  tracker.update("pfn/v1/ai/perception", perception("ACTIVITY_CONTEXT", hm::now_ms() + 60000, "garage"));
  EXPECT_EQ(tracker.current()["presence_zone"], "UNKNOWN");
}

TEST(ContextTracker, MalformedDocumentsThrowLikePython) {
  hm::ContextTracker tracker;
  hm::json missing = perception("ACTIVITY_CONTEXT", hm::now_ms() + 60000);
  missing["attributes"].erase("activity");
  EXPECT_THROW(tracker.update("t", missing), std::exception);
  EXPECT_TRUE(tracker.current().is_null());
}

}  // namespace
