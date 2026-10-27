#include "hm/audio/capture.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>

#include "hm/audio/resample.hpp"
#include "hm/common.hpp"

#if HM_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace hm::audio {

namespace {

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  return text;
}

[[maybe_unused]] std::string one_line(const char* text) {
  std::string out;
  for (const char* p = text; *p; ++p) out.push_back(*p == '\n' ? ' ' : *p);
  return out;
}

bool all_digits(const std::string& text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

}  // namespace

#if HM_HAVE_ALSA

bool alsa_available() { return true; }

std::vector<CaptureDevice> list_capture_devices() {
  std::vector<CaptureDevice> out;
  std::set<std::string> seen;
  void** hints = nullptr;
  if (snd_device_name_hint(-1, "pcm", &hints) == 0) {
    for (void** hint = hints; *hint; ++hint) {
      char* name = snd_device_name_get_hint(*hint, "NAME");
      char* desc = snd_device_name_get_hint(*hint, "DESC");
      char* io = snd_device_name_get_hint(*hint, "IOID");
      const bool capture = io == nullptr || std::strcmp(io, "Input") == 0;
      if (name && capture && std::strcmp(name, "null") != 0 && seen.insert(name).second) {
        out.push_back({name, desc ? one_line(desc) : "", std::strcmp(name, "default") == 0});
      }
      std::free(name);
      std::free(desc);
      std::free(io);
    }
    snd_device_name_free_hint(hints);
  }
  int card = -1;
  while (snd_card_next(&card) == 0 && card >= 0) {
    snd_ctl_t* ctl = nullptr;
    const std::string hw = "hw:" + std::to_string(card);
    if (snd_ctl_open(&ctl, hw.c_str(), 0) < 0) continue;
    snd_ctl_card_info_t* card_info = nullptr;
    snd_pcm_info_t* pcm_info = nullptr;
    snd_ctl_card_info_malloc(&card_info);
    snd_pcm_info_malloc(&pcm_info);
    if (snd_ctl_card_info(ctl, card_info) == 0) {
      const std::string id = snd_ctl_card_info_get_id(card_info);
      const std::string card_name = snd_ctl_card_info_get_name(card_info);
      int device = -1;
      while (snd_ctl_pcm_next_device(ctl, &device) == 0 && device >= 0) {
        snd_pcm_info_set_device(pcm_info, static_cast<unsigned>(device));
        snd_pcm_info_set_subdevice(pcm_info, 0);
        snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_CAPTURE);
        if (snd_ctl_pcm_info(ctl, pcm_info) < 0) continue;
        const std::string name = "plughw:CARD=" + id + ",DEV=" + std::to_string(device);
        if (!seen.insert(name).second) continue;
        out.push_back({name, card_name + ": " + snd_pcm_info_get_name(pcm_info) + " (card " + std::to_string(card) + ")",
                       false});
      }
    }
    snd_pcm_info_free(pcm_info);
    snd_ctl_card_info_free(card_info);
    snd_ctl_close(ctl);
  }
  return out;
}

