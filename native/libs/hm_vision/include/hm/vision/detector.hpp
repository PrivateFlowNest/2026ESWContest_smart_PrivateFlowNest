// YOLOX-Nano (416, raw head output) preprocessing, decoding and NMS. Port of
// services/edge-ai/vision-presence/src/homemind_vision/detection.py: same float32 arithmetic, same
// thresholds, same per-class NMS order. Inference sits behind InferenceBackend so the AI Sub-System
// Inference Gateway (ecoai-inference) can replace ONNX Runtime without touching the decoding.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "hm/vision/image.hpp"

namespace hm::vision {

struct Detection {
  int class_id = 0;
  double confidence = 0.0;      // float(scores[i]) in Python: the float32 score widened to double
  std::array<double, 4> box{};  // x1, y1, x2, y2 in source pixels (float32 values widened)
};

// preprocess_bgr(): letterbox into HxW with value 114 at the bottom/right, BGR, 0..255, NCHW float32.
struct Preprocessed {
  std::vector<float> tensor;  // 3 * H * W
  double ratio = 1.0;
  int resized_width = 0;
  int resized_height = 0;
};
Preprocessed preprocess_bgr(const Image& image, int input_height, int input_width);

struct PostprocessOptions {
  int input_height = 416;
  int input_width = 416;
  double ratio = 1.0;
  int image_height = 0;
  int image_width = 0;
  double score_threshold = 0.25;
  double nms_threshold = 0.45;
};

struct PostprocessResult {
  std::vector<Detection> detections;  // sorted by confidence, highest first
  std::size_t candidates = 0;         // proposals with score >= threshold (before NMS)
  double best_score = 0.0;            // highest obj * class score over all proposals (also below threshold)
};

// raw: one batch of YOLOX raw predictions, `proposals` rows of `row_width` (= 5 + classes) float32 values.
// Throws std::invalid_argument when the shape does not match the 8/16/32 stride grid (like Python).
PostprocessResult postprocess(const float* raw, std::size_t proposals, std::size_t row_width,
                              const PostprocessOptions& options);

class InferenceBackend {
 public:
  virtual ~InferenceBackend() = default;
  virtual std::vector<std::int64_t> input_shape() const = 0;  // fixed NCHW
  // Runs one input tensor, returns the first output and its shape.
  virtual std::vector<float> run(const std::vector<float>& input, std::vector<std::int64_t>* output_shape) = 0;
  virtual std::string name() const = 0;
};

// ONNX Runtime CPU (intra_op threads, inter_op 1, like detect_file). Throws when built without ORT.
std::unique_ptr<InferenceBackend> make_onnxruntime_backend(const std::filesystem::path& model_path, int threads);
bool onnxruntime_available();

struct DetectTiming {
  double preprocess_ms = 0.0;
  double inference_ms = 0.0;
  double postprocess_ms = 0.0;
};

// What homemind-vision runs per frame: YoloxDetector (trained FIGURE model) or BackgroundDetector
// (background.hpp, change against an empty-scene frame). Both report figures as class 0 boxes in source pixels.
class FrameDetector {
 public:
  virtual ~FrameDetector() = default;
  virtual PostprocessResult detect(const Image& image, DetectTiming* timing = nullptr) = 0;
};

class YoloxDetector : public FrameDetector {
 public:
  YoloxDetector(std::unique_ptr<InferenceBackend> backend, double score_threshold = 0.25,
                double nms_threshold = 0.45);

  PostprocessResult detect(const Image& image, DetectTiming* timing = nullptr) override;
  int input_height() const { return input_height_; }
  int input_width() const { return input_width_; }
  // Raw output of the last detect() call (parity probes only; never published).
  const std::vector<float>& last_raw() const { return last_raw_; }
  const std::vector<std::int64_t>& last_raw_shape() const { return last_raw_shape_; }
  void set_thresholds(double score_threshold, double nms_threshold);

 private:
  std::unique_ptr<InferenceBackend> backend_;
  int input_height_ = 0;
  int input_width_ = 0;
  double score_threshold_;
  double nms_threshold_;
  std::vector<float> last_raw_;
  std::vector<std::int64_t> last_raw_shape_;
};

}  // namespace hm::vision
