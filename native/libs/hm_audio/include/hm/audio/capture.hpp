// ALSA microphone capture -> 16 kHz mono float32 blocks of 100 ms (services/runtime capture.py
// MicrophoneSource). Asks the device for 16 kHz mono S16; a device that cannot do 16 kHz is opened at its
// own rate and converted with the scipy-compatible StreamResampler; multi-channel devices use channel 0.
// The capture thread survives device errors: it logs, closes and reopens the device every 2 s.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hm::audio {

struct CaptureDevice {
  std::string name;         // ALSA PCM name to open, e.g. "default", "plughw:CARD=Device,DEV=0"
  std::string description;  // card / PCM description
  bool is_default = false;
};

// Capture-capable PCMs: "default" and named plugins from the ALSA hints, then one plughw entry per
// hardware capture device. Empty when ALSA support was not compiled in.
std::vector<CaptureDevice> list_capture_devices();

// Setting audio_device -> PCM name (Python resolve_device semantics adapted to ALSA):
//   null / ""             -> "default"
//   digits "N"            -> "plughw:N" (card index)
//   contains ':' or is a listed PCM name -> used as is
//   anything else         -> first device whose name or description contains it (case-insensitive)
// Throws std::runtime_error when nothing matches ("run with --list-devices").
std::string resolve_capture_device(const std::optional<std::string>& spec);

class AlsaCapture {
 public:
  using Sink = std::function<void(const float* samples, std::size_t count)>;  // 1600-sample blocks, [-1, 1]

  AlsaCapture(std::optional<std::string> device_spec, Sink sink);
  ~AlsaCapture();
  AlsaCapture(const AlsaCapture&) = delete;
  AlsaCapture& operator=(const AlsaCapture&) = delete;

  void start();
  void stop();
  int native_rate() const { return native_rate_.load(); }
  std::size_t reopen_count() const { return reopens_.load(); }

 private:
  void run();

  std::optional<std::string> spec_;
  Sink sink_;
  std::atomic<bool> stop_{false};
  std::atomic<int> native_rate_{16000};
  std::atomic<std::size_t> reopens_{0};
  std::thread thread_;
};

bool alsa_available();

}  // namespace hm::audio
