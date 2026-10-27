#include "hm/vision/background.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace hm::vision {

namespace {

std::vector<std::uint8_t> to_gray(const Image& image) {
  std::vector<std::uint8_t> gray(static_cast<std::size_t>(image.width) * image.height);
  const std::uint8_t* bgr = image.bgr.data();
  for (std::size_t i = 0; i < gray.size(); ++i, bgr += 3) {
    gray[i] = static_cast<std::uint8_t>((29 * bgr[0] + 150 * bgr[1] + 77 * bgr[2] + 128) >> 8);
  }
  return gray;
}

// Median of frame / background over the analysed pixels (steps of 1/128, up to 4). Unchanged pixels are the
// majority, so this is the room's brightness change even when the scene has two large brightness levels (a plain
// median of the frame jumps between them when a small figure enters).
double brightness_ratio(const std::vector<std::uint8_t>& frame, const std::vector<std::uint8_t>& background,
                        const std::vector<std::uint8_t>& mask, int count) {
  std::array<int, 513> histogram{};
  for (std::size_t i = 0; i < frame.size(); ++i) {
    if (!mask[i]) continue;
    const double ratio = (frame[i] + 1.0) / (background[i] + 1.0);
    ++histogram[static_cast<std::size_t>(std::min(512L, std::lround(ratio * 128.0)))];
  }
  int seen = 0;
  for (int bin = 0; bin <= 512; ++bin) {
    seen += histogram[static_cast<std::size_t>(bin)];
    if (2 * seen >= count) return std::max(bin, 1) / 128.0;
  }
  return 4.0;
}

// 3x3 erosion (all neighbours set) or dilation (any neighbour set); pixels outside the image count as unset.
std::vector<std::uint8_t> morph(const std::vector<std::uint8_t>& in, int width, int height, bool erode) {
  std::vector<std::uint8_t> out(in.size(), 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      bool all = true;
      bool any = false;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx;
          const int ny = y + dy;
          const bool set = nx >= 0 && ny >= 0 && nx < width && ny < height &&
                           in[static_cast<std::size_t>(ny) * width + nx] != 0;
          all = all && set;
          any = any || set;
        }
      }
      out[static_cast<std::size_t>(y) * width + x] = (erode ? all : any) ? 1 : 0;
    }
  }
  return out;
}

double elapsed_ms(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

BackgroundDetector::BackgroundDetector(Image background, std::vector<Polygon> regions, std::vector<Polygon> ignore,
                                       BackgroundOptions options)
    : background_(std::move(background)),
      regions_(std::move(regions)),
      ignore_(std::move(ignore)),
      options_(options) {
  if (background_.empty()) throw std::invalid_argument("background image is empty");
  if (regions_.empty()) throw std::invalid_argument("background detector needs at least one region");
  if (options_.work_width < 16) throw std::invalid_argument("background work_width must be >= 16");
  if (options_.threshold < 1 || options_.threshold > 255) {
    throw std::invalid_argument("background threshold must be 1..255");
  }
  if (!(options_.min_area_ratio > 0.0 && options_.min_area_ratio < options_.max_area_ratio &&
        options_.max_area_ratio <= 1.0)) {
    throw std::invalid_argument("background area ratios must satisfy 0 < min_area_ratio < max_area_ratio <= 1");
  }
  if (!(options_.max_gain >= 1.0)) throw std::invalid_argument("background max_gain must be >= 1");
  for (const auto* list : {&regions_, &ignore_}) {
    for (const auto& polygon : *list) {
      if (polygon.size() < 3) throw std::invalid_argument("background polygons need at least three points");
      for (const auto& [x, y] : polygon) {
        if (!(0.0 <= x && x <= 1.0 && 0.0 <= y && y <= 1.0)) {
          throw std::invalid_argument("background polygon coordinates must be normalized");
        }
      }
    }
  }
  prepare(background_.width, background_.height);
}

void BackgroundDetector::prepare(int width, int height) {
  if (width == frame_width_ && height == frame_height_) return;
  frame_width_ = width;
  frame_height_ = height;
  work_width_ = std::min(options_.work_width, width);
  work_height_ = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * work_width_ / width)));
  const std::size_t pixels = static_cast<std::size_t>(work_width_) * work_height_;
  mask_.assign(pixels, 0);
  analysed_ = 0;
  for (int y = 0; y < work_height_; ++y) {
    for (int x = 0; x < work_width_; ++x) {
      const std::pair<double, double> point{(x + 0.5) / work_width_, (y + 0.5) / work_height_};
      const auto inside = [&point](const Polygon& polygon) { return point_in_polygon(point, polygon); };
      if (std::any_of(regions_.begin(), regions_.end(), inside) &&
          std::none_of(ignore_.begin(), ignore_.end(), inside)) {
        mask_[static_cast<std::size_t>(y) * work_width_ + x] = 1;
        ++analysed_;
      }
    }
  }
  if (analysed_ == 0) throw std::invalid_argument("background regions cover no pixel");
  background_gray_ = to_gray(resize_linear(background_, work_width_, work_height_));
}

