// JSONL probe for the PC parity tests (native/tests/test_vision_*.py). One JSON request per stdin line,
// one JSON answer per stdout line. Pixel/tensor outputs go to files ("out") so Python can diff them exactly.
// Test tool only: the service never writes frames, boxes or tensors anywhere.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hm/common.hpp"
#include "hm/vision/detector.hpp"
#include "hm/vision/image.hpp"
#include "hm/vision/presence.hpp"
#include "hm/vision/sources.hpp"
#include "hm/vision/zones.hpp"

using hm::json;
using ojson = nlohmann::ordered_json;
namespace hv = hm::vision;

namespace {

template <typename T>
void write_file(const std::string& path, const T* data, std::size_t count) {
  std::ofstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot write " + path);
  stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(count * sizeof(T)));
}

template <typename T>
std::vector<T> read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::runtime_error("cannot read " + path);
  const auto size = static_cast<std::size_t>(stream.tellg());
  std::vector<T> data(size / sizeof(T));
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(T)));
  return data;
}

json detections_json(const std::vector<hv::Detection>& detections) {
  json out = json::array();
  for (const auto& d : detections) {
    out.push_back({{"class_id", d.class_id}, {"confidence", d.confidence}, {"box", d.box}});
  }
  return out;
}

hv::Detection detection_from(const json& value) {
  hv::Detection detection;
  detection.class_id = value.at("class_id").get<int>();
  detection.confidence = value.at("confidence").get<double>();
  const auto box = value.at("box").get<std::vector<double>>();
  std::copy(box.begin(), box.end(), detection.box.begin());
  return detection;
}

// One ONNX session per (model, threads), reused across requests.
std::map<std::pair<std::string, int>, std::unique_ptr<hv::YoloxDetector>> g_detectors;

hv::YoloxDetector& detector(const json& request) {
  const std::string model = request.at("model").get<std::string>();
  const int threads = request.value("threads", 4);
  auto& slot = g_detectors[{model, threads}];
  if (!slot) slot = std::make_unique<hv::YoloxDetector>(hv::make_onnxruntime_backend(model, threads));
  slot->set_thresholds(request.value("score_threshold", 0.25), request.value("nms_threshold", 0.45));
  return *slot;
}

double percentile(std::vector<double> values, double q) {  // numpy.percentile (linear)
  std::sort(values.begin(), values.end());
  const double position = q / 100.0 * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(position);
  const auto upper = std::min(lower + 1, values.size() - 1);
  return values[lower] + (values[upper] - values[lower]) * (position - static_cast<double>(lower));
}

json stats(const std::vector<double>& values) {
  return {{"p50_ms", percentile(values, 50)}, {"p95_ms", percentile(values, 95)},
          {"max_ms", *std::max_element(values.begin(), values.end())}};
}

