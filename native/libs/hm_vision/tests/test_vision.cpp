// Unit tests for hm_vision (no camera, PC only). The Python parity checks live in native/tests/test_vision_*.py.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hm/vision/background.hpp"
#include "hm/vision/detector.hpp"
#include "hm/vision/image.hpp"
#include "hm/vision/presence.hpp"
#include "hm/vision/preview.hpp"
#include "hm/vision/sources.hpp"
#include "hm/vision/zones.hpp"

#ifdef __linux__
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hv = hm::vision;
namespace fs = std::filesystem;
using hm::json;

namespace {

const fs::path kRepo = HM_TEST_REPO_ROOT;
const fs::path kVision = kRepo / "services/edge-ai/vision-presence";

hv::Image pattern(int width, int height) {
  hv::Image image = hv::make_image(width, height);
  for (std::size_t i = 0; i < image.bgr.size(); ++i) image.bgr[i] = static_cast<std::uint8_t>((i * 37 + i / 7) & 0xFF);
  return image;
}

// Python: zones template polygons (config/zones.template.json order).
hv::ZoneMap two_halves() {
  return hv::ZoneMap("CAM-INDOOR-01", {{"BED", {{0, 0}, {0.5, 0}, {0.5, 1}, {0, 1}}},
                                       {"DESK", {{0.5, 0}, {1, 0}, {1, 1}, {0.5, 1}}}});
}

}  // namespace

TEST(Resize, SameSizeIsACopy) {
  const hv::Image src = pattern(17, 9);
  EXPECT_EQ(hv::resize_linear(src, 17, 9).bgr, src.bgr);
}

TEST(Resize, ExactHalfAveragesTwoByTwo) {
  hv::Image src = hv::make_image(4, 2);
  // pixel (0,0)=0 (1,0)=1 (0,1)=2 (1,1)=3 in every channel -> (0+1+2+3+2)>>2 = 2
  const int values[2][4] = {{0, 1, 10, 10}, {2, 3, 10, 12}};
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      for (int c = 0; c < 3; ++c) src.row(y)[x * 3 + c] = static_cast<std::uint8_t>(values[y][x]);
    }
  }
  const hv::Image out = hv::resize_linear(src, 2, 1);
  EXPECT_EQ(out.bgr[0], 2);
  EXPECT_EQ(out.bgr[3], 11);  // (10+10+10+12+2)>>2
}

TEST(Resize, UpscaleKeepsConstantImage) {
  hv::Image src = hv::make_image(5, 3, 77);
  const hv::Image out = hv::resize_linear(src, 13, 8);
  for (auto value : out.bgr) EXPECT_EQ(value, 77);
}

TEST(Yuyv, Bt601Extremes) {
  const std::uint8_t black[4] = {16, 128, 16, 128};
  const std::uint8_t white[4] = {235, 128, 235, 128};
  EXPECT_EQ(hv::yuyv_to_bgr(black, 2, 1, 4).bgr, std::vector<std::uint8_t>(6, 0));
  EXPECT_EQ(hv::yuyv_to_bgr(white, 2, 1, 4).bgr, std::vector<std::uint8_t>(6, 255));
  EXPECT_THROW(hv::yuyv_to_bgr(black, 1, 1, 4), std::invalid_argument);
}

TEST(Orientation, RotateNinetyClockwise) {
  // 2x1 image A B -> orientation 6 (transpose + horizontal flip) -> 1x2 column A over B
  hv::Image src = hv::make_image(2, 1);
  src.bgr = {1, 1, 1, 2, 2, 2};
  const hv::Image out = hv::apply_orientation(src, 6);
  ASSERT_EQ(out.width, 1);
  ASSERT_EQ(out.height, 2);
  EXPECT_EQ(out.bgr, (std::vector<std::uint8_t>{1, 1, 1, 2, 2, 2}));
  const hv::Image flipped = hv::apply_orientation(src, 2);
  EXPECT_EQ(flipped.bgr, (std::vector<std::uint8_t>{2, 2, 2, 1, 1, 1}));
  EXPECT_EQ(hv::apply_orientation(src, 1).bgr, src.bgr);
}

