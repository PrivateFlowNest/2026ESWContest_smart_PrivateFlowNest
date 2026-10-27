// YAMNet embeddings through ONNX Runtime (C++ API) and the YamnetEmbeddingExtractor pooling.
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include <stdexcept>

#include "hm/context/audio_features.hpp"

namespace hm::ctx {
namespace {

// One ORT environment per process (ORT recommends a single Env).
Ort::Env& ort_env() {
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "hm_context");
  return env;
}

}  // namespace

struct YamnetModel::Impl {
  Ort::SessionOptions options;
  std::unique_ptr<Ort::Session> session;
  Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::string input_name;
  std::string output_name;
};

YamnetModel::YamnetModel(const std::filesystem::path& onnx_path, int threads) : impl_(std::make_unique<Impl>()) {
  if (!std::filesystem::is_regular_file(onnx_path))
    throw std::runtime_error("YAMNet ONNX model not found: " + onnx_path.string());
  impl_->options.SetIntraOpNumThreads(std::max(1, threads));
  impl_->options.SetInterOpNumThreads(1);
  impl_->options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  // One 4 s window per second: do not keep worker threads spinning between windows (Pi5 CPU budget).
  impl_->options.AddConfigEntry("session.intra_op.allow_spinning", "0");
  impl_->session = std::make_unique<Ort::Session>(ort_env(), onnx_path.c_str(), impl_->options);
  Ort::AllocatorWithDefaultOptions allocator;
  if (impl_->session->GetInputCount() != 1 || impl_->session->GetOutputCount() < 1)
    throw std::runtime_error("unexpected YAMNet ONNX signature: " + onnx_path.string());
  impl_->input_name = impl_->session->GetInputNameAllocated(0, allocator).get();
  impl_->output_name = impl_->session->GetOutputNameAllocated(0, allocator).get();
}

YamnetModel::~YamnetModel() = default;

std::vector<float> YamnetModel::embeddings(const float* waveform, std::size_t samples, std::size_t& frames) {
  // YAMNet zero-pads to at least one patch, so an empty window equals one zero sample.
  static const float silence = 0.0f;
  if (samples == 0) {
    waveform = &silence;
    samples = 1;
  }
  const std::array<std::int64_t, 1> shape{static_cast<std::int64_t>(samples)};
  Ort::Value input = Ort::Value::CreateTensor<float>(impl_->memory, const_cast<float*>(waveform), samples,
                                                     shape.data(), shape.size());
  const char* input_names[] = {impl_->input_name.c_str()};
  const char* output_names[] = {impl_->output_name.c_str()};
  auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, input_names, &input, 1, output_names, 1);
  const auto info = outputs[0].GetTensorTypeAndShapeInfo();
  const auto dims = info.GetShape();
  if (dims.size() != 2 || dims[1] != static_cast<std::int64_t>(kEmbeddingDims) || dims[0] <= 0)
    throw std::runtime_error("unexpected YAMNet embedding shape");
  frames = static_cast<std::size_t>(dims[0]);
  const float* data = outputs[0].GetTensorData<float>();
  return std::vector<float>(data, data + frames * kEmbeddingDims);
}

std::size_t embedding_feature_dimensions(const std::string& feature_version) {
  if (feature_version == kFeatureMeanVadV1) return kEmbeddingDims;
  if (feature_version == kFeatureTemporalV2) return 3 * kEmbeddingDims;
  throw std::invalid_argument("Unknown YAMNet feature version '" + feature_version + "'");
}

std::vector<float> pool_embeddings(const std::vector<float>& embeddings, std::size_t frames,
                                   const std::string& feature_version) {
  const std::size_t dims = embedding_feature_dimensions(feature_version);
  if (frames == 0 || embeddings.size() != frames * kEmbeddingDims)
    throw std::runtime_error("Unexpected YAMNet embedding shape");
  std::vector<float> out(dims, 0.0f);
  const auto count = static_cast<float>(frames);
  for (std::size_t d = 0; d < kEmbeddingDims; ++d) {
    float sum = 0.0f;
    float peak = embeddings[d];
    for (std::size_t f = 0; f < frames; ++f) {
      const float value = embeddings[f * kEmbeddingDims + d];
      sum += value;
      peak = std::max(peak, value);
    }
    const float mean = sum / count;
    out[d] = mean;
    if (dims == kEmbeddingDims) continue;
    float squares = 0.0f;
    for (std::size_t f = 0; f < frames; ++f) {
      const float delta = embeddings[f * kEmbeddingDims + d] - mean;
      squares += delta * delta;
    }
    out[kEmbeddingDims + d] = std::sqrt(squares / count);  // tf.math.reduce_std (population)
    out[2 * kEmbeddingDims + d] = peak;
  }
  return out;
}

ExtractedFeatures extract_features(YamnetModel& model, const float* waveform, std::size_t samples,
                                   const std::string& feature_version) {
  ExtractedFeatures result;
  const auto started = std::chrono::steady_clock::now();
  const std::vector<float> frames = model.embeddings(waveform, samples, result.frames);
  result.yamnet_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  result.vector = pool_embeddings(frames, result.frames, feature_version);
  result.vad = vad_temporal_features(waveform, samples);
  result.vector.insert(result.vector.end(), result.vad.begin(), result.vad.end());
  return result;
}

}  // namespace hm::ctx