json handle(const std::string& line) {
  const json request = json::parse(line);
  const std::string op = request.at("op").get<std::string>();
  if (op == "features") {
    return {{"onnxruntime", hv::onnxruntime_available()}, {"v4l2", hv::v4l2_available()},
            {"camera1", hv::camera1_available()}};
  }
  if (op == "decode") {
    const hv::Image image = hv::read_image(request.at("path").get<std::string>());
    if (request.contains("out")) write_file(request["out"].get<std::string>(), image.bgr.data(), image.bgr.size());
    return {{"width", image.width}, {"height", image.height}};
  }
  if (op == "resize_raw") {
    hv::Image src;
    src.width = request.at("width").get<int>();
    src.height = request.at("height").get<int>();
    src.bgr = read_file<std::uint8_t>(request.at("src").get<std::string>());
    const auto dst = request.at("dst").get<std::vector<int>>();
    const hv::Image out = hv::resize_linear(src, dst.at(0), dst.at(1));
    write_file(request.at("out").get<std::string>(), out.bgr.data(), out.bgr.size());
    return {{"width", out.width}, {"height", out.height}};
  }
  if (op == "yuyv_raw") {
    const auto data = read_file<std::uint8_t>(request.at("src").get<std::string>());
    const int width = request.at("width").get<int>(), height = request.at("height").get<int>();
    const hv::Image out = hv::yuyv_to_bgr(data.data(), width, height, static_cast<std::size_t>(width) * 2);
    write_file(request.at("out").get<std::string>(), out.bgr.data(), out.bgr.size());
    return {{"width", out.width}, {"height", out.height}};
  }
  if (op == "preprocess") {
    const hv::Image image = hv::read_image(request.at("path").get<std::string>());
    const auto input = request.value("input", std::vector<int>{416, 416});
    const hv::Preprocessed pre = hv::preprocess_bgr(image, input.at(0), input.at(1));
    if (request.contains("out")) write_file(request["out"].get<std::string>(), pre.tensor.data(), pre.tensor.size());
    return {{"ratio", pre.ratio}, {"resized", {pre.resized_width, pre.resized_height}}};
  }
  if (op == "detect") {
    const hv::Image image = hv::read_image(request.at("path").get<std::string>());
    hv::YoloxDetector& yolox = detector(request);
    hv::DetectTiming timing;
    const hv::PostprocessResult result = yolox.detect(image, &timing);
    if (request.contains("raw_out")) {
      write_file(request["raw_out"].get<std::string>(), yolox.last_raw().data(), yolox.last_raw().size());
    }
    return {{"image", {image.height, image.width}},
            {"raw_shape", yolox.last_raw_shape()},
            {"detections", detections_json(result.detections)},
            {"candidates", result.candidates},
            {"best_score", result.best_score},
            {"timing", {{"preprocess_ms", timing.preprocess_ms}, {"inference_ms", timing.inference_ms},
                        {"postprocess_ms", timing.postprocess_ms}}}};
  }
  if (op == "postprocess_raw") {
    const auto raw = read_file<float>(request.at("raw").get<std::string>());
    hv::PostprocessOptions options;
    const auto input = request.value("input", std::vector<int>{416, 416});
    options.input_height = input.at(0);
    options.input_width = input.at(1);
    options.ratio = request.at("ratio").get<double>();
    const auto image = request.at("image").get<std::vector<int>>();  // [height, width]
    options.image_height = image.at(0);
    options.image_width = image.at(1);
    options.score_threshold = request.value("score_threshold", 0.25);
    options.nms_threshold = request.value("nms_threshold", 0.45);
    const std::size_t width = request.at("row_width").get<std::size_t>();
    const hv::PostprocessResult result = hv::postprocess(raw.data(), raw.size() / width, width, options);
    return {{"detections", detections_json(result.detections)}, {"candidates", result.candidates},
            {"best_score", result.best_score}};
  }
  if (op == "locate") {
    const ojson ordered = ojson::parse(line);
    const hv::ZoneMap zones = hv::ZoneMap::from_json(ordered.at("config"));
    json out = json::array();
    for (const auto& box : request.at("boxes")) {
      try {
        out.push_back(zones.locate(hv::BoundingBox::make(box.at(0).get<double>(), box.at(1).get<double>(), box.at(2).get<double>(), box.at(3).get<double>(), box.at(4).get<double>())));
      } catch (const std::invalid_argument& error) {
        out.push_back({{"error", error.what()}});
      }
    }
    return {{"zones", out}};
  }
  if (op == "zone_config") {  // validation only
    const ojson ordered = ojson::parse(line);
    const hv::ZoneMap zones = hv::ZoneMap::from_json(ordered.at("config"));
    json names = json::array();
    for (const auto& [name, polygon] : zones.zones()) names.push_back(name);
    return {{"camera_id", zones.camera_id()}, {"order", names}};
  }
  if (op == "stabilizer") {
    hv::ZoneStabilizer stabilizer(request.value("confirmation_count", 3), request.value("lost_count", 3));
    json out = json::array();
    for (const auto& item : request.at("sequence")) {
      const auto changed = stabilizer.update(item.is_null() ? std::nullopt : std::optional(item.get<std::string>()));
      out.push_back(changed ? json(*changed) : json(nullptr));
    }
    return {{"outputs", out}, {"current", stabilizer.current()}};
  }
  if (op == "summarize") {
    const ojson ordered = ojson::parse(line);
    const hv::ZoneMap zones = hv::ZoneMap::from_json(ordered.at("config"));
    hv::PostprocessResult result;
    for (const auto& item : request.at("detections")) result.detections.push_back(detection_from(item));
    result.best_score = request.value("best_score", 0.0);
    const auto image = request.at("image").get<std::vector<int>>();  // [width, height]
    const auto summary = hv::summarize(result, zones, image.at(0), image.at(1),
                                       request.value("figure_class_ids", std::vector<int>{0}));
    return {{"figure_count", summary.figure_count}, {"confidence", summary.confidence}, {"zone", summary.zone}};
  }
  if (op == "presence") {
    hv::FigurePresenceArgs args;
    args.camera_id = request.at("camera_id").get<std::string>();
    args.registry_zone = request.at("zone").get<std::string>();
    args.present = request.value("present", false);
    args.figure_count = request.value("figure_count", 0);
    args.confidence = request.value("confidence", 0.0);
    args.model_version = request.value("model_version", std::string("yolox-nano-416-figure"));
    if (request.contains("frame_age_ms")) args.frame_age_ms = request["frame_age_ms"].get<std::int64_t>();
    args.simulated = request.value("simulated", true);
    return {{"perception", hv::figure_presence(args)},
            {"observation", hv::presence_observation(request.value("ai_zone", std::string("UNKNOWN")),
                                                      args.confidence, 15000, args.simulated)}};
  }
  if (op == "bench") {
    const hv::Image image = hv::read_image(request.at("path").get<std::string>());
    hv::YoloxDetector& yolox = detector(request);
    const int runs = request.value("runs", 100), warmup = request.value("warmup", 10);
    std::vector<double> pre, inference, post, total;
    for (int i = 0; i < warmup + runs; ++i) {
      hv::DetectTiming timing;
      const auto started = std::chrono::steady_clock::now();
      yolox.detect(image, &timing);
      const double elapsed =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      if (i < warmup) continue;
      pre.push_back(timing.preprocess_ms);
      inference.push_back(timing.inference_ms);
      post.push_back(timing.postprocess_ms);
      total.push_back(elapsed);
    }
    return {{"runs", runs}, {"preprocess", stats(pre)}, {"inference", stats(inference)},
            {"postprocess", stats(post)}, {"total", stats(total)}};
  }
  throw std::invalid_argument("unknown op: " + op);
}

}  // namespace

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    json answer;
    try {
      answer = handle(line);
    } catch (const std::exception& error) {
      answer = {{"error", error.what()}};
    }
    std::cout << hm::dump_compact(answer) << std::endl;
  }
  return 0;
}
