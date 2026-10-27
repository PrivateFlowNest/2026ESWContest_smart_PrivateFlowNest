#include "hm/vision/preview.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hm::vision {

namespace {

PreviewResponse text(int status, const std::string& detail) {
  return {status, "application/json", hm::dump_compact(json{{"detail", detail}})};
}

// "::ffff:10.10.10.3" (IPv4 seen through an IPv6 socket) -> "10.10.10.3"
std::string plain_address(const std::string& address) {
  const std::string mapped = "::ffff:";
  return address.rfind(mapped, 0) == 0 ? address.substr(mapped.size()) : address;
}

}  // namespace

PreviewSettings parse_preview_settings(const json& value) {
  PreviewSettings settings;
  if (value.is_null()) return settings;
  if (!value.is_object()) throw std::invalid_argument("vision.preview must be an object");
  try {
    settings.enabled = value.value("enabled", !value.empty());
    settings.listen_host = value.value("listen_host", settings.listen_host);
    settings.port = value.value("port", settings.port);
    settings.max_age_ms = value.value("max_age_ms", settings.max_age_ms);
    if (value.contains("allowed_clients")) {
      settings.allowed_clients = value["allowed_clients"].get<std::vector<std::string>>();
    }
  } catch (const json::exception&) {
    throw std::invalid_argument("vision.preview has a value of the wrong type");
  }
  if (!settings.enabled) return settings;
  if (settings.listen_host.empty()) throw std::invalid_argument("vision.preview.listen_host must not be empty");
  if (settings.port < 1 || settings.port > 65535) throw std::invalid_argument("vision.preview.port must be 1..65535");
  if (settings.max_age_ms < 500 || settings.max_age_ms > 60000) {
    throw std::invalid_argument("vision.preview.max_age_ms must be 500..60000");
  }
  if (settings.allowed_clients.empty()) {
    throw std::invalid_argument("vision.preview.allowed_clients must name the 7-inch UI address (NODE-RPI5-01)");
  }
  for (const auto& client : settings.allowed_clients) {
    if (client.empty()) throw std::invalid_argument("vision.preview.allowed_clients has an empty address");
  }
  return settings;
}

PreviewStore::PreviewStore(const std::vector<std::string>& camera_ids) {
  for (const auto& id : camera_ids) cameras_[id] = Camera{id, false, std::nullopt};
}

void PreviewStore::put(const std::string& camera_id, std::vector<std::uint8_t> jpeg, int width, int height,
                       std::int64_t at_ms) {
  if (jpeg.empty()) return;
  std::lock_guard lock(mutex_);
  const auto camera = cameras_.find(camera_id);
  if (!camera_allowed_ || camera == cameras_.end()) return;
  camera->second.frame =
      Frame{std::make_shared<const std::vector<std::uint8_t>>(std::move(jpeg)), width, height, at_ms};
}

void PreviewStore::set_detecting(const std::string& camera_id, bool detecting) {
  std::lock_guard lock(mutex_);
  const auto camera = cameras_.find(camera_id);
  if (camera != cameras_.end()) camera->second.detecting = detecting;
}

void PreviewStore::set_camera_allowed(bool allowed) {
  std::lock_guard lock(mutex_);
  camera_allowed_ = allowed;
  if (!allowed) {
    for (auto& [id, camera] : cameras_) camera.frame.reset();
  }
}

bool PreviewStore::camera_allowed() const {
  std::lock_guard lock(mutex_);
  return camera_allowed_;
}

std::vector<PreviewStore::Camera> PreviewStore::cameras() const {
  std::lock_guard lock(mutex_);
  std::vector<Camera> out;
  for (const auto& [id, camera] : cameras_) out.push_back(camera);
  return out;
}

PreviewResponse handle_preview(const PreviewStore& store, const PreviewSettings& settings, const std::string& path,
                               const std::string& remote_addr, std::int64_t now_ms) {
  const std::string client = plain_address(remote_addr);
  const auto& allowed = settings.allowed_clients;
  if (std::find(allowed.begin(), allowed.end(), client) == allowed.end()) {
    return text(403, "this address may not see the camera preview");
  }
  const std::vector<PreviewStore::Camera> cameras = store.cameras();
  if (path == "/preview/status") {
    json list = json::array();
    for (const auto& camera : cameras) {
      json item = {{"camera_id", camera.camera_id}, {"detecting", camera.detecting}, {"frame_age_ms", nullptr},
                   {"width", 0}, {"height", 0}};
      if (camera.frame) {
        item["frame_age_ms"] = std::max<std::int64_t>(0, now_ms - camera.frame->at_ms);
        item["width"] = camera.frame->width;
        item["height"] = camera.frame->height;
      }
      list.push_back(std::move(item));
    }
    return {200, "application/json",
            hm::dump_compact(json{{"camera_allowed", store.camera_allowed()}, {"cameras", std::move(list)}})};
  }
  const std::string prefix = "/preview/";
  const std::string suffix = ".jpg";
  if (path.size() <= prefix.size() + suffix.size() || path.rfind(prefix, 0) != 0 ||
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return text(404, "unknown preview path");
  }
  const std::string id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  const auto camera = std::find_if(cameras.begin(), cameras.end(), [&id](const auto& c) { return c.camera_id == id; });
  if (camera == cameras.end()) return text(404, "unknown camera");
  if (!store.camera_allowed()) return text(423, "cameras are blocked by the privacy mode");
  if (!camera->frame) return text(404, "no frame from this camera yet");
  if (now_ms - camera->frame->at_ms > settings.max_age_ms) return text(503, "the camera has not delivered a new frame");
  const auto& bytes = *camera->frame->jpeg;
  return {200, "image/jpeg", std::string(bytes.begin(), bytes.end())};
}

}  // namespace hm::vision
