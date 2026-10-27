#include "hm/audio/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "hm/audio/common.hpp"
#include "hm/audio/resample.hpp"

namespace hm::audio {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMinSamples = static_cast<std::size_t>(kSampleRate);  // Python MIN_SAMPLES (1 s)

std::uint32_t le32(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const unsigned char* p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

[[noreturn]] void fail(const fs::path& path, const std::string& what) {
  throw std::runtime_error(path.string() + ": " + what);
}

// One sample -> float32 like homemind_audio.audio._to_float32 (floats are clipped to [-1, 1]).
float convert_sample(const unsigned char* p, int format, int width) {
  if (format == 3) {
    if (width == 4) {
      float value;
      std::memcpy(&value, p, 4);
      return std::clamp(value, -1.0f, 1.0f);
    }
    double value;
    std::memcpy(&value, p, 8);
    return std::clamp(static_cast<float>(value), -1.0f, 1.0f);
  }
  switch (width) {
    case 1:
      return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
    case 2:
      return static_cast<float>(static_cast<std::int16_t>(le16(p))) / 32768.0f;
    case 3: {
      std::int32_t value = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
      if (value & 0x800000) value -= 0x1000000;
      return static_cast<float>(value) / 8388608.0f;
    }
    default:
      return static_cast<float>(static_cast<std::int32_t>(le32(p))) / 2147483648.0f;
  }
}

std::vector<float> normalise_16k(std::vector<float> samples, int sample_rate) {
  if (sample_rate != kSampleRate) {
    int up = 1;
    int down = 1;
    rational_factors(sample_rate, kSampleRate, up, down);
    samples = resample_poly(samples, up, down);
  }
  if (samples.size() < kMinSamples) samples.resize(kMinSamples, 0.0f);
  for (float& value : samples) value = std::clamp(value, -1.0f, 1.0f);
  return samples;
}

std::int64_t round_half_even(double value) { return static_cast<std::int64_t>(std::nearbyint(value)); }

void put32(std::string& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}
void put16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

std::string wav_header(int format, int channels, int sample_rate, int bits, std::uint32_t data_bytes) {
  std::string out = "RIFF";
  put32(out, 36 + data_bytes);
  out += "WAVEfmt ";
  put32(out, 16);
  put16(out, static_cast<std::uint16_t>(format));
  put16(out, static_cast<std::uint16_t>(channels));
  put32(out, static_cast<std::uint32_t>(sample_rate));
  const int block_align = channels * bits / 8;
  put32(out, static_cast<std::uint32_t>(sample_rate * block_align));
  put16(out, static_cast<std::uint16_t>(block_align));
  put16(out, static_cast<std::uint16_t>(bits));
  out += "data";
  put32(out, data_bytes);
  return out;
}

void write_file(const fs::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) throw std::runtime_error("cannot write " + path.string());
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream) throw std::runtime_error("write failed: " + path.string());
}

}  // namespace

WavInfo read_wav_info(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) fail(path, "cannot open");
  stream.seekg(0, std::ios::end);
  const auto file_size = static_cast<std::int64_t>(stream.tellg());
  stream.seekg(0);
  unsigned char riff[12];
  if (!stream.read(reinterpret_cast<char*>(riff), 12) || std::memcmp(riff, "RIFF", 4) != 0 ||
      std::memcmp(riff + 8, "WAVE", 4) != 0) {
    fail(path, "not a RIFF/WAVE file");
  }
  WavInfo info;
  int& block_align = info.block_align;
  bool have_fmt = false;
  std::int64_t data_size = -1;
  std::int64_t position = 12;
  while (position + 8 <= file_size) {
    unsigned char header[8];
    stream.seekg(position);
    if (!stream.read(reinterpret_cast<char*>(header), 8)) break;
    const std::int64_t size = le32(header + 4);
    const std::int64_t body = position + 8;
    if (std::memcmp(header, "fmt ", 4) == 0) {
      if (size < 16) fail(path, "short fmt chunk");
      unsigned char fmt[40] = {};
      stream.read(reinterpret_cast<char*>(fmt), static_cast<std::streamsize>(std::min<std::int64_t>(size, 40)));
      info.format = le16(fmt);
      info.channels = le16(fmt + 2);
      info.sample_rate = static_cast<int>(le32(fmt + 4));
      block_align = le16(fmt + 12);
      info.bits_per_sample = le16(fmt + 14);
      if (info.format == 0xFFFE) {
        if (size < 40) fail(path, "short WAVE_FORMAT_EXTENSIBLE fmt chunk");
        info.format = le16(fmt + 24);  // sub format GUID starts with the format code
      }
      have_fmt = true;
    } else if (std::memcmp(header, "data", 4) == 0) {
      info.data_offset = body;
      data_size = std::min<std::int64_t>(size, file_size - body);  // tolerate truncated/streamed files
      if (have_fmt) break;
    }
    position = body + size + (size & 1);
  }
  if (!have_fmt) fail(path, "missing fmt chunk");
  if (data_size < 0) fail(path, "missing data chunk");
  if (info.format != 1 && info.format != 3) fail(path, "unsupported WAV format code " + std::to_string(info.format));
  if (info.channels < 1 || info.sample_rate < 1 || block_align < 1 || block_align % info.channels != 0) {
    fail(path, "invalid fmt chunk");
  }
  const int width = block_align / info.channels;
  const bool ok = info.format == 3 ? (width == 4 || width == 8) : (width >= 1 && width <= 4);
  if (!ok) fail(path, "unsupported sample width " + std::to_string(width * 8) + " bits");
  info.frames = data_size / block_align;
  return info;
}

