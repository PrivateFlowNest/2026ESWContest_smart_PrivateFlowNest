// Energy VAD endpointing for the voice path: port of services/runtime audio/vad.py (EnergyEndpointer),
// same frames, thresholds, noise floor and decisions on the same float32 input.
//
// Frame RMS in dBFS against an adaptive noise floor. An utterance starts once start_ratio of the last
// start_ms is voiced, ends after end_silence_ms of silence or at max_ms, and is dropped when it has less
// than min_ms of voiced frames. armed=false keeps the noise floor tracking but never starts by itself
// (push-to-talk idle); begin() opens an utterance with pre-roll and silence only ends it after an onset.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "hm/common.hpp"

namespace hm::audio {

struct EndpointConfig {
  int frame_ms = 20;
  int start_ms = 200;
  double start_ratio = 0.8;
  int end_silence_ms = 700;
  int max_ms = 8000;
  int min_ms = 400;
  int pre_roll_ms = 300;
  int tail_ms = 200;
  double threshold_db = -45.0;
  double noise_margin_db = 10.0;
  double noise_adapt = 0.05;

  // null/{} -> defaults. Unknown keys and invalid values throw std::invalid_argument (Python ValueError).
  static EndpointConfig from_json(const json& value);
  void validate() const;
};

struct Utterance {
  std::vector<float> samples;
  std::int64_t start_sample = 0;
  std::int64_t end_sample = 0;
  std::string reason;  // silence | max_length | manual | flush
  double seconds() const { return static_cast<double>(samples.size()) / 16000.0; }
};

// Per-frame decision, for parity tests.
struct FrameTrace {
  std::int64_t start_sample;
  double level_db;
  double threshold_db;
  bool voiced;
  bool active;  // an utterance was open before this frame
};

class EnergyEndpointer {
 public:
  explicit EnergyEndpointer(EndpointConfig config = {}, bool armed = true);

  bool armed = true;
  std::optional<double> noise_floor_db;
  std::function<void(const FrameTrace&)> trace;  // optional

  bool in_speech() const { return active_; }
  bool onset_seen() const { return active_ && onset_seen_; }
  const EndpointConfig& config() const { return config_; }
  std::size_t frame_samples() const { return frame_; }

  // Streaming view of the open utterance (partial transcripts while it is captured; does not change decisions).
  // activations() counts every opened utterance (begin() or onset), so a caller can tell a new utterance apart.
  std::uint64_t activations() const { return activations_; }
  std::size_t active_frame_count() const { return active_ ? active_frames_.size() : 0; }
  // Appends the samples of open-utterance frames [from_frame, active_frame_count()) to out.
  void copy_active_frames(std::size_t from_frame, std::vector<float>& out) const;

  void begin();
  double threshold_db() const;
  std::vector<Utterance> feed(const float* samples, std::size_t count);
  std::vector<Utterance> feed(const std::vector<float>& samples) { return feed(samples.data(), samples.size()); }
  std::optional<Utterance> stop(const std::string& reason = "manual");
  void reset();

 private:
  struct Frame {
    std::vector<float> samples;
    bool voiced;
  };
  std::optional<Utterance> frame(const float* samples);
  std::optional<Utterance> finish(const std::string& reason, bool trim_silence);
  void push_history(Frame frame);
  void push_recent(bool voiced);
  bool start_condition() const;

  EndpointConfig config_;
  std::size_t frame_;
  std::size_t start_frames_, end_frames_, max_frames_, min_frames_, pre_frames_;
  std::int64_t tail_frames_;  // may be negative like Python floor division
  std::vector<float> pending_;
  std::int64_t position_ = 0;  // samples consumed into frames
  std::deque<Frame> history_;
  std::deque<bool> recent_;
  bool active_ = false;
  std::vector<std::vector<float>> active_frames_;
  std::int64_t active_start_ = 0;
  std::size_t silence_run_ = 0;
  std::size_t voiced_frames_ = 0;
  bool onset_seen_ = false;
  std::uint64_t activations_ = 0;
};

}  // namespace hm::audio