TEST(Preprocess, LetterboxShapeAndPadding) {
  const hv::Image image = hv::make_image(640, 480, 0);
  const hv::Preprocessed pre = hv::preprocess_bgr(image, 416, 416);
  EXPECT_EQ(pre.tensor.size(), 3u * 416 * 416);
  EXPECT_DOUBLE_EQ(pre.ratio, 0.65);  // test_preprocess_preserves_fixed_nchw_shape
  EXPECT_EQ(pre.resized_width, 416);
  EXPECT_EQ(pre.resized_height, 312);
  EXPECT_EQ(pre.tensor[0], 0.0f);
  EXPECT_EQ(pre.tensor[415 * 416 + 3], 114.0f);  // padding rows at the bottom
}

TEST(Postprocess, OneHighConfidencePrediction) {
  // test_decode_and_postprocess_one_high_confidence_prediction
  std::vector<float> raw(3549 * 7, 0.0f);
  raw[0] = 5.0f;
  raw[1] = 5.0f;
  raw[4] = 0.9f;
  raw[5] = 0.1f;
  raw[6] = 0.8f;
  hv::PostprocessOptions options;
  options.image_height = 416;
  options.image_width = 416;
  options.score_threshold = 0.5;
  const auto result = hv::postprocess(raw.data(), 3549, 7, options);
  ASSERT_EQ(result.detections.size(), 1u);
  EXPECT_EQ(result.detections[0].class_id, 1);
  EXPECT_NEAR(result.detections[0].confidence, 0.72, 1e-6);
  // centre (5+0)*8 = 40, size exp(0)*8 = 8 -> 36..44
  EXPECT_NEAR(result.detections[0].box[0], 36.0, 1e-6);
  EXPECT_NEAR(result.detections[0].box[3], 44.0, 1e-6);
  EXPECT_EQ(result.candidates, 1u);
}

TEST(Postprocess, NmsKeepsTheHigherOfTwoOverlappingBoxes) {
  std::vector<float> raw(3549 * 6, 0.0f);
  auto set = [&](int index, float dx, float score) {
    float* row = raw.data() + index * 6;
    row[0] = dx;
    row[1] = 0.5f;
    row[2] = 1.0f;
    row[3] = 1.0f;
    row[4] = score;
    row[5] = 1.0f;
  };
  set(10, 0.5f, 0.6f);
  set(11, -0.4f, 0.9f);  // neighbouring cell, almost the same box
  set(200, 0.5f, 0.7f);  // far away
  hv::PostprocessOptions options;
  options.image_height = 416;
  options.image_width = 416;
  const auto result = hv::postprocess(raw.data(), 3549, 6, options);
  ASSERT_EQ(result.detections.size(), 2u);
  EXPECT_NEAR(result.detections[0].confidence, 0.9, 1e-6);
  EXPECT_NEAR(result.detections[1].confidence, 0.7, 1e-6);
  EXPECT_EQ(result.candidates, 3u);
}

TEST(Postprocess, RejectsWrongProposalCount) {
  std::vector<float> raw(100 * 6, 0.0f);
  EXPECT_THROW(hv::postprocess(raw.data(), 100, 6, {}), std::invalid_argument);
  EXPECT_THROW(hv::postprocess(raw.data(), 3549, 5, {}), std::invalid_argument);
}

TEST(Zones, BottomCenterSelectsZone) {
  const auto zones = two_halves();
  EXPECT_EQ(zones.locate(hv::BoundingBox::make(0.1, 0.1, 0.3, 0.9, 0.95)), "BED");
  EXPECT_EQ(zones.locate(hv::BoundingBox::make(0.7, 0.1, 0.9, 0.9, 0.95)), "DESK");
  const hv::ZoneMap outdoor("CAM-OUTDOOR-01", {{"OUTDOOR", {{0, 0}, {0.4, 0}, {0.4, 1}, {0, 1}}}});
  EXPECT_EQ(outdoor.locate(hv::BoundingBox::make(0.7, 0.1, 0.9, 0.9, 0.8)), "UNKNOWN");
}

TEST(Zones, InvalidBoxAndZoneAreRejected) {
  EXPECT_THROW(hv::BoundingBox::make(0.9, 0.1, 0.2, 0.5, 0.8), std::invalid_argument);
  EXPECT_THROW(hv::ZoneMap("CAM", {{"KITCHEN", {{0, 0}, {1, 0}, {1, 1}}}}), std::invalid_argument);
  EXPECT_THROW(hv::ZoneMap("", {}), std::invalid_argument);
  EXPECT_THROW(hv::ZoneMap("CAM", {{"BED", {{0, 0}, {1, 0}}}}), std::invalid_argument);
}

