// homemind-vision: NODE-RPI5-02 figure presence. Cameras (Camera1 D-Bus on the device, V4L2 or image files on
// the PC) -> detector -> foot-point zone -> stabilizer -> pfn/v1/ai/perception FIGURE_PRESENCE. Detector
// (vision.detector): "yolox" = trained YOLOX-Nano FIGURE model (ONNX Runtime), "background" = change against the
// empty scene inside the zones, no model (one-axis doll model). Frames never leave the process; messages carry
// presence, count, zone and confidence only (the empty-scene PNG stays on the device).
//
//   homemind-vision --config /etc/homemind/vision.json
//   homemind-vision --config cfg.json --capture-background [--camera CAM-ID]   (figure out of view; writes the PNG)
//   homemind-vision --config cfg.json --images DIR --stdout            (offline, simulated)
//   homemind-vision --config cfg.json --once --stdout                   (one frame per camera, smoke check)
#include <fcntl.h>
#include <httplib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "hm/action_map.hpp"
#include "hm/common.hpp"
#include "hm/config.hpp"
#include "hm/mqtt.hpp"
#include "hm/pfn.hpp"
#include "hm/vision/background.hpp"
#include "hm/vision/detector.hpp"
#include "hm/vision/image.hpp"
#include "hm/vision/presence.hpp"
#include "hm/vision/preview.hpp"
#include "hm/vision/sources.hpp"
#include "hm/vision/zones.hpp"
#include "settings.hpp"

namespace hv = hm::vision;
using hm::json;
using hm::LogLevel;

namespace {

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop = true; }

struct Options {
  std::string config;
  std::string log_level = "INFO";
  bool stdout_publish = false;
  bool once = false;
  std::optional<std::string> images;
  std::optional<std::string> camera;
  long max_frames = 0;
  bool realtime = false;
  bool capture_background = false;
  int capture_frames = 15;
};

void usage() {
  std::fprintf(stderr,
               "usage: homemind-vision --config FILE [--log-level LEVEL] [--stdout] [--once]\n"
               "                       [--images PATH [--camera CAM-ID]] [--max-frames N] [--realtime]\n"
               "       homemind-vision --config FILE --capture-background [--frames N] [--camera CAM-ID]\n"
               "  --capture-background  detector \"background\": store the per-pixel median of N frames (default 15)\n"
               "                  as the camera's empty scene (background_image). Take the figure out of view first\n"
               "  --images PATH   replace camera sources with a JPEG/PNG file or directory (results simulated)\n"
               "  --once          one frame per camera, publish its unstabilized result, exit\n"
               "  --realtime      pace image sources at interval_ms (default: as fast as possible)\n");
}

Options parse(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(arg + " needs a value");
      return argv[++i];
    };
    if (arg == "--config") options.config = value();
    else if (arg == "--log-level") options.log_level = value();
    else if (arg == "--stdout") options.stdout_publish = true;
    else if (arg == "--once") options.once = true;
    else if (arg == "--images") options.images = value();
    else if (arg == "--camera") options.camera = value();
    else if (arg == "--max-frames") options.max_frames = std::stol(value());
    else if (arg == "--realtime") options.realtime = true;
    else if (arg == "--capture-background") options.capture_background = true;
    else if (arg == "--frames") options.capture_frames = std::stoi(value());
    else if (arg == "-h" || arg == "--help") {
      usage();
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument " + arg);
    }
  }
  if (options.config.empty()) throw std::invalid_argument("--config is required");
  if (options.capture_frames < 1 || options.capture_frames > 300) {
    throw std::invalid_argument("--frames must be 1..300");
  }
  return options;
}

std::int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class Publisher {
 public:
  // on_privacy: pfn/v1/system/privacy/state documents (camera_allowed gates the home preview).
  Publisher(const hm::RuntimeConfig& config, bool to_stdout, hm::Bus::Handler on_privacy = {}) {
    if (to_stdout) return;
    bus_ = std::make_unique<hm::Bus>(config.broker, "homemind-vision-" + config.perception_node_id);
    if (on_privacy) bus_->subscribe("pfn/v1/system/privacy/state", std::move(on_privacy));
    if (!bus_->start(10000)) hm::log(LogLevel::warning, "vision", "MQTT not connected yet; retrying in background");
  }
  ~Publisher() {
    if (!bus_) return;
    // QoS 1 messages of a finite run (--images/--once) are still in flight right after the last publish.
    if (published_) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    bus_->stop();
  }
  void publish(const std::string& topic, const json& document) {
    if (!bus_) {
      std::cout << topic << ' ' << hm::dump_compact(document) << std::endl;
      return;
    }
    if (bus_->publish(topic, document, 1)) {
      ++published_;
    } else {
      hm::log(LogLevel::warning, "vision", "publish refused for " + topic);
    }
  }

 private:
  std::unique_ptr<hm::Bus> bus_;
  long published_ = 0;
};

