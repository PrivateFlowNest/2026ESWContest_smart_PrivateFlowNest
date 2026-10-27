// Camera1 D-Bus source for LG AI Native OS (com.atlas.Camera1), written with sd-bus from the call sequence of
// docs/ai-native-os-sample/cameraapp: PermissionAgent1 user consent -> DeviceList -> Open -> SetFormat
// (width/height/fps 'u', format 's') -> StartPreview(handle, 0) -> wait -> Capture(handle, 1, dir) per frame.
// Capture writes a JPEG into capture_dir; the file is read and deleted immediately (frames never persist).
// Ran on NODE-RPI5-02 2026-10-25 (two USB webcams, root and the service account with the perm_camera* groups). On the
// PC only option handling, device choice and the capture folder are tested: there is no Camera1 service off the device.
#include <stdexcept>
#include <string>

#include "hm/vision/sources.hpp"

#ifdef __linux__
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#ifdef HM_VISION_HAVE_SDBUS
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>
#endif

namespace hm::vision {

CaptureDir::CaptureDir(std::filesystem::path dir, int mode, const std::string& group) {
  if (dir.empty()) throw std::invalid_argument("camera1 capture_dir is required");
  dir_ = std::filesystem::absolute(dir).lexically_normal();
#ifdef __linux__
  gid_t gid = static_cast<gid_t>(-1);
  if (!group.empty()) {
    const struct group* entry = ::getgrnam(group.c_str());
    if (!entry) throw std::invalid_argument("capture_dir_group " + group + " does not exist");
    gid = entry->gr_gid;
  }
  for (auto path = dir_; !std::filesystem::exists(path); path = path.parent_path()) {
    created_.push_back(path);
    if (path == path.parent_path()) break;
  }
  std::filesystem::create_directories(dir_);
  const auto set = [&](const std::filesystem::path& path, int folder_mode) {
    if (gid != static_cast<gid_t>(-1) && ::chown(path.c_str(), static_cast<uid_t>(-1), gid) != 0) {
      throw std::runtime_error("capture folder " + path.string() + ": group " + group + ": " + std::strerror(errno) +
                               " (the folder must belong to this account and the account must be in that group)");
    }
    if (::chmod(path.c_str(), static_cast<mode_t>(folder_mode)) != 0) {
      throw std::runtime_error("capture folder " + path.string() + ": mode: " + std::strerror(errno) +
                               " (the folder must belong to this account: remove it and start again)");
    }
  };
  try {
    set(dir_, mode);
    if (gid != static_cast<gid_t>(-1)) {
      for (const auto& parent : created_) {
        if (parent != dir_) set(parent, 0710);
      }
    }
    // Leftovers from an earlier crash: this folder is ours, delete any image in it.
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
      const auto extension = entry.path().extension().string();
      if (entry.is_regular_file() && (extension == ".jpg" || extension == ".jpeg" || extension == ".JPG")) {
        std::filesystem::remove(entry.path());
      }
    }
  } catch (...) {
    std::error_code ignored;
    for (const auto& path : created_) std::filesystem::remove(path, ignored);
    throw;
  }
#else
  (void)mode;
  if (!group.empty()) throw std::invalid_argument("capture_dir_group needs Linux");
  std::filesystem::create_directories(dir_);
#endif
}

CaptureDir::~CaptureDir() {
  std::error_code ignored;  // a folder that is not empty (or not ours) stays
  for (const auto& path : created_) std::filesystem::remove(path, ignored);
}

#ifdef HM_VISION_HAVE_SDBUS
namespace {

constexpr const char* kService = "com.atlas.Camera1";
constexpr const char* kPath = "/com/atlas/Camera1";
constexpr const char* kInterface = "com.atlas.Camera1";
constexpr const char* kPermService = "com.atlas.PermissionAgent1";
constexpr const char* kPermPath = "/com/atlas/PermissionAgent1";
constexpr const char* kPermInterface = "com.atlas.PermissionAgent1";
const char* const kCameraPermissions[] = {
    "com.atlas.permission.user_consent.cameradevice",
    "com.atlas.permission.user_consent.camerastream",
    "com.atlas.permission.user_consent.cameradescriptor",
    "com.atlas.permission.user_consent.cameracontrol",
};

struct BusError {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  ~BusError() { sd_bus_error_free(&error); }
  std::string text(const std::string& what) const {
    return what + ": " + (error.name ? error.name : "?") + " " + (error.message ? error.message : "");
  }
};

struct Message {
  sd_bus_message* message = nullptr;
  ~Message() { sd_bus_message_unref(message); }
};

void check(int result, const std::string& what) {
  if (result < 0) throw std::runtime_error(what + ": " + std::strerror(-result));
}

class Camera1Source final : public FrameSource {
 public:
  explicit Camera1Source(const Camera1Options& options) : options_(options) {
    check(sd_bus_open_system(&bus_), "sd_bus_open_system");
    try {
      capture_dir_ = std::make_unique<CaptureDir>(options_.capture_dir, options_.capture_dir_mode,
                                                  options_.capture_dir_group);
      open_camera();
    } catch (...) {
      shutdown();
      throw;
    }
  }
  ~Camera1Source() override { shutdown(); }