namespace {

struct OpenPcm {
  snd_pcm_t* pcm = nullptr;
  snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
  unsigned channels = 1;
  unsigned rate = 16000;
  snd_pcm_uframes_t period = 1600;
  ~OpenPcm() {
    if (pcm) snd_pcm_close(pcm);
  }
};

// Opens and configures the device; returns an empty string or the reason it failed.
std::string open_pcm(const std::string& name, OpenPcm& out) {
  int rc = snd_pcm_open(&out.pcm, name.c_str(), SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    out.pcm = nullptr;
    return std::string("open: ") + snd_strerror(rc);
  }
  snd_pcm_hw_params_t* hw = nullptr;
  snd_pcm_hw_params_malloc(&hw);
  auto done = [&](const std::string& error) {
    snd_pcm_hw_params_free(hw);
    return error;
  };
  if ((rc = snd_pcm_hw_params_any(out.pcm, hw)) < 0) return done(std::string("hw_params_any: ") + snd_strerror(rc));
  if ((rc = snd_pcm_hw_params_set_access(out.pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    return done(std::string("access: ") + snd_strerror(rc));
  }
  bool format_ok = false;
  for (snd_pcm_format_t format : {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_FLOAT_LE}) {
    if (snd_pcm_hw_params_test_format(out.pcm, hw, format) == 0 &&
        snd_pcm_hw_params_set_format(out.pcm, hw, format) == 0) {
      out.format = format;
      format_ok = true;
      break;
    }
  }
  if (!format_ok) return done("no supported sample format (S16_LE, S32_LE, FLOAT_LE)");
  if (snd_pcm_hw_params_set_channels(out.pcm, hw, 1) == 0) {
    out.channels = 1;
  } else {
    unsigned channels = 1;
    snd_pcm_hw_params_get_channels_min(hw, &channels);
    if ((rc = snd_pcm_hw_params_set_channels(out.pcm, hw, channels)) < 0) {
      return done(std::string("channels: ") + snd_strerror(rc));
    }
    out.channels = channels;
  }
  unsigned rate = 16000;
  if ((rc = snd_pcm_hw_params_set_rate_near(out.pcm, hw, &rate, nullptr)) < 0) {
    return done(std::string("rate: ") + snd_strerror(rc));
  }
  out.rate = rate;
  snd_pcm_uframes_t period = std::max(1u, rate / 10);
  snd_pcm_hw_params_set_period_size_near(out.pcm, hw, &period, nullptr);
  snd_pcm_uframes_t buffer = period * 8;
  snd_pcm_hw_params_set_buffer_size_near(out.pcm, hw, &buffer);
  if ((rc = snd_pcm_hw_params(out.pcm, hw)) < 0) return done(std::string("hw_params: ") + snd_strerror(rc));
  snd_pcm_hw_params_get_period_size(hw, &period, nullptr);
  out.period = std::max<snd_pcm_uframes_t>(1, period);
  return done("");
}

bool sleep_unless(const std::atomic<bool>& stop, int milliseconds) {
  for (int waited = 0; waited < milliseconds && !stop; waited += 100) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return !stop;
}

}  // namespace

void AlsaCapture::run() {
  int failures = 0;
  while (!stop_) {
    std::string name;
    try {
      name = resolve_capture_device(spec_);
    } catch (const std::exception& error) {
      if (failures++ % 15 == 0) hm::log(hm::LogLevel::error, "capture", std::string(error.what()) + " (retrying)");
      sleep_unless(stop_, 2000);
      continue;
    }
    OpenPcm device;
    const std::string error = open_pcm(name, device);
    if (!error.empty()) {
      if (failures++ % 15 == 0) {
        hm::log(hm::LogLevel::error, "capture", "microphone " + name + ": " + error + " (retrying every 2 s)");
      }
      sleep_unless(stop_, 2000);
      continue;
    }
    failures = 0;
    native_rate_ = static_cast<int>(device.rate);
    hm::log(hm::LogLevel::info, "capture",
            "microphone device=" + name + " native_rate=" + std::to_string(device.rate) +
                " channels=" + std::to_string(device.channels) + " format=" + snd_pcm_format_name(device.format));
    int up = 1;
    int down = 1;
    rational_factors(static_cast<int>(device.rate), 16000, up, down);
    StreamResampler resampler(up, down);
    const std::size_t sample_bytes = static_cast<std::size_t>(snd_pcm_format_physical_width(device.format)) / 8;
    std::vector<unsigned char> raw(device.period * device.channels * sample_bytes);
    std::vector<float> mono(device.period);
    std::vector<float> pending;
    bool healthy = true;
    while (!stop_ && healthy) {
      const snd_pcm_sframes_t got = snd_pcm_readi(device.pcm, raw.data(), device.period);
      if (got == -EAGAIN) continue;
      if (got < 0) {
        if (got == -EPIPE) hm::log(hm::LogLevel::debug, "capture", "overrun");
        const int rc = snd_pcm_recover(device.pcm, static_cast<int>(got), 1);
        if (rc < 0) {
          hm::log(hm::LogLevel::error, "capture", std::string("read failed: ") + snd_strerror(rc) + "; reopening");
          healthy = false;
        }
        continue;
      }
      const auto frames = static_cast<std::size_t>(got);
      for (std::size_t i = 0; i < frames; ++i) {
        const unsigned char* p = raw.data() + i * device.channels * sample_bytes;  // channel 0
        float value = 0.0f;
        if (device.format == SND_PCM_FORMAT_S16_LE) {
          std::int16_t v;
          std::memcpy(&v, p, 2);
          value = static_cast<float>(v) / 32768.0f;
        } else if (device.format == SND_PCM_FORMAT_S32_LE) {
          std::int32_t v;
          std::memcpy(&v, p, 4);
          value = static_cast<float>(v) / 2147483648.0f;
        } else {
          std::memcpy(&value, p, 4);
        }
        mono[i] = value;
      }
      resampler.push(mono.data(), frames, pending);
      std::size_t offset = 0;
      while (pending.size() - offset >= 1600) {
        float* block = pending.data() + offset;
        for (std::size_t i = 0; i < 1600; ++i) block[i] = std::clamp(block[i], -1.0f, 1.0f);
        sink_(block, 1600);
        offset += 1600;
      }
      pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    if (!stop_) {
      ++reopens_;
      sleep_unless(stop_, 2000);
    }
  }
}

#else  // no ALSA headers at build time

bool alsa_available() { return false; }
std::vector<CaptureDevice> list_capture_devices() { return {}; }
void AlsaCapture::run() { hm::log(hm::LogLevel::error, "capture", "built without ALSA; no microphone input"); }

#endif

std::string resolve_capture_device(const std::optional<std::string>& spec) {
  if (!spec || spec->empty()) return "default";
  const std::string& value = *spec;
  if (all_digits(value)) return "plughw:" + value;
  if (value.find(':') != std::string::npos) return value;
  const auto devices = list_capture_devices();
  for (const auto& device : devices) {
    if (device.name == value) return value;
  }
  const std::string needle = lower(value);
  for (const auto& device : devices) {
    if (lower(device.name).find(needle) != std::string::npos ||
        lower(device.description).find(needle) != std::string::npos) {
      return device.name;
    }
  }
  if (value == "default" || value == "null") return value;
  throw std::runtime_error("no input device matches '" + value + "'; run with --list-devices");
}

AlsaCapture::AlsaCapture(std::optional<std::string> device_spec, Sink sink)
    : spec_(std::move(device_spec)), sink_(std::move(sink)) {}

AlsaCapture::~AlsaCapture() { stop(); }

void AlsaCapture::start() {
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void AlsaCapture::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

}  // namespace hm::audio