TEST(Zones, FileOrderIsKept) {
  const auto config = nlohmann::ordered_json::parse(
      R"({"camera_id": "CAM-INDOOR-01", "zones": {"DESK": [[0,0],[1,0],[1,1],[0,1]], "BED": [[0,0],[1,0],[1,1],[0,1]]}})");
  const auto zones = hv::ZoneMap::from_json(config);
  EXPECT_EQ(zones.zones().front().first, "DESK");
  EXPECT_EQ(zones.locate(hv::BoundingBox::make(0.2, 0.2, 0.4, 0.4, 0.5)), "DESK");
}

TEST(Zones, TemplateLoads) {
  const fs::path path = kVision / "config/zones.template.json";
  if (!fs::exists(path)) GTEST_SKIP() << "no template at " << path;
  const auto zones = hv::ZoneMap::load(path);
  EXPECT_EQ(zones.camera_id(), "CAM-INDOOR-01");
  EXPECT_EQ(zones.zones().size(), 5u);
}

TEST(Stabilizer, ConfirmationAndDebouncedLoss) {
  hv::ZoneStabilizer stable(3, 2);
  EXPECT_FALSE(stable.update("DESK"));
  EXPECT_FALSE(stable.update("DESK"));
  EXPECT_EQ(stable.update("DESK"), "DESK");
  EXPECT_FALSE(stable.update(std::nullopt));
  EXPECT_EQ(stable.current(), "DESK");
  EXPECT_EQ(stable.update(std::nullopt), "UNKNOWN");
  EXPECT_THROW(hv::ZoneStabilizer(0, 1), std::invalid_argument);
}

TEST(Presence, SummaryUsesTheBestFigureFootPoint) {
  hv::PostprocessResult result;
  result.detections.push_back({0, 0.8, {300, 100, 380, 400}});  // foot (340/640, 400/480) -> DESK
  result.detections.push_back({0, 0.6, {10, 10, 50, 50}});
  result.detections.push_back({3, 0.99, {10, 10, 50, 50}});  // not a figure class
  const auto summary = hv::summarize(result, two_halves(), 640, 480, {0});
  EXPECT_EQ(summary.figure_count, 2);
  EXPECT_EQ(summary.zone, "DESK");
  EXPECT_DOUBLE_EQ(summary.confidence, 0.8);
  ASSERT_TRUE(summary.foot);
  EXPECT_DOUBLE_EQ(summary.foot->first, 340.0 / 640.0);
  EXPECT_DOUBLE_EQ(summary.foot->second, 400.0 / 480.0);

  hv::PostprocessResult empty;
  empty.best_score = 0.1;
  const auto none = hv::summarize(empty, two_halves(), 640, 480, {0});
  EXPECT_EQ(none.figure_count, 0);
  EXPECT_EQ(none.zone, "UNKNOWN");
  EXPECT_DOUBLE_EQ(none.confidence, 0.9);
  EXPECT_FALSE(none.foot);
}

TEST(Presence, PublishesOnStableChangeAndRepublish) {
  hv::CameraPresence camera("CAM-INDOOR-01", {3, 3, 10000});
  hv::FrameSummary desk{1, 0.8, "DESK"};
  hv::FrameSummary none{0, 0.95, "UNKNOWN"};
  EXPECT_FALSE(camera.update(desk, 0));
  EXPECT_FALSE(camera.update(desk, 200));
  auto first = camera.update(desk, 400);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->reason, "change");
  EXPECT_EQ(first->zone, "DESK");
  EXPECT_TRUE(first->present);
  EXPECT_FALSE(camera.update(desk, 600));
  auto again = camera.update(desk, 10400);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->reason, "republish");
  EXPECT_FALSE(camera.update(none, 10600));
  EXPECT_FALSE(camera.update(none, 10800));
  auto lost = camera.update(none, 11000);
  ASSERT_TRUE(lost);
  EXPECT_EQ(lost->zone, "UNKNOWN");
  EXPECT_FALSE(lost->present);
}

TEST(Presence, InitialStateIsAnnouncedOnce) {
  hv::CameraPresence camera("CAM-OUTDOOR-01", {3, 3, 0});
  hv::FrameSummary none{0, 0.99, "UNKNOWN"};
  EXPECT_FALSE(camera.update(none, 0));
  EXPECT_FALSE(camera.update(none, 1));
  auto initial = camera.update(none, 2);
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial->reason, "initial");
  EXPECT_FALSE(camera.update(none, 100000));  // republish disabled
}

