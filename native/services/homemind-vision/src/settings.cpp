#include "settings.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace hm::vision_service {

namespace {

template <typename T>
T read(const json& block, const char* key, T fallback) {
  if (!block.contains(key) || block[key].is_null()) return fallback;
  try {
    return block[key].get<T>();
  } catch (const json::exception&) {
    throw std::invalid_argument(std::string("vision.") + key + " has the wrong type");
  }
}

std::vector<hm::vision::Polygon> read_polygons(const json& item, const std::string& where) {
  std::vector<hm::vision::Polygon> polygons;
  if (!item.contains("ignore") || item["ignore"].is_null()) return polygons;
  try {
    for (const auto& polygon : item["ignore"]) polygons.push_back(polygon.get<hm::vision::Polygon>());
  } catch (const json::exception&) {
    throw std::invalid_argument(where + ".ignore must be a list of polygons [[x, y], ...]");
  }
  return polygons;
}

}  // namespace

VisionSettings resolve_settings(const hm::RuntimeConfig& config) {
  if (!config.extra.contains("vision") || !config.extra["vision"].is_object()) {
    throw std::invalid_argument("config needs a \"vision\" object");
  }
  const json& block = config.extra["vision"];
  static const std::set<std::string> known = {
      "detector", "background", "model_path", "model_version", "threads", "score_threshold", "nms_threshold",
      "figure_class_ids",
      "interval_ms", "confirmation_count", "lost_count", "republish_seconds", "presence_fifo",
      "presence_ttl_seconds", "presence_cameras", "preview", "cameras"};
  for (const auto& [key, value] : block.items()) {
    if (!known.count(key)) hm::log(LogLevel::warning, "vision", "ignoring unknown setting vision." + key);
  }
  VisionSettings settings;
  settings.detector = read(block, "detector", settings.detector);
  if (settings.detector != "yolox" && settings.detector != "background") {
    throw std::invalid_argument("vision.detector must be \"yolox\" or \"background\"");
  }
  const bool background = settings.detector == "background";
  const std::string model = read<std::string>(block, "model_path", "");
  if (model.empty() && !background) throw std::invalid_argument("vision.model_path is required");
  if (!model.empty()) settings.model_path = config.resolve_path(model);
  if (background) {
    settings.model_version = "background-diff-v1";
    const json options = read(block, "background", json::object());
    if (!options.is_object()) throw std::invalid_argument("vision.background must be an object");
    auto& tuning = settings.background;
    tuning.work_width = read(options, "work_width", tuning.work_width);
    tuning.threshold = read(options, "threshold", tuning.threshold);
    tuning.min_area_ratio = read(options, "min_area_ratio", tuning.min_area_ratio);
    tuning.max_area_ratio = read(options, "max_area_ratio", tuning.max_area_ratio);
    tuning.max_gain = read(options, "max_gain", tuning.max_gain);
  }
  settings.model_version = read(block, "model_version", settings.model_version);
  if (settings.model_version.empty() || settings.model_version.size() > 64) {
    throw std::invalid_argument("vision.model_version must be 1..64 characters");
  }
  settings.threads = read(block, "threads", settings.threads);
  settings.score_threshold = read(block, "score_threshold", settings.score_threshold);
  settings.nms_threshold = read(block, "nms_threshold", settings.nms_threshold);
  settings.figure_class_ids = background ? std::vector<int>{0}  // blobs are class 0
                                         : read(block, "figure_class_ids", settings.figure_class_ids);
  settings.interval_ms = read(block, "interval_ms", settings.interval_ms);
  settings.confirmation_count = read(block, "confirmation_count", settings.confirmation_count);
  settings.lost_count = read(block, "lost_count", settings.lost_count);
  settings.republish_seconds = read(block, "republish_seconds", settings.republish_seconds);
  settings.presence_ttl_seconds = read(block, "presence_ttl_seconds", settings.presence_ttl_seconds);
  const std::string fifo = read<std::string>(block, "presence_fifo", "");
  if (!fifo.empty()) settings.presence_fifo = config.resolve_path(fifo);
  settings.presence_cameras = read(block, "presence_cameras", settings.presence_cameras);
  settings.preview = hm::vision::parse_preview_settings(block.contains("preview") ? block["preview"] : json());
  for (const auto& id : settings.presence_cameras) {
    if (std::find(kCameraIds.begin(), kCameraIds.end(), id) == kCameraIds.end()) {
      throw std::invalid_argument("vision.presence_cameras: camera ids must be CAM-INDOOR-01 or CAM-OUTDOOR-01");
    }
  }
  if (!(settings.presence_ttl_seconds > 0.0)) throw std::invalid_argument("vision.presence_ttl_seconds must be > 0");
  if (settings.threads < 1) throw std::invalid_argument("vision.threads must be >= 1");
  if (!(settings.score_threshold >= 0.0 && settings.score_threshold <= 1.0)) {
    throw std::invalid_argument("vision.score_threshold must be within [0, 1]");
  }
  if (!(settings.nms_threshold >= 0.0 && settings.nms_threshold <= 1.0)) {
    throw std::invalid_argument("vision.nms_threshold must be within [0, 1]");
  }
  if (settings.figure_class_ids.empty()) throw std::invalid_argument("vision.figure_class_ids must not be empty");
  if (settings.interval_ms < 0) throw std::invalid_argument("vision.interval_ms must be >= 0");
  if (settings.confirmation_count < 1 || settings.lost_count < 1) {
    throw std::invalid_argument("vision.confirmation_count and vision.lost_count must be >= 1");
  }
  if (settings.republish_seconds < 0) throw std::invalid_argument("vision.republish_seconds must be >= 0");

  if (!block.contains("cameras") || !block["cameras"].is_array() || block["cameras"].empty()) {
    throw std::invalid_argument("vision.cameras must be a non-empty list");
  }
  std::set<std::string> seen;
  for (const auto& item : block["cameras"]) {
    if (!item.is_object()) throw std::invalid_argument("vision.cameras entries must be objects");
    if (!read(item, "enabled", true)) continue;
    CameraSettings camera;
    camera.camera_id = read<std::string>(item, "camera_id", "");
    if (std::find(kCameraIds.begin(), kCameraIds.end(), camera.camera_id) == kCameraIds.end()) {
      throw std::invalid_argument("vision.cameras: camera_id must be CAM-INDOOR-01 or CAM-OUTDOOR-01");
    }
    if (!seen.insert(camera.camera_id).second) throw std::invalid_argument("vision.cameras: duplicate " + camera.camera_id);
    const std::string zones = read<std::string>(item, "zones", "");
    if (zones.empty()) throw std::invalid_argument("vision.cameras[" + camera.camera_id + "].zones is required");
    camera.zones = config.resolve_path(zones);
    if (!item.contains("source") || !item["source"].is_object()) {
      throw std::invalid_argument("vision.cameras[" + camera.camera_id + "].source is required");
    }
    camera.source = item["source"];
    camera.interval_ms = read(item, "interval_ms", 0);
    const std::string empty_scene = "vision-background-" + camera.camera_id + ".png";
    camera.background_image = config.resolve_path(read<std::string>(item, "background_image", empty_scene));
    camera.ignore = read_polygons(item, "vision.cameras[" + camera.camera_id + "]");
    settings.cameras.push_back(std::move(camera));
  }
  if (settings.cameras.empty()) throw std::invalid_argument("vision.cameras: every camera is disabled");
  return settings;
}

}  // namespace hm::vision_service
