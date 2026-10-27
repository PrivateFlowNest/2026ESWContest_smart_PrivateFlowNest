// WAV input/output for the perception service, same conversions as the Python reference:
//   load_wav_16k_mono / load_wav_segment_16k_mono = services/edge-ai/audio-context homemind_audio.audio
//   encode_pcm16_wav                              = services/runtime audio/stt.py write_pcm16_wav
// Supported: RIFF/WAVE PCM 8/16/24/32-bit, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE; any channel count
// (mean of channels). FLAC is not supported (no libsndfile on the device).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hm::audio {

struct WavInfo {
  int format = 0;  // 1 = PCM integer, 3 = IEEE float (extensible resolved to its sub format)
  int channels = 0;
  int sample_rate = 0;
  int bits_per_sample = 0;
  int block_align = 0;  // bytes per frame
  std::int64_t frames = 0;
  std::int64_t data_offset = 0;
};

WavInfo read_wav_info(const std::filesystem::path& path);

// Frames [first, first + count) as mono float32 (homemind_audio _to_float32, then mean over channels).
// No resampling, no padding, no final clip.
std::vector<float> read_wav_frames_mono(const std::filesystem::path& path, const WavInfo& info, std::int64_t first,
                                        std::int64_t count);

// Whole file -> 16 kHz mono float32 in [-1, 1], padded with zeros to at least 1 s (Python MIN_SAMPLES).
std::vector<float> load_wav_16k_mono(const std::filesystem::path& path);
// [start_seconds, end_seconds) of the file (Python rounding and bounds), same normalisation.
std::vector<float> load_wav_segment_16k_mono(const std::filesystem::path& path, double start_seconds,
                                             double end_seconds);

// 16-bit mono WAV bytes (44-byte header, like Python's wave module): clip to [-1, 1], *32767, truncate.
std::string encode_pcm16_wav(const float* samples, std::size_t count, int sample_rate = 16000);
void write_pcm16_wav(const std::filesystem::path& path, const float* samples, std::size_t count,
                     int sample_rate = 16000);
// 32-bit float WAV (tests and debugging).
void write_float_wav(const std::filesystem::path& path, const float* samples, std::size_t count, int sample_rate,
                     int channels = 1);

}  // namespace hm::audio
