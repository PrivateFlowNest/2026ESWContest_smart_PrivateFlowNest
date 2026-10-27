// Audio feature pieces of the context path: VAD temporal features, YAMNet (ONNX Runtime) embeddings and the
// embedding pooling selected by the activity head's feature_version (homemind_audio.yamnet).
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hm::ctx {

inline constexpr int kSampleRate = 16000;
inline constexpr std::size_t kVadFeatures = 12;
inline constexpr std::size_t kEmbeddingDims = 1024;
inline constexpr const char* kFeatureMeanVadV1 = "mean-vad-v1";
inline constexpr const char* kFeatureTemporalV2 = "mean-std-max-vad-v2";

// homemind_audio.audio.vad_temporal_features at 16 kHz. Reproduces NumPy's float32 arithmetic (pairwise sums,
// float32 percentile interpolation); only log10 rounding may differ by 1 ulp between C libraries.
std::array<float, kVadFeatures> vad_temporal_features(const float* waveform, std::size_t samples);

// YAMNet exported by tools/export_yamnet_onnx.py: waveform float32[samples] -> embeddings float32[frames, 1024].
class YamnetModel {
 public:
  YamnetModel(const std::filesystem::path& onnx_path, int threads);
  ~YamnetModel();
  YamnetModel(const YamnetModel&) = delete;
  YamnetModel& operator=(const YamnetModel&) = delete;

  // Row-major [frames, 1024]; `frames` receives the frame count (8 for a 4 s window).
  std::vector<float> embeddings(const float* waveform, std::size_t samples, std::size_t& frames);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Embedding dimensions for a feature version (1024 or 3072); throws for unknown versions.
std::size_t embedding_feature_dimensions(const std::string& feature_version);
// mean (v1) or mean|std|max (v2) over frames, float32 like the TensorFlow reductions.
std::vector<float> pool_embeddings(const std::vector<float>& embeddings, std::size_t frames,
                                   const std::string& feature_version);

// YamnetEmbeddingExtractor.extract_waveform: pooled embeddings followed by the 12 VAD features.
struct ExtractedFeatures {
  std::vector<float> vector;
  std::array<float, kVadFeatures> vad{};
  std::size_t frames = 0;
  double yamnet_ms = 0.0;
};
ExtractedFeatures extract_features(YamnetModel& model, const float* waveform, std::size_t samples,
                                   const std::string& feature_version);

}  // namespace hm::ctx
