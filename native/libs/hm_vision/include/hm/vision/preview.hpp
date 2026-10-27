// Live camera preview for the 7-inch UI (NODE-RPI5-01) inside the home. User rule 2026-10-25: raw video may travel
// between the home nodes over the router, never to an outside server; it never goes over MQTT and is never stored.
// The store keeps only the newest JPEG of each camera as the camera delivered it (no re-encoding); the vision service
// answers HTTP requests from allowed_clients only:
//   GET /preview/<CAMERA-ID>.jpg  200 image/jpeg | 403 other client | 404 unknown camera or no frame yet
//                                 | 423 cameras blocked by the privacy mode | 503 newest frame older than max_age_ms
//   GET /preview/status           200 {"camera_allowed": bool, "cameras": [{"camera_id", "detecting",
//                                 "frame_age_ms" (null: none), "width", "height"}]}
// "detecting" is false while a camera has no empty scene yet (preview only, no presence messages).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hm/common.hpp"

namespace hm::vision {

struct PreviewSettings {
  bool enabled = false;
  std::string listen_host = "0.0.0.0";
  int port = 8091;
  std::vector<std::string> allowed_clients;  // IPv4 text, e.g. "10.10.10.3" (NODE-RPI5-01)
  int max_age_ms = 5000;
};

// The vision config's "preview" object ({} or absent: off). Throws std::invalid_argument on bad values.
PreviewSettings parse_preview_settings(const json& value);

class PreviewStore {
 public:
  struct Frame {
    std::shared_ptr<const std::vector<std::uint8_t>> jpeg;
    int width = 0;
    int height = 0;
    std::int64_t at_ms = 0;
  };
  struct Camera {
    std::string camera_id;
    bool detecting = false;
    std::optional<Frame> frame;
  };

  explicit PreviewStore(const std::vector<std::string>& camera_ids);

  // Newest frame of a camera. Dropped for an unknown camera, an empty JPEG or while cameras are blocked.
  void put(const std::string& camera_id, std::vector<std::uint8_t> jpeg, int width, int height, std::int64_t at_ms);
  void set_detecting(const std::string& camera_id, bool detecting);
  // pfn/v1/system/privacy/state camera_allowed. false also forgets every stored frame.
  void set_camera_allowed(bool allowed);
  bool camera_allowed() const;
  std::vector<Camera> cameras() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, Camera> cameras_;
  bool camera_allowed_ = true;
};

struct PreviewResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

// One HTTP request (path and the client's address as the server saw it).
PreviewResponse handle_preview(const PreviewStore& store, const PreviewSettings& settings, const std::string& path,
                               const std::string& remote_addr, std::int64_t now_ms);

}  // namespace hm::vision
