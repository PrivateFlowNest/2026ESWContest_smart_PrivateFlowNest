// Shared constants and numpy-compatible numeric helpers for the 16 kHz audio path.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hm::audio {

inline constexpr int kSampleRate = 16000;
inline constexpr std::size_t kBlockSamples = 1600;  // 100 ms, same block as services/runtime capture.py

// numpy's pairwise summation (np.sum / np.mean on a contiguous float64 array), same order of additions,
// so energy levels match the Python reference bit for bit on the same input.
double pairwise_sum(const double* values, std::size_t count);

// Number of Unicode code points in UTF-8 text (Python len()).
std::size_t utf8_length(const std::string& text);
// First `limit` code points of UTF-8 text (Python text[:limit]).
std::string utf8_prefix(const std::string& text, std::size_t limit);

}  // namespace hm::audio
