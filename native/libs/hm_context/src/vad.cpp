// Port of homemind_audio.audio.vad_temporal_features. Every step mirrors the NumPy dtype rules of the
// reference (float32 arrays with weak Python scalars, float64 for run statistics) so the 12 features match
// bit for bit; this file must be compiled with -ffp-contract=off (no FMA contraction).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "hm/context/audio_features.hpp"

namespace hm::ctx {
namespace {

// numpy's pairwise summation (8 accumulators, block 128) used by add.reduce along a contiguous axis.
template <typename T>
T pairwise_sum(const T* a, std::size_t n) {
  if (n < 8) {
    T res = 0;
    for (std::size_t i = 0; i < n; ++i) res += a[i];
    return res;
  }
  if (n <= 128) {
    T r[8];
    for (int j = 0; j < 8; ++j) r[j] = a[j];
    std::size_t i = 8;
    for (; i < n - (n % 8); i += 8)
      for (int j = 0; j < 8; ++j) r[j] += a[i + j];
    T res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) res += a[i];
    return res;
  }
  std::size_t n2 = n / 2;
  n2 -= n2 % 8;
  return pairwise_sum(a, n2) + pairwise_sum(a + n2, n - n2);
}

// np.mean of a float32 array: float32 pairwise sum, then (float64 sum / count) rounded to float32.
float mean_f32(const float* a, std::size_t n) {
  return static_cast<float>(static_cast<double>(0.0f + pairwise_sum(a, n)) / static_cast<double>(n));
}

// np.std of a float32 array (ddof=0), float32 throughout like NumPy's _var.
float std_f32(const float* a, std::size_t n) {
  const float mean = mean_f32(a, n);
  std::vector<float> squares(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float d = a[i] - mean;
    squares[i] = d * d;
  }
  return std::sqrt(mean_f32(squares.data(), n));
}

// np.percentile(a, q) (method="linear") of a float32 array: q / float32(100), float32 index and lerp.
float percentile_f32(const std::vector<float>& sorted, int q) {
  const std::size_t n = sorted.size();
  const float quantile = static_cast<float>(q) / 100.0f;
  const float virtual_index = static_cast<float>(n - 1) * quantile;
  if (virtual_index >= static_cast<float>(n - 1)) return sorted[n - 1];
  const auto previous = static_cast<std::size_t>(std::floor(virtual_index));
  const auto gamma = static_cast<float>(static_cast<double>(virtual_index) - static_cast<double>(previous));
  const float a = sorted[previous];
  const float b = sorted[previous + 1];
  const float diff = b - a;
  float result = a + diff * gamma;
  if (gamma >= 0.5f) result = b - diff * (1.0f - gamma);
  return result;
}

struct RunStatistics {
  double mean = 0.0, std = 0.0, max = 0.0;
};

RunStatistics statistics(const std::vector<double>& runs) {
  if (runs.empty()) return {};
  const auto n = static_cast<double>(runs.size());
  const double mean = (0.0 + pairwise_sum(runs.data(), runs.size())) / n;
  std::vector<double> squares(runs.size());
  for (std::size_t i = 0; i < runs.size(); ++i) {
    const double d = runs[i] - mean;
    squares[i] = d * d;
  }
  const double variance = (0.0 + pairwise_sum(squares.data(), squares.size())) / n;
  return {mean, std::sqrt(variance), *std::max_element(runs.begin(), runs.end())};
}

}  // namespace

std::array<float, kVadFeatures> vad_temporal_features(const float* waveform, std::size_t samples) {
  constexpr std::size_t frame_size = 480;  // round(0.030 * 16000)
  constexpr std::size_t hop_size = 160;    // round(0.010 * 16000)
  std::vector<float> values(waveform, waveform + samples);
  if (values.size() < frame_size) values.resize(frame_size, 0.0f);
  const std::size_t frame_count = 1 + (values.size() - frame_size) / hop_size;

  std::vector<float> db(frame_count);
  std::array<float, frame_size> squares{};
  for (std::size_t i = 0; i < frame_count; ++i) {
    const float* row = values.data() + i * hop_size;
    for (std::size_t k = 0; k < frame_size; ++k) squares[k] = row[k] * row[k];
    const float rms = std::sqrt(mean_f32(squares.data(), frame_size) + 1e-12f);
    db[i] = 20.0f * static_cast<float>(std::log10(static_cast<double>(rms + 1e-8f)));
  }
  std::vector<float> sorted = db;
  std::sort(sorted.begin(), sorted.end());
  const float noise_floor = percentile_f32(sorted, 20);
  const double threshold = std::max(static_cast<double>(noise_floor) + 8.0, -45.0);
  const auto threshold_f32 = static_cast<float>(threshold);  // NumPy casts the Python float to float32

  // Run lengths of the voiced mask (speech vs pause) in seconds.
  std::vector<double> speech_runs, pause_runs;
  std::size_t voiced_frames = 0;
  std::size_t run_start = 0;
  constexpr double frame_seconds = static_cast<double>(hop_size) / kSampleRate;
  for (std::size_t i = 0; i < frame_count; ++i) {
    const bool voiced = db[i] >= threshold_f32;
    voiced_frames += voiced ? 1 : 0;
    const bool last = i + 1 == frame_count;
    if (last || (db[i + 1] >= threshold_f32) != voiced) {
      const double seconds = static_cast<double>(i + 1 - run_start) * frame_seconds;
      (voiced ? speech_runs : pause_runs).push_back(seconds);
      run_start = i + 1;
    }
  }
  const RunStatistics speech = statistics(speech_runs);
  const RunStatistics pause = statistics(pause_runs);
  const auto frames = static_cast<double>(frame_count);
  const float mean_db = mean_f32(db.data(), db.size());
  const float std_db = std_f32(db.data(), db.size());
  const float p90 = percentile_f32(sorted, 90);
  const float p10 = percentile_f32(sorted, 10);
  return {
      static_cast<float>(static_cast<double>(voiced_frames) / frames),
      static_cast<float>(static_cast<double>(speech_runs.size()) / frames),
      static_cast<float>(speech.mean),
      static_cast<float>(speech.std),
      static_cast<float>(speech.max),
      static_cast<float>(pause.mean),
      static_cast<float>(pause.std),
      static_cast<float>(pause.max),
      mean_db / 100.0f,
      std_db / 100.0f,
      (p90 - p10) / 100.0f,
      static_cast<float>(static_cast<double>(noise_floor) / 100.0),
  };
}

}  // namespace hm::ctx
