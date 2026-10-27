#include "hm/vision/presence.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <stdexcept>

#include "hm/pfn.hpp"

namespace hm::vision {

FrameSummary summarize(const PostprocessResult& result, const ZoneMap& zones, int image_width, int image_height,
                       const std::vector<int>& figure_class_ids) {
  if (image_width <= 0 || image_height <= 0) throw std::invalid_argument("summarize: empty image size");
  FrameSummary summary;
  std::optional<double> top;
  bool located = false;
  for (const Detection& detection : result.detections) {  // highest confidence first
    if (std::find(figure_class_ids.begin(), figure_class_ids.end(), detection.class_id) == figure_class_ids.end()) {
      continue;
    }
    ++summary.figure_count;
    if (!top) top = detection.confidence;
    if (located) continue;
    try {
      const BoundingBox box = BoundingBox::make(detection.box[0] / image_width, detection.box[1] / image_height,
                                                detection.box[2] / image_width, detection.box[3] / image_height,
                                                std::clamp(detection.confidence, 0.0, 1.0));
      summary.zone = zones.locate(box);
      summary.foot = box.foot_point();
      summary.confidence = detection.confidence;
      located = true;
    } catch (const std::invalid_argument&) {
      // degenerate box after clipping (zero width or height): counted, but cannot be placed
    }
  }
  if (!located) summary.confidence = top ? *top : 1.0 - std::clamp(result.best_score, 0.0, 1.0);
  return summary;
}

CameraPresence::CameraPresence(std::string camera_id, PresencePolicy policy)
    : camera_id_(std::move(camera_id)), policy_(policy), stabilizer_(policy.confirmation_count, policy.lost_count) {
  if (policy_.republish_ms < 0) throw std::invalid_argument("republish_ms must not be negative");
}

std::optional<PresenceUpdate> CameraPresence::update(const FrameSummary& frame, std::int64_t now_ms) {
  ++frames_;
  const auto changed = stabilizer_.update(frame.zone);
  PresenceUpdate update{stabilizer_.current(), frame.figure_count > 0, frame.figure_count, frame.confidence, ""};
  if (changed) {
    update.reason = "change";
  } else if (!last_publish_ms_ && frames_ >= policy_.confirmation_count) {
    update.reason = "initial";  // first state announcement once a full confirmation window has been seen
  } else if (last_publish_ms_ && policy_.republish_ms > 0 && now_ms - *last_publish_ms_ >= policy_.republish_ms) {
    update.reason = "republish";
  } else {
    return std::nullopt;
  }
  last_publish_ms_ = now_ms;
  return update;
}

json figure_presence(const FigurePresenceArgs& args) {
  if (args.camera_id.empty()) throw std::invalid_argument("figure_presence: camera_id is required");
  if (args.model_version.empty() || args.model_version.size() > 64) {
    throw std::invalid_argument("figure_presence: model_version must be 1..64 characters");
  }
  json attributes = {
      {"present", args.present},
      {"figure_count", std::clamp(args.figure_count, 0, kMaxFigureCount)},
      {"model_version", args.model_version},
  };
  if (args.frame_age_ms) attributes["frame_age_ms"] = std::max<std::int64_t>(0, *args.frame_age_ms);
  hm::pfn::PerceptionArgs perception;
  perception.observation_type = "FIGURE_PRESENCE";
  perception.zone = args.registry_zone;
  perception.confidence = args.confidence;
  perception.attributes = std::move(attributes);
  perception.source_device_id = args.camera_id;
  perception.trace_id = args.trace_id;
  perception.simulated = args.simulated;
  perception.source_node_id = args.source_node_id;
  return hm::pfn::perception(perception);
}

std::string iso_utc_ms(std::int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  const int millis = static_cast<int>(epoch_ms % 1000);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char text[96];
  std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
  return text;
}

json presence_observation(const std::string& ai_zone, double confidence, std::int64_t valid_ms, bool simulated,
                          const std::optional<std::string>& trace_id) {
  if (ai_zone != "UNKNOWN" && !is_allowed_zone(ai_zone)) throw std::invalid_argument("unsupported zone: " + ai_zone);
  const std::int64_t now = hm::now_ms();
  return {
      {"schema_version", "1.0"},
      {"event_id", hm::new_id()},
      {"trace_id", trace_id.value_or(hm::new_id())},
      {"timestamp", iso_utc_ms(now)},
      {"valid_until", iso_utc_ms(now + std::max<std::int64_t>(0, valid_ms))},
      {"source", "pi5.vision-presence"},
      {"source_node_id", "NODE-RPI5-02"},
      {"event_type", "PRESENCE"},
      {"simulated", simulated},
      {"payload", {{"presence_zone", ai_zone}, {"confidence", hm::confidence4(confidence)}}},
  };
}

}  // namespace hm::vision
