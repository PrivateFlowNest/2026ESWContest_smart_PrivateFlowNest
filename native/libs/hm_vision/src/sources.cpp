#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <vector>

#include "hm/vision/sources.hpp"

namespace hm::vision {

namespace {

bool is_image_file(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".jpg" || extension == ".jpeg" || extension == ".png";
}

class ImageSource final : public FrameSource {
 public:
  ImageSource(const std::filesystem::path& path, bool loop) : path_(path), loop_(loop) {
    if (std::filesystem::is_directory(path)) {
      for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_regular_file() && is_image_file(entry.path())) files_.push_back(entry.path());
      }
      std::sort(files_.begin(), files_.end());
    } else if (std::filesystem::is_regular_file(path)) {
      files_.push_back(path);
    }
    if (files_.empty()) throw std::runtime_error("no JPEG/PNG images at " + path.string());
  }

  std::optional<Frame> grab() override {
    if (next_ >= files_.size()) {
      if (!loop_) return std::nullopt;
      next_ = 0;
    }
    Frame frame;
    frame.image = read_image(files_[next_++]);
    frame.captured_at_ms = hm::now_ms();
    return frame;
  }

  bool live() const override { return false; }
  std::string describe() const override {
    return "images:" + path_.string() + " (" + std::to_string(files_.size()) + " files)";
  }

 private:
  std::filesystem::path path_;
  bool loop_;
  std::vector<std::filesystem::path> files_;
  std::size_t next_ = 0;
};

template <typename T>
T get_or(const json& spec, const char* key, T fallback) {
  return spec.contains(key) && !spec[key].is_null() ? spec[key].get<T>() : fallback;
}

}  // namespace

std::unique_ptr<FrameSource> make_image_source(const std::filesystem::path& path, bool loop) {
  return std::make_unique<ImageSource>(path, loop);
}

std::unique_ptr<FrameSource> make_source(const json& spec, const std::filesystem::path& base_dir) {
  if (!spec.is_object() || !spec.contains("type")) throw std::invalid_argument("camera source needs a type");
  const std::string type = spec["type"].get<std::string>();
  if (type == "images") {
    std::filesystem::path path = spec.at("path").get<std::string>();
    if (path.is_relative()) path = (base_dir / path).lexically_normal();
    return make_image_source(path, get_or(spec, "loop", false));
  }
  if (type == "v4l2") {
    V4l2Options options;
    options.device = get_or(spec, "device", options.device);
    options.width = get_or(spec, "width", options.width);
    options.height = get_or(spec, "height", options.height);
    options.fps = get_or(spec, "fps", options.fps);
    options.pixel_format = get_or(spec, "pixel_format", options.pixel_format);
    options.buffers = get_or(spec, "buffers", options.buffers);
    options.timeout_ms = get_or(spec, "timeout_ms", options.timeout_ms);
    return make_v4l2_source(options);
  }
  if (type == "camera1") return make_camera1_source(parse_camera1_options(spec));
  throw std::invalid_argument("unknown camera source type: " + type);
}

Camera1Options parse_camera1_options(const json& spec) {
  Camera1Options options;
  options.device_id = get_or(spec, "device_id", options.device_id);
  options.device_name = get_or(spec, "device_name", options.device_name);
  options.device_index = get_or(spec, "device_index", options.device_index);
  options.width = get_or(spec, "width", options.width);
  options.height = get_or(spec, "height", options.height);
  options.fps = get_or(spec, "fps", options.fps);
  options.format = get_or(spec, "format", options.format);
  options.warmup_ms = get_or(spec, "warmup_ms", options.warmup_ms);
  options.capture_dir = get_or(spec, "capture_dir", options.capture_dir.string());
  if (spec.contains("capture_dir_mode")) options.capture_dir_mode = std::stoi(spec["capture_dir_mode"].get<std::string>(), nullptr, 8);
  options.capture_dir_group = get_or(spec, "capture_dir_group", options.capture_dir_group);
  options.request_permissions = get_or(spec, "request_permissions", options.request_permissions);
  options.app_id = get_or(spec, "app_id", options.app_id);
  return options;
}

std::string select_camera1_device(const std::vector<std::string>& ids, const std::vector<std::string>& names,
                                  const Camera1Options& options) {
  if (!options.device_id.empty()) {
    if (std::find(ids.begin(), ids.end(), options.device_id) == ids.end()) {
      throw std::runtime_error("Camera1 device not in DeviceList: " + options.device_id);
    }
    return options.device_id;
  }
  if (!options.device_name.empty()) {
    if (names.size() != ids.size()) throw std::invalid_argument("select_camera1_device: one name per DeviceList entry");
    const auto lower = [](std::string text) {
      std::transform(text.begin(), text.end(), text.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    };
    const std::string wanted = lower(options.device_name);
    std::vector<std::size_t> matches;
    std::string list;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (lower(names[i]).find(wanted) != std::string::npos) matches.push_back(i);
      list += " " + ids[i] + "=" + names[i];
    }
    if (matches.size() == 1) return ids[matches.front()];
    throw std::runtime_error(std::string(matches.empty() ? "no" : "more than one") + " Camera1 device named like \"" +
                             options.device_name + "\" (DeviceList:" + (list.empty() ? " empty" : list) + ")");
  }
  if (options.device_index < 0 || static_cast<std::size_t>(options.device_index) >= ids.size()) {
    throw std::runtime_error("Camera1 device_index out of range");
  }
  return ids[static_cast<std::size_t>(options.device_index)];
}

}  // namespace hm::vision