PostprocessResult BackgroundDetector::detect(const Image& image, DetectTiming* timing) {
  if (image.empty()) throw std::invalid_argument("background detector: empty frame");
  const auto started = std::chrono::steady_clock::now();
  prepare(image.width, image.height);
  const int w = work_width_;
  const int h = work_height_;
  std::vector<std::uint8_t> gray = to_gray(resize_linear(image, w, h));
  if (timing) timing->preprocess_ms = elapsed_ms(started);
  const auto compared = std::chrono::steady_clock::now();

  stats_ = BackgroundStats{};
  stats_.analysed_pixels = analysed_;
  PostprocessResult result;
  stats_.gain = 1.0 / brightness_ratio(gray, background_gray_, mask_, analysed_);
  if (stats_.gain > options_.max_gain || stats_.gain < 1.0 / options_.max_gain) {
    stats_.unusable = true;
    stats_.reason = "brightness";
  }

  std::vector<int> difference(gray.size(), 0);
  for (std::size_t i = 0; i < gray.size(); ++i) {
    const int matched = std::min(255, static_cast<int>(std::lround(gray[i] * stats_.gain)));
    difference[i] = std::abs(matched - background_gray_[i]);
  }
  std::vector<std::uint8_t> changed(gray.size(), 0);
  std::vector<int> smoothed(gray.size(), 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * w + x;
      if (!mask_[index]) continue;
      int sum = 0;
      int count = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          sum += difference[static_cast<std::size_t>(ny) * w + nx];
          ++count;
        }
      }
      smoothed[index] = sum / count;
      changed[index] = smoothed[index] >= options_.threshold ? 1 : 0;
    }
  }
  changed = morph(morph(changed, w, h, true), w, h, false);   // opening: drop isolated pixels
  changed = morph(morph(changed, w, h, false), w, h, true);   // closing: fill small holes
  for (std::size_t i = 0; i < changed.size(); ++i) changed[i] = changed[i] && mask_[i];

  struct Blob {
    int area = 0;
    long long difference = 0;
    int x1, y1, x2, y2;
  };
  std::vector<Blob> blobs;
  std::vector<std::uint8_t> visited(changed.size(), 0);
  std::vector<int> stack;
  const int min_area = std::max(4, static_cast<int>(std::ceil(options_.min_area_ratio * analysed_)));
  for (int start = 0; start < w * h; ++start) {
    if (!changed[static_cast<std::size_t>(start)] || visited[static_cast<std::size_t>(start)]) continue;
    ++result.candidates;
    Blob blob{0, 0, w, h, -1, -1};
    stack.assign(1, start);
    visited[static_cast<std::size_t>(start)] = 1;
    while (!stack.empty()) {
      const int index = stack.back();
      stack.pop_back();
      const int x = index % w;
      const int y = index / w;
      blob.area += 1;
      blob.difference += smoothed[static_cast<std::size_t>(index)];
      blob.x1 = std::min(blob.x1, x);
      blob.y1 = std::min(blob.y1, y);
      blob.x2 = std::max(blob.x2, x);
      blob.y2 = std::max(blob.y2, y);
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const int next = ny * w + nx;
          if (changed[static_cast<std::size_t>(next)] && !visited[static_cast<std::size_t>(next)]) {
            visited[static_cast<std::size_t>(next)] = 1;
            stack.push_back(next);
          }
        }
      }
    }
    if (blob.area >= min_area) blobs.push_back(blob);
  }
  for (const Blob& blob : blobs) stats_.changed_pixels += blob.area;
  stats_.blobs = static_cast<int>(blobs.size());
  if (!stats_.unusable && stats_.changed_pixels > options_.max_area_ratio * analysed_) {
    stats_.unusable = true;
    stats_.reason = "too much change";
  }
  if (!stats_.unusable) {
    std::stable_sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) { return a.area > b.area; });
    const double sx = static_cast<double>(image.width) / w;
    const double sy = static_cast<double>(image.height) / h;
    const double threshold = options_.threshold;
    for (const Blob& blob : blobs) {
      const double mean = static_cast<double>(blob.difference) / blob.area;
      Detection detection;
      detection.class_id = 0;
      detection.confidence = std::clamp(0.5 + 0.49 * (mean - threshold) / threshold, 0.5, 0.99);
      detection.box = {blob.x1 * sx, blob.y1 * sy, (blob.x2 + 1) * sx, (blob.y2 + 1) * sy};
      result.detections.push_back(detection);
      result.best_score = std::max(result.best_score, detection.confidence);
    }
  }
  if (timing) {
    timing->inference_ms = elapsed_ms(compared);
    timing->postprocess_ms = 0.0;
  }
  return result;
}

Image median_image(const std::vector<Image>& frames) {
  if (frames.empty()) throw std::invalid_argument("median_image: no frames");
  const int width = frames.front().width;
  const int height = frames.front().height;
  for (const Image& frame : frames) {
    if (frame.empty() || frame.width != width || frame.height != height) {
      throw std::invalid_argument("median_image: frames must be non-empty and the same size");
    }
  }
  Image out = make_image(width, height);
  std::vector<std::uint8_t> values(frames.size());
  const std::size_t middle = (frames.size() - 1) / 2;
  for (std::size_t i = 0; i < out.bgr.size(); ++i) {
    for (std::size_t f = 0; f < frames.size(); ++f) values[f] = frames[f].bgr[i];
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
    out.bgr[i] = values[middle];
  }
  return out;
}

}  // namespace hm::vision
