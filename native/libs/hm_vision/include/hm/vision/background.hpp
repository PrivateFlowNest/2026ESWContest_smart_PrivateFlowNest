// Figure presence by change against an empty-scene frame: no trained model, no photos of the figure.
// Built for the one-axis doll model, where the figure only moves along a rail inside the camera's zone polygons,
// so "something differs from the empty scene inside a zone" is the figure. Native only (the Python reference has
// the same idea only as a labelling aid, dataset._proposed_bbox). Per frame:
//   1. frame and background scaled to `work_width` (aspect kept, resize_linear), BT.601 gray
//   2. brightness matched: frame gray divided by the median per-pixel ratio frame / background over the analysed
//      pixels (robust to scenes with two large brightness levels, unlike a plain median of the frame)
//   3. |frame - background| averaged over 3x3; >= threshold is a change. Analysed pixels: inside a region polygon
//      (the camera's zones) and outside every `ignore` polygon (status LEDs, screens)
//   4. 3x3 opening then closing, 8-connected blobs; blobs below min_area_ratio of the analysed area are noise
//   5. the scene is unusable (no figure reported) when the gain leaves [1/max_gain, max_gain] or the blobs cover
//      more than max_area_ratio of the analysed area (lights off, camera moved, a hand in the model)
// Each kept blob is one figure: class 0, box in source pixels, confidence 0.5 at the threshold rising linearly to
// 0.99 at twice the threshold (mean difference over the blob). Detections are sorted by area, largest first, so
// summarize() places the largest blob. Nothing image-derived leaves the process.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hm/vision/detector.hpp"
#include "hm/vision/image.hpp"
#include "hm/vision/zones.hpp"

namespace hm::vision {

struct BackgroundOptions {
  int work_width = 160;
  int threshold = 30;             // gray levels (1..255) after brightness matching
  double min_area_ratio = 0.004;  // of the analysed pixels
  double max_area_ratio = 0.5;
  double max_gain = 2.0;
};

struct BackgroundStats {  // last frame, for logs and tests
  double gain = 1.0;
  int analysed_pixels = 0;
  int changed_pixels = 0;  // in kept blobs
  int blobs = 0;           // kept blobs
  bool unusable = false;
  std::string reason;      // why the frame was unusable
};

class BackgroundDetector : public FrameDetector {
 public:
  // regions / ignore: normalized polygons. Throws std::invalid_argument for an empty background, bad options
  // or regions that cover no pixel.
  BackgroundDetector(Image background, std::vector<Polygon> regions, std::vector<Polygon> ignore,
                     BackgroundOptions options = {});

  PostprocessResult detect(const Image& image, DetectTiming* timing = nullptr) override;
  const BackgroundStats& last_stats() const { return stats_; }

 private:
  void prepare(int width, int height);  // analysis size, mask and scaled background for this frame size

  Image background_;
  std::vector<Polygon> regions_;
  std::vector<Polygon> ignore_;
  BackgroundOptions options_;
  int frame_width_ = 0;
  int frame_height_ = 0;
  int work_width_ = 0;
  int work_height_ = 0;
  std::vector<std::uint8_t> mask_;
  std::vector<std::uint8_t> background_gray_;
  int analysed_ = 0;
  BackgroundStats stats_;
};

// Per-pixel median of same-size frames (the empty scene over a few seconds: sensor noise and anything passing
// through drop out). Throws std::invalid_argument for no frames or mixed sizes.
Image median_image(const std::vector<Image>& frames);

}  // namespace hm::vision