TEST(Presence, FigurePresenceMessageHasOnlyContractFields) {
  hv::FigurePresenceArgs args;
  args.camera_id = "CAM-OUTDOOR-01";
  args.registry_zone = "outdoor";
  args.present = true;
  args.figure_count = 42;
  args.confidence = 0.876543;
  args.model_version = "yolox-nano-416-figure";
  args.frame_age_ms = 120;
  const auto message = hv::figure_presence(args);
  EXPECT_EQ(message["observation_type"], "FIGURE_PRESENCE");
  EXPECT_EQ(message["source_device_id"], "CAM-OUTDOOR-01");
  EXPECT_EQ(message["attributes"]["figure_count"], 20);
  EXPECT_DOUBLE_EQ(message["confidence"].get<double>(), 0.8765);
  EXPECT_EQ(message["attributes"].size(), 4u);
  EXPECT_FALSE(message.contains("simulated"));
  args.model_version.clear();
  EXPECT_THROW(hv::figure_presence(args), std::invalid_argument);
}

TEST(Sources, ImageDirectoryIsSortedAndFinite) {
  const fs::path dir = kVision / "data/smoke-dataset/images/surrogate/CAM-INDOOR-01";
  if (!fs::exists(dir)) GTEST_SKIP() << "no sample images";
  std::unique_ptr<hv::FrameSource> source;
  try {
    source = hv::make_image_source(dir);
    auto frame = source->grab();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->image.width, 320);
    EXPECT_EQ(frame->image.height, 240);
  } catch (const std::runtime_error& error) {
    if (std::string(error.what()).find("built without") != std::string::npos) GTEST_SKIP() << error.what();
    throw;
  }
  int count = 1;
  while (source->grab()) ++count;
  EXPECT_EQ(count, 6);
  EXPECT_FALSE(source->live());
  EXPECT_THROW(hv::make_image_source(kVision / "does-not-exist"), std::runtime_error);
}

TEST(Sources, UnknownTypeAndMissingDevicesFail) {
  EXPECT_THROW(hv::make_source({{"type", "rtsp"}}, kRepo), std::invalid_argument);
  if (hv::v4l2_available()) {
    EXPECT_THROW(hv::make_source({{"type", "v4l2"}, {"device", "/dev/hm-no-such-video"}}, kRepo), std::runtime_error);
  }
  // Camera1 without a system bus (PC container): opening must fail cleanly, never hang.
  hv::Camera1Options options;
  options.capture_dir = fs::temp_directory_path() / "hm-vision-camera1-test";
  options.warmup_ms = 0;
  EXPECT_THROW(hv::make_camera1_source(options), std::runtime_error);
}

TEST(Sources, Camera1OptionsAndDeviceChoice) {
  const auto options = hv::parse_camera1_options({{"type", "camera1"},
                                                  {"device_name", "LifeCam"},
                                                  {"capture_dir", "/tmp/homemind-vision/indoor"},
                                                  {"capture_dir_mode", "0730"},
                                                  {"capture_dir_group", "camera"}});
  EXPECT_EQ(options.device_name, "LifeCam");
  EXPECT_EQ(options.capture_dir, fs::path("/tmp/homemind-vision/indoor"));
  EXPECT_EQ(options.capture_dir_mode, 0730);
  EXPECT_EQ(options.capture_dir_group, "camera");

  // NODE-RPI5-02 2026-10-25: the ids follow the plug-in order, the names stay with the cameras.
  const std::vector<std::string> ids = {"camera1", "camera2"};
  const std::vector<std::string> names = {"Microsoft\xc2\xae_LifeCam_HD-3000", "USB_Composite_Device"};
  EXPECT_EQ(hv::select_camera1_device(ids, names, options), "camera1");
  EXPECT_EQ(hv::select_camera1_device(ids, {names[1], names[0]}, options), "camera2");
  hv::Camera1Options by_name;
  by_name.device_name = "usb_composite";
  EXPECT_EQ(hv::select_camera1_device(ids, names, by_name), "camera2");
  by_name.device_name = "_";  // both
  EXPECT_THROW(hv::select_camera1_device(ids, names, by_name), std::runtime_error);
  by_name.device_name = "Logitech";
  EXPECT_THROW(hv::select_camera1_device(ids, names, by_name), std::runtime_error);
  EXPECT_THROW(hv::select_camera1_device(ids, {}, by_name), std::invalid_argument);

  hv::Camera1Options by_id;
  by_id.device_id = "camera2";
  by_id.device_name = "LifeCam";  // the exact id wins
  EXPECT_EQ(hv::select_camera1_device(ids, {}, by_id), "camera2");
  by_id.device_id = "camera3";
  EXPECT_THROW(hv::select_camera1_device(ids, {}, by_id), std::runtime_error);

  hv::Camera1Options by_index;
  by_index.device_index = 1;
  EXPECT_EQ(hv::select_camera1_device(ids, {}, by_index), "camera2");
  by_index.device_index = 2;
  EXPECT_THROW(hv::select_camera1_device(ids, {}, by_index), std::runtime_error);
}