  std::optional<Frame> grab() override {
    pump();
    if (fault_) {
      hm::log(LogLevel::warning, "vision.camera1", "reopening " + device_id_ + " after a Camera1 fault/removal");
      close_camera();
      open_camera();
    }
    BusError error;
    Message reply;
    const std::string target = capture_dir_->path().string() + "/";
    if (sd_bus_call_method(bus_, kService, kPath, kInterface, "Capture", &error.error, &reply.message, "iis",
                           handle_, 1, target.c_str()) < 0) {
      fault_ = true;
      throw std::runtime_error(error.text("Camera1.Capture"));
    }
    char** files = nullptr;
    check(sd_bus_message_read_strv(reply.message, &files), "Capture reply");
    std::vector<std::string> paths;
    for (char** item = files; item && *item; ++item) {
      paths.emplace_back(*item);
      std::free(*item);
    }
    std::free(files);
    if (paths.empty()) throw std::runtime_error("Camera1.Capture returned no file");
    std::vector<std::uint8_t> bytes;
    {
      std::ifstream stream(paths.front(), std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    for (const auto& path : paths) ::unlink(path.c_str());  // the frame must not stay on disk
    if (bytes.empty()) throw std::runtime_error("Camera1 capture file was empty or unreadable");
    Frame frame;
    frame.image = decode_image(bytes.data(), bytes.size());
    frame.captured_at_ms = hm::now_ms();
    frame.encoded = std::move(bytes);
    return frame;
  }

  bool live() const override { return true; }
  std::string describe() const override { return "camera1:" + device_id_; }

 private:
  static int on_signal(sd_bus_message* message, void* self, sd_bus_error*) {
    auto* source = static_cast<Camera1Source*>(self);
    const char* camera = nullptr;
    if (sd_bus_message_read(message, "s", &camera) >= 0 && camera && source->device_id_ == camera) {
      source->fault_ = true;
    }
    return 0;
  }

  void pump() {
    while (sd_bus_process(bus_, nullptr) > 0) {
    }
  }

  enum class Consent { granted, needed, denied };

  Consent consent(const char* permission) {
    BusError error;
    Message reply;
    if (sd_bus_call_method(bus_, kPermService, kPermPath, kPermInterface, "CheckSelfUserConsentPermission",
                           &error.error, &reply.message, "s", permission) >= 0) {
      return Consent::granted;
    }
    const std::string name = error.error.name ? error.error.name : "";
    return name.find("UserConsentDenied") != std::string::npos ? Consent::denied : Consent::needed;
  }

  // GetUserConsentPermissions([""]) -> aa{sv}; the entry whose app_id is ours carries app_uid.
  std::string app_uid() {
    BusError error;
    Message reply;
    if (sd_bus_call_method(bus_, kPermService, kPermPath, kPermInterface, "GetUserConsentPermissions", &error.error,
                           &reply.message, "as", 1, "") >= 0 &&
        sd_bus_message_enter_container(reply.message, 'a', "a{sv}") > 0) {
      while (sd_bus_message_enter_container(reply.message, 'a', "{sv}") > 0) {
        std::string id, uid;
        while (sd_bus_message_enter_container(reply.message, 'e', "sv") > 0) {
          const char* key = nullptr;
          check(sd_bus_message_read(reply.message, "s", &key), "permission entry");
          const std::string name = key ? key : "";
          char type = 0;
          const char* contents = nullptr;
          check(sd_bus_message_peek_type(reply.message, &type, &contents), "permission entry");
          if ((name == "app_id" || name == "app_uid") && contents && std::strcmp(contents, "s") == 0) {
            const char* value = nullptr;
            check(sd_bus_message_read(reply.message, "v", "s", &value), "permission entry");
            (name == "app_id" ? id : uid) = value ? value : "";
          } else {
            check(sd_bus_message_skip(reply.message, "v"), "permission entry");
          }
          sd_bus_message_exit_container(reply.message);
        }
        sd_bus_message_exit_container(reply.message);
        if (id == options_.app_id && !uid.empty()) return uid;
      }
    }
    std::ifstream status("/proc/self/status");  // sample fallback: numeric uid
    std::string line;
    while (std::getline(status, line)) {
      if (line.rfind("Uid:", 0) == 0) {
        std::istringstream fields(line.substr(4));
        std::string uid;
        fields >> uid;
        return uid;
      }
    }
    return {};
  }

  void ensure_permissions() {
    std::vector<const char*> needed;
    for (const char* permission : kCameraPermissions) {
      const Consent state = consent(permission);
      if (state == Consent::denied) throw std::runtime_error(std::string("camera permission denied: ") + permission);
      if (state == Consent::needed) needed.push_back(permission);
    }
    if (needed.empty()) return;
    const std::string uid = app_uid();
    if (uid.empty()) throw std::runtime_error("cannot resolve the app uid for the camera permission grant");
    for (const char* permission : needed) {
      BusError error;
      Message reply;
      if (sd_bus_call_method(bus_, kPermService, kPermPath, kPermInterface, "UpdateUserConsentPermission",
                             &error.error, &reply.message, "ssi", uid.c_str(), permission, 1) < 0) {
        hm::log(LogLevel::warning, "vision.camera1", error.text("UpdateUserConsentPermission"));
      }
    }
    for (const char* permission : kCameraPermissions) {
      if (consent(permission) != Consent::granted) {
        throw std::runtime_error(std::string("camera permission still not granted: ") + permission);
      }
    }
  }

  struct DeviceInfo {
    std::string name;     // GetDeviceInfo "name"
    std::string summary;  // " key=value ..." of the text and true/false entries, for the log
  };

  DeviceInfo device_info(const std::string& id) {
    DeviceInfo info;
    BusError error;
    Message reply;
    if (sd_bus_call_method(bus_, kService, kPath, kInterface, "GetDeviceInfo", &error.error, &reply.message, "s",
                           id.c_str()) < 0 ||
        sd_bus_message_enter_container(reply.message, 'a', "{sv}") <= 0) {
      return info;
    }
    while (sd_bus_message_enter_container(reply.message, 'e', "sv") > 0) {
      const char* key = nullptr;
      char type = 0;
      const char* contents = nullptr;
      if (sd_bus_message_read(reply.message, "s", &key) < 0 ||
          sd_bus_message_peek_type(reply.message, &type, &contents) < 0) {
        break;
      }
      const std::string name = key ? key : "?";
      info.summary += " " + name;
      if (contents && std::strcmp(contents, "s") == 0) {
        const char* value = nullptr;
        if (sd_bus_message_read(reply.message, "v", "s", &value) >= 0 && value) {
          info.summary += std::string("=") + value;
          if (name == "name") info.name = value;
        }
      } else if (contents && std::strcmp(contents, "b") == 0) {
        int value = 0;
        if (sd_bus_message_read(reply.message, "v", "b", &value) >= 0) info.summary += value ? "=true" : "=false";
      } else {
        sd_bus_message_skip(reply.message, "v");
      }
      sd_bus_message_exit_container(reply.message);
    }
    return info;
  }

  void open_camera() {
    fault_ = false;
    if (options_.request_permissions) ensure_permissions();
    {
      BusError error;
      Message reply;
      // com.atlas.Camera1 is D-Bus activated and not running after a boot until someone calls it (real Pi5-02,
      // 2026-10-25: GetNameOwner said "no such name"). StartServiceByName starts it, or finds it running.
      if (sd_bus_call_method(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                             "StartServiceByName", &error.error, &reply.message, "su", kService, 0u) < 0) {
        throw std::runtime_error(error.text("Camera1 service not available"));
      }
    }
    {
      BusError error;
      char** devices = nullptr;
      if (sd_bus_get_property_strv(bus_, kService, kPath, kInterface, "DeviceList", &error.error, &devices) < 0) {
        throw std::runtime_error(error.text("Camera1.DeviceList"));
      }
      std::vector<std::string> list;
      for (char** item = devices; item && *item; ++item) {
        list.emplace_back(*item);
        std::free(*item);
      }
      std::free(devices);
      std::string all;
      for (const auto& id : list) all += " " + id;
      hm::log(LogLevel::info, "vision.camera1", "DeviceList:" + (all.empty() ? std::string(" (empty)") : all));
      std::vector<std::string> names;
      if (options_.device_id.empty() && !options_.device_name.empty()) {
        for (const auto& id : list) names.push_back(device_info(id).name);
      }
      device_id_ = select_camera1_device(list, names, options_);
    }
    hm::log(LogLevel::info, "vision.camera1", "device " + device_id_ + ":" + device_info(device_id_).summary);
    {
      BusError error;
      Message reply;
      if (sd_bus_call_method(bus_, kService, kPath, kInterface, "Open", &error.error, &reply.message, "s",
                             device_id_.c_str()) < 0) {
        throw std::runtime_error(error.text("Camera1.Open"));
      }
      check(sd_bus_message_read(reply.message, "i", &handle_), "Open reply");
    }
    try {
      BusError error;
      Message reply;
      if (sd_bus_call_method(bus_, kService, kPath, kInterface, "SetFormat", &error.error, &reply.message, "ia{sv}",
                             handle_, 4, "width", "u", static_cast<std::uint32_t>(options_.width), "height", "u",
                             static_cast<std::uint32_t>(options_.height), "fps", "u",
                             static_cast<std::uint32_t>(options_.fps), "format", "s", options_.format.c_str()) < 0) {
        throw std::runtime_error(error.text("Camera1.SetFormat"));
      }
      BusError preview_error;
      Message preview_reply;
      if (sd_bus_call_method(bus_, kService, kPath, kInterface, "StartPreview", &preview_error.error,
                             &preview_reply.message, "ii", handle_, 0) < 0) {
        throw std::runtime_error(preview_error.text("Camera1.StartPreview"));
      }
      preview_ = true;
    } catch (...) {
      close_camera();
      throw;
    }
    if (!fault_slot_) {
      check(sd_bus_match_signal(bus_, &fault_slot_, kService, kPath, kInterface, "FaultEvent", on_signal, this),
            "match FaultEvent");
      check(sd_bus_match_signal(bus_, &removed_slot_, kService, kPath, kInterface, "DeviceRemoved", on_signal, this),
            "match DeviceRemoved");
    }
    // Let the pipeline settle like the sample does before the first Capture.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.warmup_ms);
    while (std::chrono::steady_clock::now() < until) {
      pump();
      sd_bus_wait(bus_, 100000);
    }
    hm::log(LogLevel::info, "vision.camera1",
            "opened " + device_id_ + " handle=" + std::to_string(handle_) + " " + std::to_string(options_.width) + "x" +
                std::to_string(options_.height) + "@" + std::to_string(options_.fps) + " " + options_.format);
  }

  void close_camera() {
    if (handle_ < 0) return;
    if (preview_) {
      BusError error;
      Message reply;
      sd_bus_call_method(bus_, kService, kPath, kInterface, "StopPreview", &error.error, &reply.message, "i", handle_);
      preview_ = false;
    }
    BusError error;
    Message reply;
    sd_bus_call_method(bus_, kService, kPath, kInterface, "Close", &error.error, &reply.message, "i", handle_);
    handle_ = -1;
  }

  void shutdown() {
    if (!bus_) return;
    close_camera();
    fault_slot_ = sd_bus_slot_unref(fault_slot_);
    removed_slot_ = sd_bus_slot_unref(removed_slot_);
    bus_ = sd_bus_flush_close_unref(bus_);
  }

  Camera1Options options_;
  std::unique_ptr<CaptureDir> capture_dir_;
  sd_bus* bus_ = nullptr;
  sd_bus_slot* fault_slot_ = nullptr;
  sd_bus_slot* removed_slot_ = nullptr;
  std::string device_id_;
  std::int32_t handle_ = -1;
  bool preview_ = false;
  bool fault_ = false;
};

}  // namespace

bool camera1_available() { return true; }
std::unique_ptr<FrameSource> make_camera1_source(const Camera1Options& options) {
  return std::make_unique<Camera1Source>(options);
}
#else
bool camera1_available() { return false; }
std::unique_ptr<FrameSource> make_camera1_source(const Camera1Options&) {
  throw std::runtime_error("hm_vision was built without sd-bus (libsystemd-dev); Camera1 is unavailable");
}
#endif

}  // namespace hm::vision
