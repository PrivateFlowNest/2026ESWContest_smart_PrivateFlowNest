// homemind-vision settings: the "vision" block of the runtime config file (hm::RuntimeConfig extra).
// Relative paths resolve against RuntimeConfig::base_dir() (repository root on the PC build, the config
// file's folder on the device).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "hm/common.hpp"
#include "hm/config.hpp"
#include "hm/vision/background.hpp"
#include "hm/vision/preview.hpp"

namespace hm::vision_service {

struct CameraSettings {
  std::string camera_id;  // CAM-INDOOR-01 | CAM-OUTDOOR-01
  std::filesystem::path zones;
  json source;            // {"type": "camera1"|"v4l2"|"images", ...}
  int interval_ms = 0;    // 0 -> VisionSettings::interval_ms
  // detector "background": the empty scene (written by --capture-background; default
  // vision-background-<CAMERA>.png) and normalized polygons left out of the comparison (status LEDs, screens)
  std::filesystem::path background_image;
  std::vector<hm::vision::Polygon> ignore;
};

struct VisionSettings {
  // "yolox": trained FIGURE model (model_path). "background": change against the empty scene inside the zones,
  // no model (one-axis doll model; hm/vision/background.hpp).
  std::string detector = "yolox";
  hm::vision::BackgroundOptions background;
  std::filesystem::path model_path;
  std::string model_version = "yolox-nano-416-figure";
  int threads = 2;
  double score_threshold = 0.25;
  double nms_threshold = 0.45;
  std::vector<int> figure_class_ids{0};
  int interval_ms = 200;  // 5 fps per camera (config/model-selection.json target_fps_per_camera)
  int confirmation_count = 3;
  int lost_count = 3;
  double republish_seconds = 10.0;
  std::optional<std::filesystem::path> presence_fifo;  // local PRESENCE observations for context-fusion
  double presence_ttl_seconds = 15.0;
  // Cameras whose stable zone feeds context-fusion through presence_fifo. Fusion keeps one PRESENCE value (the
  // newest), so a second camera would overwrite the room position with its own zone (OUTDOOR / UNKNOWN) every
  // republish. Default: the room camera only.
  std::vector<std::string> presence_cameras{"CAM-INDOOR-01"};
  // Live preview for the 7-inch UI over the home router (hm/vision/preview.hpp). Off unless configured.
  hm::vision::PreviewSettings preview;
  std::vector<CameraSettings> cameras;
};

inline const std::vector<std::string> kCameraIds = {"CAM-INDOOR-01", "CAM-OUTDOOR-01"};

// Throws std::invalid_argument with the offending key.
VisionSettings resolve_settings(const hm::RuntimeConfig& config);

}  // namespace hm::vision_service
