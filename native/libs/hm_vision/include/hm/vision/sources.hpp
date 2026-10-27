// Frame sources. Frames stay in this process: they are decoded to BGR, used once and dropped.
//   images  - one file or a directory of *.jpg/*.jpeg/*.png (sorted by name), PC tests and offline runs
//   v4l2    - Linux V4L2 capture (MJPEG or YUYV, mmap streaming), PC webcam fallback
//   camera1 - LG AI Native OS Camera1 D-Bus service (com.atlas.Camera1), the device path. Follows
//             docs/ai-native-os-sample/cameraapp (Open, SetFormat, StartPreview(0), Capture). Ran on NODE-RPI5-02
//             2026-10-25 with two USB webcams: the caller needs the D-Bus groups perm_cameradevice,
//             perm_camerastream and perm_cameracontrol (root has all), and Camera1 writes each JPEG as the user
//             `camera`, so the capture folder must let that group in (CaptureDir).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hm/common.hpp"
#include "hm/vision/image.hpp"

namespace hm::vision {

struct Frame {
  Image image;
  std::int64_t captured_at_ms = 0;  // wall clock (epoch ms) of the exposure, best effort
  std::vector<std::uint8_t> encoded;  // the camera's own JPEG when it delivers one (camera1): the home preview
};

class FrameSource {
 public:
  virtual ~FrameSource() = default;
  // Next frame; nullopt when a finite source (images) is exhausted. Throws on capture errors.
  virtual std::optional<Frame> grab() = 0;
  virtual bool live() const = 0;  // real camera (results are not simulated)
  virtual std::string describe() const = 0;
};

std::unique_ptr<FrameSource> make_image_source(const std::filesystem::path& path, bool loop = false);

struct V4l2Options {
  std::string device = "/dev/video0";
  int width = 640;
  int height = 480;
  int fps = 15;
  std::string pixel_format = "MJPG";  // MJPG | YUYV
  int buffers = 4;
  int timeout_ms = 2000;
};
std::unique_ptr<FrameSource> make_v4l2_source(const V4l2Options& options);
bool v4l2_available();

struct Camera1Options {
  std::string device_id;    // exact entry of the DeviceList property, else
  std::string device_name;  // the one entry whose GetDeviceInfo name contains this (any case), else
  int device_index = 0;     // the position in DeviceList (plug-in order: can change after a power cycle)
  int width = 640;
  int height = 480;
  int fps = 30;
  std::string format = "JPEG";
  int warmup_ms = 5000;  // the sample waits 5 s after StartPreview before Capture
  std::filesystem::path capture_dir = "/tmp/homemind-vision";
  int capture_dir_mode = 0700;
  std::string capture_dir_group;  // Atlas: "camera" (Camera1 writes as that user); empty: leave the group as is
  bool request_permissions = true;  // PermissionAgent1 user-consent check/grant like the sample
  std::string app_id = "com.homemind.service.vision";
};
Camera1Options parse_camera1_options(const json& spec);
// The DeviceList entry `options` asks for; names[i] is the GetDeviceInfo name of ids[i] (needed for device_name only).
// Throws when the id is missing, no name or more than one name matches, or the index is out of range.
std::string select_camera1_device(const std::vector<std::string>& ids, const std::vector<std::string>& names,
                                  const Camera1Options& options);

// The folder Camera1 writes each frame into. The folder gets `mode`, and with a `group` it and the parent folders made
// here get that group, the parents 0710: only that group can put files in, nobody else can look. Parents that already
// exist are left alone. Leftover JPEGs in the folder are deleted. The destructor removes the folders it made once
// they are empty, so a root run (--capture-background) leaves nothing the service account cannot use. Give each camera
// its own folder right under /tmp: a parent shared by two folders stays behind when its maker finishes first.
class CaptureDir {
 public:
  CaptureDir(std::filesystem::path dir, int mode, const std::string& group);
  ~CaptureDir();
  CaptureDir(const CaptureDir&) = delete;
  CaptureDir& operator=(const CaptureDir&) = delete;
  const std::filesystem::path& path() const { return dir_; }

 private:
  std::filesystem::path dir_;
  std::vector<std::filesystem::path> created_;  // deepest first
};
std::unique_ptr<FrameSource> make_camera1_source(const Camera1Options& options);
bool camera1_available();  // built with sd-bus (libsystemd)

// {"type": "images"|"v4l2"|"camera1", ...options}. Relative image paths resolve against base_dir.
std::unique_ptr<FrameSource> make_source(const json& spec, const std::filesystem::path& base_dir);

}  // namespace hm::vision
