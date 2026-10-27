#include "hm/vision/detector.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace hm::vision {

Preprocessed preprocess_bgr(const Image& image, int input_height, int input_width) {
  if (image.empty() || image.bgr.size() != static_cast<std::size_t>(image.width) * image.height * 3) {
    throw std::invalid_argument("expected one BGR image");
  }
  Preprocessed out;
  out.ratio = std::min(static_cast<double>(input_height) / image.height, static_cast<double>(input_width) / image.width);
  out.resized_width = static_cast<int>(image.width * out.ratio);  // int() truncates like Python
  out.resized_height = static_cast<int>(image.height * out.ratio);
  if (out.resized_width <= 0 || out.resized_height <= 0) throw std::invalid_argument("image too small to letterbox");
  const Image resized = resize_linear(image, out.resized_width, out.resized_height);
  const std::size_t plane = static_cast<std::size_t>(input_height) * input_width;
  out.tensor.assign(3 * plane, 114.0f);
  for (int y = 0; y < resized.height; ++y) {
    const std::uint8_t* src = resized.row(y);
    for (int x = 0; x < resized.width; ++x) {
      const std::size_t at = static_cast<std::size_t>(y) * input_width + x;
      for (int c = 0; c < 3; ++c) out.tensor[c * plane + at] = static_cast<float>(src[x * 3 + c]);
    }
  }
  return out;
}

namespace {

struct Candidate {
  float box[4];
  float score;
  int class_id;
};

// _iou(reference, boxes) in float32, element by element as numpy evaluates it.
float iou(const float* reference, const float* box) {
  const float left = std::max(reference[0], box[0]);
  const float top = std::max(reference[1], box[1]);
  const float right = std::min(reference[2], box[2]);
  const float bottom = std::min(reference[3], box[3]);
  const float intersection = std::max(0.0f, right - left) * std::max(0.0f, bottom - top);
  const float raw_area = (reference[2] - reference[0]) * (reference[3] - reference[1]);
  const float reference_area = raw_area > 0.0f ? raw_area : 0.0f;  // Python max(0.0, value)
  const float box_area = std::max(0.0f, box[2] - box[0]) * std::max(0.0f, box[3] - box[1]);
  const float denominator = std::max(reference_area + box_area - intersection, 1e-9f);
  return intersection / denominator;
}

// _nms(): scores.argsort()[::-1]. Ties keep the higher index first (stable ascending sort, reversed);
// numpy's own tie order is unspecified, the parity tests report whether ties occurred.
std::vector<std::size_t> nms(const std::vector<const Candidate*>& items, float threshold) {
  std::vector<std::size_t> order(items.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return items[a]->score < items[b]->score; });
  std::reverse(order.begin(), order.end());
  std::vector<std::size_t> keep;
  while (!order.empty()) {
    const std::size_t current = order.front();
    keep.push_back(current);
    std::vector<std::size_t> remainder;
    remainder.reserve(order.size());
    for (std::size_t i = 1; i < order.size(); ++i) {
      if (iou(items[current]->box, items[order[i]]->box) <= threshold) remainder.push_back(order[i]);
    }
    order.swap(remainder);
  }
  return keep;
}

}  // namespace