TEST(Sources, CaptureDirLetsOnlyTheCameraGroupIn) {
#ifdef __linux__
  const fs::path base = fs::temp_directory_path() / ("hm-capture-dir-" + std::to_string(::getpid()));
  fs::remove_all(base);
  fs::create_directories(base);
  const std::string group = ::getgrgid(::getegid())->gr_name;  // a group this process may give its folders
  const auto mode_of = [](const fs::path& path) {
    struct stat info {};
    EXPECT_EQ(::stat(path.c_str(), &info), 0) << path;
    return std::make_pair(info.st_mode & 07777, info.st_gid);
  };
  {
    hv::CaptureDir indoor(base / "vision" / "indoor", 0730, group);
    EXPECT_EQ(indoor.path(), base / "vision" / "indoor");
    EXPECT_EQ(mode_of(base / "vision" / "indoor"), std::make_pair(static_cast<mode_t>(0730), ::getegid()));
    EXPECT_EQ(mode_of(base / "vision"), std::make_pair(static_cast<mode_t>(0710), ::getegid()));
    {
      hv::CaptureDir outdoor(base / "vision" / "outdoor", 0730, group);  // the parent exists now: left alone
      EXPECT_EQ(mode_of(base / "vision" / "outdoor").first, 0730u);
    }
    EXPECT_FALSE(fs::exists(base / "vision" / "outdoor"));
    EXPECT_TRUE(fs::exists(base / "vision" / "indoor"));
  }
  EXPECT_FALSE(fs::exists(base / "vision"));  // made by the indoor folder and empty again

  // A folder that already existed keeps existing; leftover frames in it go.
  fs::create_directories(base / "kept");
  std::ofstream(base / "kept" / "Picture25102026-15151885.jpeg") << "frame";
  {
    hv::CaptureDir kept(base / "kept", 0700, "");
    EXPECT_FALSE(fs::exists(base / "kept" / "Picture25102026-15151885.jpeg"));
  }
  EXPECT_TRUE(fs::exists(base / "kept"));
  EXPECT_THROW(hv::CaptureDir(base / "other", 0730, "hm-no-such-group"), std::invalid_argument);
  EXPECT_FALSE(fs::exists(base / "other"));
  fs::remove_all(base);
#else
  GTEST_SKIP() << "POSIX folder modes";
#endif
}

TEST(Detector, SmokeModelRunsWhenAvailable) {
  const fs::path model = kVision / "artifacts/smoke-yolox-nano-figure.onnx";
  const fs::path image = kVision / "data/surrogate/dog.jpg";
  if (!hv::onnxruntime_available() || !fs::exists(model) || !fs::exists(image)) {
    GTEST_SKIP() << "ONNX Runtime or the smoke model is not available";
  }
  hv::Image frame;
  try {
    frame = hv::read_image(image);
  } catch (const std::runtime_error& error) {
    GTEST_SKIP() << error.what();
  }
  hv::YoloxDetector detector(hv::make_onnxruntime_backend(model, 2));
  EXPECT_EQ(detector.input_height(), 416);
  const auto result = detector.detect(frame);
  EXPECT_EQ(detector.last_raw_shape(), (std::vector<std::int64_t>{1, 3549, 6}));
  EXPECT_TRUE(result.detections.empty());  // the smoke model never passes 0.25
  EXPECT_GT(result.best_score, 0.0);
}