// PRESENCE observations for context-fusion over a local FIFO (non-blocking; dropped when nobody reads).
class PresenceFifo {
 public:
  explicit PresenceFifo(std::optional<std::filesystem::path> path) : path_(std::move(path)) {
    if (!path_) return;
    struct stat info{};
    if (::stat(path_->c_str(), &info) != 0) {
      if (::mkfifo(path_->c_str(), 0600) != 0) {
        throw std::runtime_error("mkfifo " + path_->string() + ": " + std::strerror(errno));
      }
    } else if (!S_ISFIFO(info.st_mode)) {
      throw std::runtime_error("presence_fifo exists and is not a FIFO: " + path_->string());
    }
  }
  void write(const json& document) {
    if (!path_) return;
    const int fd = ::open(path_->c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;  // ENXIO: no reader yet
    const std::string line = hm::dump_compact(document) + "\n";
    if (::write(fd, line.data(), line.size()) < 0) hm::log(LogLevel::debug, "vision", "presence fifo write failed");
    ::close(fd);
  }

 private:
  std::optional<std::filesystem::path> path_;
};

// HTTP server of the home preview; stops and joins on every way out of run().
struct PreviewHttp {
  httplib::Server server;
  std::thread thread;
  ~PreviewHttp() {
    server.stop();
    if (thread.joinable()) thread.join();
  }
};

struct Camera {
  hm::vision_service::CameraSettings settings;
  std::unique_ptr<hv::ZoneMap> zones;
  std::unique_ptr<hv::FrameSource> source;
  std::unique_ptr<hv::CameraPresence> presence;
  std::shared_ptr<hv::FrameDetector> detector;
  hv::BackgroundDetector* background = nullptr;  // detector "background": stats for the logs
  std::string unusable;                          // last logged reason the scene could not be judged
  json source_spec;
  int interval_ms = 0;
  std::int64_t next_due = 0;
  long frames = 0;
  int failures = 0;
  bool done = false;
  bool live = false;
  double inference_ms = 0.0;
};

// --capture-background: per camera, the per-pixel median of N frames (paced at interval_ms for live cameras)
// becomes the empty scene of the "background" detector.
int capture_background(std::vector<Camera>& cameras, const Options& options) {
  int exit_code = 0;
  for (auto& camera : cameras) {
    const auto& id = camera.settings.camera_id;
    if (!camera.source) {
      hm::log(LogLevel::error, "vision", id + ": camera not available, empty scene not captured");
      exit_code = 1;
      continue;
    }
    std::vector<hv::Image> frames;
    while (static_cast<int>(frames.size()) < options.capture_frames && !g_stop) {
      auto frame = camera.source->grab();
      if (!frame) break;  // image source exhausted
      const hv::Image& image = frame->image;
      if (!frames.empty() && (image.width != frames.front().width || image.height != frames.front().height)) {
        throw std::runtime_error(id + ": frames of different sizes; capture needs one camera resolution");
      }
      frames.push_back(std::move(frame->image));
      if (camera.live) std::this_thread::sleep_for(std::chrono::milliseconds(std::max(camera.interval_ms, 100)));
    }
    if (frames.empty()) {
      hm::log(LogLevel::error, "vision", id + ": no frame captured");
      exit_code = 1;
      continue;
    }
    const auto& path = camera.settings.background_image;
    hv::write_png(hv::median_image(frames), path);
    hm::log(LogLevel::info, "vision",
            id + ": empty scene from " + std::to_string(frames.size()) + " frames (" +
                std::to_string(frames.front().width) + "x" + std::to_string(frames.front().height) + ") -> " +
                path.string());
  }
  return exit_code;
}

int run(const Options& options) {
  hm::set_log_level(hm::parse_log_level(options.log_level));
  const hm::RuntimeConfig config = hm::RuntimeConfig::load(options.config);
  const auto settings = hm::vision_service::resolve_settings(config);
  const hm::ActionMap action_map(config.contracts_dir);
  for (const auto& camera : settings.cameras) {
    const json* device = action_map.device(camera.camera_id);
    if (!device || device->value("controller_node_id", "") != config.perception_node_id) {
      throw std::invalid_argument(camera.camera_id + " is not attached to " + config.perception_node_id +
                                  " in device-registry.json");
    }
  }

  std::shared_ptr<hv::FrameDetector> yolox;
  if (settings.detector == "yolox" && !options.capture_background) {
    auto detector = std::make_shared<hv::YoloxDetector>(
        hv::make_onnxruntime_backend(settings.model_path, settings.threads), settings.score_threshold,
        settings.nms_threshold);
    hm::log(LogLevel::info, "vision",
            "model " + settings.model_path.filename().string() + " (" + settings.model_version + ") input " +
                std::to_string(detector->input_width()) + "x" + std::to_string(detector->input_height()) + ", " +
                std::to_string(settings.threads) + " threads");
    yolox = std::move(detector);
  }

  std::vector<Camera> cameras;
  for (const auto& camera_settings : settings.cameras) {
    if (options.camera && *options.camera != camera_settings.camera_id) continue;
    Camera camera;
    camera.settings = camera_settings;
    camera.zones = std::make_unique<hv::ZoneMap>(hv::ZoneMap::load(camera_settings.zones));
    if (camera.zones->camera_id() != camera_settings.camera_id) {
      throw std::invalid_argument(camera_settings.zones.string() + " is for " + camera.zones->camera_id() + ", not " +
                                  camera_settings.camera_id);
    }
    camera.source_spec = options.images ? json{{"type", "images"}, {"path", *options.images}} : camera_settings.source;
    camera.interval_ms = camera_settings.interval_ms > 0 ? camera_settings.interval_ms : settings.interval_ms;
    camera.presence = std::make_unique<hv::CameraPresence>(
        camera_settings.camera_id,
        hv::PresencePolicy{settings.confirmation_count, settings.lost_count,
                           static_cast<std::int64_t>(settings.republish_seconds * 1000.0)});
    camera.detector = yolox;
    const bool preview_only = settings.detector == "background" && !options.capture_background &&
                              !std::filesystem::exists(camera_settings.background_image) &&
                              settings.preview.enabled && !options.once && !options.images;
    if (preview_only) {
      // The cameras are being mounted or aimed: show them on the 7-inch UI, judge nothing until the empty scene.
      hm::log(LogLevel::warning, "vision",
              camera_settings.camera_id + ": no empty scene " + camera_settings.background_image.filename().string() +
                  " yet: preview only, no presence until homemind-vision --capture-background and a restart");
      camera.detector.reset();
    } else if (settings.detector == "background" && !options.capture_background) {
      const auto& path = camera_settings.background_image;
      if (!std::filesystem::exists(path)) {
        throw std::runtime_error("no empty-scene image " + path.string() + ": take the figure out of view and run "
                                 "homemind-vision --config " + options.config + " --capture-background");
      }
      std::vector<hv::Polygon> regions;
      for (const auto& zone : camera.zones->zones()) regions.push_back(zone.second);
      auto detector = std::make_shared<hv::BackgroundDetector>(hv::read_image(path), std::move(regions),
                                                               camera_settings.ignore, settings.background);
      camera.background = detector.get();
      camera.detector = std::move(detector);
      hm::log(LogLevel::info, "vision",
              camera_settings.camera_id + " background detector, empty scene " + path.filename().string() + ", " +
                  std::to_string(camera_settings.ignore.size()) + " ignored area(s)");
    }
    cameras.push_back(std::move(camera));
  }
  if (cameras.empty()) throw std::invalid_argument("no camera selected");

  std::vector<std::string> camera_ids;
  for (const auto& camera : cameras) camera_ids.push_back(camera.settings.camera_id);
  hv::PreviewStore preview(camera_ids);
  for (const auto& camera : cameras) preview.set_detecting(camera.settings.camera_id, camera.detector != nullptr);
  const bool serve_preview = settings.preview.enabled && !options.once && !options.images && !options.capture_background;
  std::unique_ptr<PreviewHttp> preview_http;
  if (serve_preview) {
    preview_http = std::make_unique<PreviewHttp>();
    preview_http->server.Get(R"(/preview/.*)", [&preview, &settings](const httplib::Request& req,
                                                                       httplib::Response& res) {
      const auto answer = hv::handle_preview(preview, settings.preview, req.path, req.remote_addr, hm::now_ms());
      res.status = answer.status;
      res.set_header("Cache-Control", "no-store");
      res.set_content(answer.body, answer.content_type);
    });
    if (!preview_http->server.bind_to_port(settings.preview.listen_host, settings.preview.port)) {
      hm::log(LogLevel::error, "vision",
              "preview port " + std::to_string(settings.preview.port) + " cannot be opened: no 7-inch preview");
      preview_http.reset();
    } else {
      preview_http->thread = std::thread([server = &preview_http->server] { server->listen_after_bind(); });
      std::string clients;
      for (const auto& client : settings.preview.allowed_clients) clients += " " + client;
      hm::log(LogLevel::info, "vision",
              "home preview on " + settings.preview.listen_host + ":" + std::to_string(settings.preview.port) +
                  " for" + clients + " (newest JPEG per camera, not stored, never over MQTT)");
    }
  }
  hm::Bus::Handler on_privacy;
  if (serve_preview) {
    on_privacy = [&preview](const std::string&, const json& document) {
      if (!document.is_object() || !document.contains("camera_allowed") || !document["camera_allowed"].is_boolean()) {
        return;
      }
      const bool allowed = document["camera_allowed"].get<bool>();
      if (allowed != preview.camera_allowed()) {
        hm::log(LogLevel::info, "vision", allowed ? "privacy mode: cameras allowed, preview on"
                                                  : "privacy mode: cameras blocked, preview off and frames dropped");
      }
      preview.set_camera_allowed(allowed);
    };
  }
  Publisher publisher(config, options.stdout_publish, on_privacy);
  PresenceFifo fifo(settings.presence_fifo);
  if (settings.presence_fifo) {
    std::string feeding;
    for (const auto& camera : cameras) {
      const auto& ids = settings.presence_cameras;
      if (std::find(ids.begin(), ids.end(), camera.settings.camera_id) != ids.end()) {
        feeding += (feeding.empty() ? "" : ", ") + camera.settings.camera_id;
      }
    }
    hm::log(LogLevel::info, "vision",
            "presence_fifo " + settings.presence_fifo->string() + " <- " + (feeding.empty() ? "no camera" : feeding) +
                " (context-fusion PRESENCE)");
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto open_source = [&](Camera& camera) {
    try {
      camera.source = hv::make_source(camera.source_spec, config.base_dir());
      camera.live = camera.source->live();
      hm::log(LogLevel::info, "vision", camera.settings.camera_id + " <- " + camera.source->describe());
      return true;
    } catch (const std::exception& error) {
      if (camera.source_spec.value("type", "") == "images") throw;  // configuration error, not a device fault
      camera.failures += 1;
      const int backoff = std::min(30000, 1000 << std::min(camera.failures, 5));
      hm::log(LogLevel::error, "vision", camera.settings.camera_id + " open failed (retry in " +
                                             std::to_string(backoff / 1000) + " s): " + error.what());
      camera.next_due = steady_ms() + backoff;
      return false;
    }
  };
  for (auto& camera : cameras) open_source(camera);
  if (options.capture_background) return capture_background(cameras, options);

  int exit_code = 0;
  std::int64_t last_stats = steady_ms();
  while (!g_stop) {
    bool active = false;
    std::int64_t next_wake = steady_ms() + 1000;
    for (auto& camera : cameras) {
      if (camera.done) continue;
      active = true;
      const std::int64_t now = steady_ms();
      if (now < camera.next_due) {
        next_wake = std::min(next_wake, camera.next_due);
        continue;
      }
      if (!camera.source && !open_source(camera)) {
        if (options.once) {
          camera.done = true;
          exit_code = 1;
        }
        next_wake = std::min(next_wake, camera.next_due);
        continue;
      }
      std::optional<hv::Frame> frame;
      try {
        frame = camera.source->grab();
      } catch (const std::exception& error) {
        camera.failures += 1;
        hm::log(LogLevel::warning, "vision", camera.settings.camera_id + " capture failed: " + error.what());
        if (!camera.live) throw;
        if (options.once) {
          camera.done = true;
          exit_code = 1;
          continue;
        }
        if (camera.failures % 3 == 0) camera.source.reset();  // reopen after repeated failures
        camera.next_due = now + std::min(30000, 500 << std::min(camera.failures, 6));
        continue;
      }
      if (!frame) {
        camera.done = true;
        continue;
      }
      camera.failures = 0;
      if (serve_preview) {
        preview.put(camera.settings.camera_id, std::move(frame->encoded), frame->image.width, frame->image.height,
                    hm::now_ms());
      }
      if (!camera.detector) {  // preview only: no empty scene yet
        camera.next_due = std::max(camera.next_due + camera.interval_ms, now);
        next_wake = std::min(next_wake, camera.next_due);
        continue;
      }
      hv::DetectTiming timing;
      const auto result = camera.detector->detect(frame->image, &timing);
      if (camera.background) {
        const auto& stats = camera.background->last_stats();
        const std::string reason = stats.unusable ? stats.reason : "";
        if (reason != camera.unusable) {
          hm::log(reason.empty() ? LogLevel::info : LogLevel::warning, "vision",
                  camera.settings.camera_id +
                      (reason.empty() ? std::string(" scene usable again")
                                      : " scene cannot be judged (" + reason +
                                            "): no figure reported until it settles; capture the empty scene again "
                                            "if it persists"));
          camera.unusable = reason;
        }
      }
      camera.inference_ms += timing.inference_ms;
      const auto summary = hv::summarize(result, *camera.zones, frame->image.width, frame->image.height,
                                         settings.figure_class_ids);
      camera.frames += 1;
      const std::int64_t wall = hm::now_ms();
      std::optional<hv::PresenceUpdate> update;
      if (options.once) {
        update = hv::PresenceUpdate{summary.zone, summary.figure_count > 0, summary.figure_count, summary.confidence,
                                    "once"};
      } else {
        update = camera.presence->update(summary, wall);
      }
      // foot=x,y (0..1, bottom centre of the primary figure) is what zones.*.json polygons are fitted to.
      char foot[32] = "";
      if (summary.foot) std::snprintf(foot, sizeof foot, " foot=%.2f,%.2f", summary.foot->first, summary.foot->second);
      hm::log(LogLevel::debug, "vision",
              camera.settings.camera_id + " frame " + std::to_string(camera.frames) + " figures=" +
                  std::to_string(summary.figure_count) + " zone=" + summary.zone + foot + " stable=" +
                  camera.presence->stable_zone() + " infer_ms=" + std::to_string(timing.inference_ms));
      if (update) {
        hv::FigurePresenceArgs args;
        args.camera_id = camera.settings.camera_id;
        args.registry_zone = action_map.zone(update->zone);
        args.present = update->present;
        args.figure_count = update->figure_count;
        args.confidence = update->confidence;
        args.model_version = settings.model_version;
        args.frame_age_ms = wall - frame->captured_at_ms;
        args.simulated = config.simulated || !camera.live;
        args.source_node_id = config.perception_node_id;
        const json message = hv::figure_presence(args);
        publisher.publish(hm::pfn::kPerception, message);
        const auto& feeds = settings.presence_cameras;
        if (std::find(feeds.begin(), feeds.end(), camera.settings.camera_id) != feeds.end()) {
          fifo.write(hv::presence_observation(update->zone, update->confidence,
                                              static_cast<std::int64_t>(settings.presence_ttl_seconds * 1000.0),
                                              args.simulated, message["trace_id"].get<std::string>()));
        }
        hm::log(LogLevel::info, "vision",
                camera.settings.camera_id + " " + update->reason + ": zone=" + args.registry_zone +
                    " present=" + (update->present ? "true" : "false") + " count=" + std::to_string(update->figure_count));
      }
      if (options.once || (options.max_frames > 0 && camera.frames >= options.max_frames)) camera.done = true;
      const bool paced = camera.live || options.realtime;
      camera.next_due = paced ? std::max(camera.next_due + camera.interval_ms, now) : now;
      next_wake = std::min(next_wake, camera.next_due);
    }
    if (!active) break;
    if (steady_ms() - last_stats >= 60000) {
      last_stats = steady_ms();
      for (const auto& camera : cameras) {
        hm::log(LogLevel::info, "vision",
                camera.settings.camera_id + " frames=" + std::to_string(camera.frames) + " mean_infer_ms=" +
                    std::to_string(camera.frames ? camera.inference_ms / static_cast<double>(camera.frames) : 0.0));
      }
    }
    const std::int64_t wait = next_wake - steady_ms();
    if (wait > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(wait, 200)));
  }
  for (const auto& camera : cameras) {
    hm::log(LogLevel::info, "vision", camera.settings.camera_id + " processed " + std::to_string(camera.frames) + " frames");
  }
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(parse(argc, argv));
  } catch (const std::invalid_argument& error) {
    hm::log(LogLevel::error, "vision", error.what());
    usage();
    return 2;
  } catch (const std::exception& error) {
    hm::log(LogLevel::error, "vision", error.what());
    return 1;
  }
}