PostprocessResult postprocess(const float* raw, std::size_t proposals, std::size_t row_width,
                              const PostprocessOptions& options) {
  if (row_width < 6) throw std::invalid_argument("unexpected YOLOX output shape");
  std::vector<std::array<float, 3>> grid;  // x, y, stride (meshgrid ij, strides 8/16/32)
  for (int stride : {8, 16, 32}) {
    const int rows = options.input_height / stride, cols = options.input_width / stride;
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < cols; ++x) grid.push_back({float(x), float(y), float(stride)});
    }
  }
  if (grid.size() != proposals) throw std::invalid_argument("model proposal count does not match 416 YOLOX strides");

  PostprocessResult result;
  const float threshold = static_cast<float>(options.score_threshold);
  const float ratio = static_cast<float>(options.ratio);
  const float max_x = static_cast<float>(options.image_width - 1), max_y = static_cast<float>(options.image_height - 1);
  float best = -std::numeric_limits<float>::infinity();
  std::vector<Candidate> candidates;
  for (std::size_t p = 0; p < proposals; ++p) {
    const float* row = raw + p * row_width;
    // argmax/max over class scores (first maximum; any NaN makes the score NaN like numpy)
    int class_id = 0;
    float class_score = row[5];
    bool nan = std::isnan(row[5]);
    for (std::size_t k = 6; k < row_width; ++k) {
      if (std::isnan(row[k])) nan = true;
      if (row[k] > class_score) {
        class_score = row[k];
        class_id = static_cast<int>(k - 5);
      }
    }
    const float score = nan ? std::numeric_limits<float>::quiet_NaN() : row[4] * class_score;
    if (score > best) best = score;
    if (!(score >= threshold)) continue;
    const auto& g = grid[p];
    const float cx = (row[0] + g[0]) * g[2];
    const float cy = (row[1] + g[1]) * g[2];
    const float half_w = std::exp(row[2]) * g[2] / 2.0f;
    const float half_h = std::exp(row[3]) * g[2] / 2.0f;
    Candidate candidate{{cx - half_w, cy - half_h, cx + half_w, cy + half_h}, score, class_id};
    for (float& value : candidate.box) value /= ratio;
    candidate.box[0] = std::min(std::max(candidate.box[0], 0.0f), max_x);
    candidate.box[2] = std::min(std::max(candidate.box[2], 0.0f), max_x);
    candidate.box[1] = std::min(std::max(candidate.box[1], 0.0f), max_y);
    candidate.box[3] = std::min(std::max(candidate.box[3], 0.0f), max_y);
    candidates.push_back(candidate);
  }
  result.best_score = std::isfinite(best) ? static_cast<double>(best) : 0.0;
  result.candidates = candidates.size();
  if (candidates.empty()) return result;

  std::vector<int> classes;
  for (const auto& candidate : candidates) classes.push_back(candidate.class_id);
  std::sort(classes.begin(), classes.end());
  classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
  const float nms_threshold = static_cast<float>(options.nms_threshold);
  for (int class_id : classes) {
    std::vector<const Candidate*> items;
    for (const auto& candidate : candidates) {
      if (candidate.class_id == class_id) items.push_back(&candidate);
    }
    for (std::size_t kept : nms(items, nms_threshold)) {
      const Candidate& c = *items[kept];
      result.detections.push_back({c.class_id, static_cast<double>(c.score),
                                   {double(c.box[0]), double(c.box[1]), double(c.box[2]), double(c.box[3])}});
    }
  }
  std::stable_sort(result.detections.begin(), result.detections.end(),
                   [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
  return result;
}

YoloxDetector::YoloxDetector(std::unique_ptr<InferenceBackend> backend, double score_threshold, double nms_threshold)
    : backend_(std::move(backend)), score_threshold_(score_threshold), nms_threshold_(nms_threshold) {
  if (!backend_) throw std::invalid_argument("YoloxDetector needs an inference backend");
  const auto shape = backend_->input_shape();
  if (shape.size() != 4 || shape[0] != 1 || shape[1] != 3 || shape[2] <= 0 || shape[3] <= 0) {
    throw std::invalid_argument("fixed 1x3xHxW input required");
  }
  input_height_ = static_cast<int>(shape[2]);
  input_width_ = static_cast<int>(shape[3]);
}

void YoloxDetector::set_thresholds(double score_threshold, double nms_threshold) {
  score_threshold_ = score_threshold;
  nms_threshold_ = nms_threshold;
}

PostprocessResult YoloxDetector::detect(const Image& image, DetectTiming* timing) {
  using clock = std::chrono::steady_clock;
  const auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
  const auto t0 = clock::now();
  const Preprocessed input = preprocess_bgr(image, input_height_, input_width_);
  const auto t1 = clock::now();
  last_raw_ = backend_->run(input.tensor, &last_raw_shape_);
  const auto t2 = clock::now();
  if (last_raw_shape_.size() != 3 || last_raw_shape_[0] != 1 || last_raw_shape_[2] < 6) {
    throw std::invalid_argument("unexpected YOLOX output shape");
  }
  PostprocessOptions options;
  options.input_height = input_height_;
  options.input_width = input_width_;
  options.ratio = input.ratio;
  options.image_height = image.height;
  options.image_width = image.width;
  options.score_threshold = score_threshold_;
  options.nms_threshold = nms_threshold_;
  PostprocessResult result = postprocess(last_raw_.data(), static_cast<std::size_t>(last_raw_shape_[1]),
                                         static_cast<std::size_t>(last_raw_shape_[2]), options);
  const auto t3 = clock::now();
  if (timing) *timing = {ms(t1 - t0), ms(t2 - t1), ms(t3 - t2)};
  return result;
}

}  // namespace hm::vision