// ---- BackgroundDetector (one-axis doll model: the figure moves along a rail through three zones) --------------
namespace {

constexpr int kSceneWidth = 320;
constexpr int kSceneHeight = 240;

// Floor with an 8 px checker (texture, so a moved camera shows up) and a small ramp.
hv::Image rail_background() {
  hv::Image image = hv::make_image(kSceneWidth, kSceneHeight);
  for (int y = 0; y < kSceneHeight; ++y) {
    for (int x = 0; x < kSceneWidth; ++x) {
      const int base = 90 + 40 * ((x / 8 + y / 8) % 2) + (x + y) % 5;
      std::uint8_t* pixel = image.row(y) + x * 3;
      pixel[0] = static_cast<std::uint8_t>(base);
      pixel[1] = static_cast<std::uint8_t>(base + 10);
      pixel[2] = static_cast<std::uint8_t>(base - 10);
    }
  }
  return image;
}

// Deterministic sensor noise of +-4 and a brightness gain.
hv::Image camera_frame(const hv::Image& scene, unsigned seed, double gain = 1.0) {
  hv::Image frame = scene;
  unsigned state = seed * 2654435761u + 1u;
  for (auto& value : frame.bgr) {
    state = state * 1103515245u + 12345u;
    const int noise = static_cast<int>((state >> 16) % 9) - 4;
    value = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(value * gain)) + noise, 0, 255));
  }
  return frame;
}

// A 24 x 60 px yellow figure standing at x (pixels), feet at y = 228.
hv::Image with_figure(hv::Image scene, int center_x) {
  for (int y = 168; y < 228; ++y) {
    for (int x = center_x - 12; x < center_x + 12; ++x) {
      std::uint8_t* pixel = scene.row(y) + x * 3;
      pixel[0] = 40;
      pixel[1] = 220;
      pixel[2] = 240;
    }
  }
  return scene;
}

hv::ZoneMap rail_zones() {
  return hv::ZoneMap("CAM-INDOOR-01", {{"ENTRANCE", {{0, 0.4}, {0.33, 0.4}, {0.33, 1}, {0, 1}}},
                                       {"BED", {{0.33, 0.4}, {0.66, 0.4}, {0.66, 1}, {0.33, 1}}},
                                       {"BATHROOM", {{0.66, 0.4}, {1, 0.4}, {1, 1}, {0.66, 1}}}});
}

std::vector<hv::Polygon> regions_of(const hv::ZoneMap& zones) {
  std::vector<hv::Polygon> regions;
  for (const auto& zone : zones.zones()) regions.push_back(zone.second);
  return regions;
}

hv::FrameSummary place(hv::BackgroundDetector& detector, const hv::Image& frame) {
  return hv::summarize(detector.detect(frame), rail_zones(), frame.width, frame.height, {0});
}

}  // namespace

TEST(Background, EmptySceneHasNoFigure) {
  hv::BackgroundDetector detector(rail_background(), regions_of(rail_zones()), {});
  for (unsigned seed = 1; seed <= 5; ++seed) {
    const auto result = detector.detect(camera_frame(rail_background(), seed));
    EXPECT_TRUE(result.detections.empty()) << seed;
    EXPECT_FALSE(detector.last_stats().unusable);
  }
}

TEST(Background, FigureLandsInTheZoneOfItsFeet) {
  hv::BackgroundDetector detector(rail_background(), regions_of(rail_zones()), {});
  const std::vector<std::pair<int, std::string>> stops = {{53, "ENTRANCE"}, {160, "BED"}, {266, "BATHROOM"}};
  for (const auto& [x, zone] : stops) {
    const hv::FrameSummary summary = place(detector, camera_frame(with_figure(rail_background(), x), 7));
    EXPECT_EQ(summary.figure_count, 1) << zone;
    EXPECT_EQ(summary.zone, zone);
    EXPECT_GE(summary.confidence, 0.5);
  }
}

TEST(Background, BrighterRoomIsMatched) {
  hv::BackgroundDetector detector(rail_background(), regions_of(rail_zones()), {});
  EXPECT_TRUE(detector.detect(camera_frame(rail_background(), 3, 1.3)).detections.empty());
  EXPECT_NEAR(detector.last_stats().gain, 1.0 / 1.3, 0.05);
  EXPECT_EQ(place(detector, camera_frame(with_figure(rail_background(), 160), 3, 1.3)).zone, "BED");
}

TEST(Background, IgnoredLedDoesNotCount) {
  hv::Image lit = rail_background();
  for (int y = 120; y < 140; ++y) {
    for (int x = 150; x < 170; ++x) std::fill_n(lit.row(y) + x * 3, 3, std::uint8_t{255});
  }
  const hv::Polygon led = {{0.45, 0.48}, {0.55, 0.48}, {0.55, 0.6}, {0.45, 0.6}};
  hv::BackgroundDetector watching(rail_background(), regions_of(rail_zones()), {});
  EXPECT_EQ(watching.detect(camera_frame(lit, 2)).detections.size(), 1u);  // the LED alone looks like a figure
  hv::BackgroundDetector ignoring(rail_background(), regions_of(rail_zones()), {led});
  EXPECT_TRUE(ignoring.detect(camera_frame(lit, 2)).detections.empty());
  EXPECT_EQ(place(ignoring, camera_frame(with_figure(lit, 266), 2)).zone, "BATHROOM");
}