std::vector<float> read_wav_frames_mono(const fs::path& path, const WavInfo& info, std::int64_t first,
                                        std::int64_t count) {
  first = std::clamp<std::int64_t>(first, 0, info.frames);
  count = std::clamp<std::int64_t>(count, 0, info.frames - first);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) fail(path, "cannot open");
  const int channels = info.channels;
  const int sample_width = info.block_align / channels;
  const std::int64_t frame_bytes = info.block_align;
  std::vector<float> mono;
  std::vector<unsigned char> raw(static_cast<std::size_t>(count * frame_bytes));
  stream.seekg(info.data_offset + first * frame_bytes);
  if (count > 0 && !stream.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
    fail(path, "truncated data chunk");
  }
  mono.resize(static_cast<std::size_t>(count));
  for (std::int64_t frame = 0; frame < count; ++frame) {
    const unsigned char* p = raw.data() + frame * frame_bytes;
    if (channels == 1) {
      mono[static_cast<std::size_t>(frame)] = convert_sample(p, info.format, sample_width);
      continue;
    }
    // numpy mean(axis=1) of float32: float32 sum in channel order, then divide by the count.
    float sum = 0.0f;
    for (int channel = 0; channel < channels; ++channel) {
      sum += convert_sample(p + channel * sample_width, info.format, sample_width);
    }
    mono[static_cast<std::size_t>(frame)] = sum / static_cast<float>(channels);
  }
  return mono;
}

std::vector<float> load_wav_16k_mono(const fs::path& path) {
  const WavInfo info = read_wav_info(path);
  return normalise_16k(read_wav_frames_mono(path, info, 0, info.frames), info.sample_rate);
}

std::vector<float> load_wav_segment_16k_mono(const fs::path& path, double start_seconds, double end_seconds) {
  if (start_seconds < 0 || end_seconds <= start_seconds) throw std::invalid_argument("Invalid WAV segment bounds");
  const WavInfo info = read_wav_info(path);
  const std::int64_t start = std::max<std::int64_t>(0, round_half_even(start_seconds * info.sample_rate));
  const std::int64_t end = std::min<std::int64_t>(info.frames, round_half_even(end_seconds * info.sample_rate));
  if (end <= start) fail(path, "segment is outside the file");
  return normalise_16k(read_wav_frames_mono(path, info, start, end - start), info.sample_rate);
}

std::string encode_pcm16_wav(const float* samples, std::size_t count, int sample_rate) {
  std::string out = wav_header(1, 1, sample_rate, 16, static_cast<std::uint32_t>(count * 2));
  out.reserve(out.size() + count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    const float value = std::clamp(samples[i], -1.0f, 1.0f);
    const auto pcm = static_cast<std::int16_t>(value * 32767.0f);  // numpy astype: truncation toward zero
    put16(out, static_cast<std::uint16_t>(pcm));
  }
  return out;
}

void write_pcm16_wav(const fs::path& path, const float* samples, std::size_t count, int sample_rate) {
  write_file(path, encode_pcm16_wav(samples, count, sample_rate));
}

void write_float_wav(const fs::path& path, const float* samples, std::size_t count, int sample_rate, int channels) {
  std::string out = wav_header(3, channels, sample_rate, 32, static_cast<std::uint32_t>(count * 4));
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t bits;
    std::memcpy(&bits, &samples[i], 4);
    put32(out, bits);
  }
  write_file(path, out);
}

}  // namespace hm::audio
