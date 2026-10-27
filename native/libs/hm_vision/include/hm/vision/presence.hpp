// Detections -> FIGURE_PRESENCE. The Python reference stops at boxes and zones (no runtime loop), so the
// glue here is native-only and documented in README.md:
//   * the primary figure is the highest-confidence detection of a figure class with a non-degenerate box;
//     its pixel box is normalized by the image size and its foot point picks the zone (ZoneMap.locate)
//   * ZoneStabilizer (3 frames to confirm, 3 misses to drop) decides when the stable zone changes
//   * a perception is published on every stable change and every `republish_ms` (no retained messages)
//   * confidence = primary score, or 1 - best score when no figure passed the threshold
// Nothing image-derived beyond the contract attributes is published: no boxes, no coordinates (the foot point
// only reaches the local debug log that is used to fit the zones).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hm/common.hpp"
#include "hm/vision/detector.hpp"
#include "hm/vision/zones.hpp"

namespace hm::vision {

inline constexpr int kMaxFigureCount = 20;  // perception.schema.json attributes.figure_count maximum

struct FrameSummary {
  int figure_count = 0;      // figure-class detections in this frame
  double confidence = 0.0;   // see header comment
  std::string zone = "UNKNOWN";  // AI zone of the primary figure (UNKNOWN when none / outside every polygon)
  std::optional<std::pair<double, double>> foot;  // its normalized foot point: local debug log only, never published
};

FrameSummary summarize(const PostprocessResult& result, const ZoneMap& zones, int image_width, int image_height,
                       const std::vector<int>& figure_class_ids);

struct PresencePolicy {
  int confirmation_count = 3;
  int lost_count = 3;
  std::int64_t republish_ms = 10000;  // 0 disables periodic republish
};

struct PresenceUpdate {
  std::string zone;  // stable AI zone
  bool present = false;
  int figure_count = 0;
  double confidence = 0.0;
  std::string reason;  // change | republish | once
};

class CameraPresence {
 public:
  CameraPresence(std::string camera_id, PresencePolicy policy);
  // One processed frame. Returns what to publish now, if anything.
  std::optional<PresenceUpdate> update(const FrameSummary& frame, std::int64_t now_ms);
  const std::string& camera_id() const { return camera_id_; }
  const std::string& stable_zone() const { return stabilizer_.current(); }

 private:
  std::string camera_id_;
  PresencePolicy policy_;
  ZoneStabilizer stabilizer_;
  std::int64_t frames_ = 0;
  std::optional<std::int64_t> last_publish_ms_;
};

struct FigurePresenceArgs {
  std::string camera_id;      // CAM-INDOOR-01 | CAM-OUTDOOR-01
  std::string registry_zone;  // hm::ActionMap::zone(AI zone)
  bool present = false;
  int figure_count = 0;       // clamped to [0, 20]
  double confidence = 0.0;
  std::string model_version;
  std::optional<std::int64_t> frame_age_ms;
  std::optional<std::string> trace_id;
  bool simulated = false;
  std::string source_node_id = "NODE-RPI5-02";
};
// pfn.v1 perception FIGURE_PRESENCE (contracts/pfn.v1/perception.schema.json).
json figure_presence(const FigurePresenceArgs& args);

// rpi5-internal context-feature-observation PRESENCE for context-fusion (local only, never MQTT).
json presence_observation(const std::string& ai_zone, double confidence, std::int64_t valid_ms, bool simulated,
                          const std::optional<std::string>& trace_id = std::nullopt);

// UTC ISO-8601 with milliseconds ("2026-10-22T05:00:00.000Z").
std::string iso_utc_ms(std::int64_t epoch_ms);

}  // namespace hm::vision