TEST(Background, LightsOffOrMovedCameraIsUnusable) {
  hv::BackgroundDetector detector(rail_background(), regions_of(rail_zones()), {});
  EXPECT_TRUE(detector.detect(camera_frame(with_figure(rail_background(), 53), 4, 0.3)).detections.empty());
  EXPECT_TRUE(detector.last_stats().unusable);
  EXPECT_EQ(detector.last_stats().reason, "brightness");

  const hv::Image scene = rail_background();
  hv::Image moved = scene;  // camera nudged 8 px: every checker cell flips
  for (int y = 0; y < kSceneHeight; ++y) {
    for (int x = 0; x < kSceneWidth; ++x) {
      std::copy_n(scene.row(y) + ((x + 8) % kSceneWidth) * 3, 3, moved.row(y) + x * 3);
    }
  }
  EXPECT_TRUE(detector.detect(camera_frame(moved, 5)).detections.empty());
  EXPECT_TRUE(detector.last_stats().unusable);
  EXPECT_EQ(detector.last_stats().reason, "too much change");
}

TEST(Background, OtherFrameSizeIsRescaled) {
  hv::BackgroundDetector detector(rail_background(), regions_of(rail_zones()), {});
  const hv::Image big = hv::resize_linear(with_figure(rail_background(), 266), 640, 480);
  EXPECT_EQ(place(detector, big).zone, "BATHROOM");
}

TEST(Background, MedianDropsWhatPassesThrough) {
  const hv::Image scene = rail_background();
  const std::vector<hv::Image> frames = {scene, with_figure(scene, 53), scene, with_figure(scene, 160), scene};
  EXPECT_EQ(hv::median_image(frames).bgr, scene.bgr);
  EXPECT_THROW(hv::median_image({}), std::invalid_argument);
  EXPECT_THROW(hv::median_image({scene, hv::make_image(4, 4)}), std::invalid_argument);
}

TEST(Background, RejectsBadSetup) {
  EXPECT_THROW(hv::BackgroundDetector(hv::Image{}, regions_of(rail_zones()), {}), std::invalid_argument);
  EXPECT_THROW(hv::BackgroundDetector(rail_background(), {}, {}), std::invalid_argument);
  const hv::Polygon sliver = {{0.5, 0.5}, {0.5001, 0.5}, {0.5001, 0.5001}};
  EXPECT_THROW(hv::BackgroundDetector(rail_background(), {sliver}, {}), std::invalid_argument);
  hv::BackgroundOptions options;
  options.min_area_ratio = 0.6;
  EXPECT_THROW(hv::BackgroundDetector(rail_background(), regions_of(rail_zones()), {}, options), std::invalid_argument);
}

TEST(Png, WriteThenReadIsIdentical) {
  const hv::Image scene = camera_frame(rail_background(), 9);
  const fs::path path = fs::temp_directory_path() / "hm_vision_png_roundtrip.png";
  hv::write_png(scene, path);
  const hv::Image back = hv::read_image(path);
  fs::remove(path);
  EXPECT_EQ(back.width, scene.width);
  EXPECT_EQ(back.height, scene.height);
  EXPECT_EQ(back.bgr, scene.bgr);
}

TEST(Preview, OnlyTheUiSeesTheNewestFrameAndPrivacyDropsIt) {
  const auto settings = hv::parse_preview_settings(
      {{"enabled", true}, {"port", 8091}, {"allowed_clients", {"10.10.10.3"}}, {"max_age_ms", 5000}});
  ASSERT_TRUE(settings.enabled);
  hv::PreviewStore store({"CAM-INDOOR-01", "CAM-OUTDOOR-01"});
  store.set_detecting("CAM-INDOOR-01", true);
  const std::int64_t now = 1792560000000;
  EXPECT_EQ(hv::handle_preview(store, settings, "/preview/CAM-INDOOR-01.jpg", "10.10.10.3", now).status, 404)
      << "no frame yet";
  store.put("CAM-INDOOR-01", {0xFF, 0xD8, 0x01, 0xFF, 0xD9}, 640, 480, now - 100);
  store.put("CAM-INDOOR-01", {0xFF, 0xD8, 0x02, 0xFF, 0xD9}, 640, 480, now - 50);  // only the newest stays
  store.put("CAM-GARAGE-09", {0xFF, 0xD8}, 640, 480, now);                       // not one of ours: dropped
  const auto image = hv::handle_preview(store, settings, "/preview/CAM-INDOOR-01.jpg", "10.10.10.3", now);
  EXPECT_EQ(image.status, 200);
  EXPECT_EQ(image.content_type, "image/jpeg");
  const std::vector<std::uint8_t> newest = {0xFF, 0xD8, 0x02, 0xFF, 0xD9};
  EXPECT_EQ(image.body, std::string(newest.begin(), newest.end()));
  EXPECT_EQ(hv::handle_preview(store, settings, "/preview/CAM-INDOOR-01.jpg", "::ffff:10.10.10.3", now).status, 200);
  for (const char* other : {"10.10.10.2", "10.10.10.50", "192.168.0.14", "10.10.10.30"}) {
    EXPECT_EQ(hv::handle_preview(store, settings, "/preview/CAM-INDOOR-01.jpg", other, now).status, 403) << other;
    EXPECT_EQ(hv::handle_preview(store, settings, "/preview/status", other, now).status, 403) << other;
  }
  EXPECT_EQ(hv::handle_preview(store, settings, "/preview/CAM-GARAGE-09.jpg", "10.10.10.3", now).status, 404);
  EXPECT_EQ(hv::handle_preview(store, settings, "/preview/../config/rpi5-02-vision.json", "10.10.10.3", now).status, 404);
  EXPECT_EQ(hv::handle_preview(store, settings, "/preview/CAM-INDOOR-01.jpg", "10.10.10.3", now + 6000).status, 503)
      << "stale frame";

  const json status = json::parse(hv::handle_preview(store, settings, "/preview/status", "10.10.10.3", now).body);
  EXPECT_EQ(status["camera_allowed"], true);
  ASSERT_EQ(status["cameras"].size(), 2u);
  EXPECT_EQ(status["cameras"][0]["camera_id"], "CAM-INDOOR-01");
  EXPECT_EQ(status["cameras"][0]["detecting"], true);
  EXPECT_EQ(status["cameras"][0]["frame_age_ms"], 50);
  EXPECT_EQ(status["cameras"][0]["width"], 640);
  EXPECT_TRUE(status["cameras"][1]["frame_age_ms"].is_null());
  EXPECT_EQ(status["cameras"][1]["detecting"], false);

  store.set_camera_allowed(false);  // privacy mode camera_blocked / private
  EXPECT_EQ(hv::handle_preview(store, settings, "/preview/CAM-INDOOR-01.jpg", "10.10.10.3", now).status, 423);
  store.put("CAM-INDOOR-01", {0xFF, 0xD8, 0x03, 0xFF, 0xD9}, 640, 480, now);  // ignored while blocked
  store.set_camera_allowed(true);
  EXPECT_EQ(hv::handle_preview(store, settings, "/preview/CAM-INDOOR-01.jpg", "10.10.10.3", now).status, 404)
      << "frames from before and during the block are gone";
}

TEST(Preview, SettingsAreOffUnlessConfiguredAndNeedTheUiAddress) {
  EXPECT_FALSE(hv::parse_preview_settings(nullptr).enabled);
  EXPECT_FALSE(hv::parse_preview_settings(json::object()).enabled);
  EXPECT_FALSE(hv::parse_preview_settings({{"enabled", false}}).enabled);
  EXPECT_THROW(hv::parse_preview_settings({{"enabled", true}}), std::invalid_argument) << "nobody allowed";
  EXPECT_THROW(hv::parse_preview_settings({{"allowed_clients", {"10.10.10.3"}}, {"port", 70000}}),
               std::invalid_argument);
  EXPECT_THROW(hv::parse_preview_settings({{"allowed_clients", {""}}}), std::invalid_argument);
  EXPECT_THROW(hv::parse_preview_settings({{"allowed_clients", "10.10.10.3"}}), std::invalid_argument);
  EXPECT_THROW(hv::parse_preview_settings(json::array()), std::invalid_argument);
  const auto on = hv::parse_preview_settings({{"allowed_clients", {"10.10.10.3"}}});
  EXPECT_TRUE(on.enabled);
  EXPECT_EQ(on.port, 8091);
  EXPECT_EQ(on.listen_host, "0.0.0.0");
}
