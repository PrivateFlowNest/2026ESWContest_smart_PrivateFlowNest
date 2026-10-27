// Context path: 4 s window -> YAMNet embeddings + VAD features -> activity head -> AUDIO_CONTEXT feature
// event -> context-fusion (versioned policy) -> fused ACTIVITY_CONTEXT event (rpi5-internal).
// Port of services/runtime audio/context.py AudioContextPipeline (+ audio-context, context-fusion).
// INTERFACE CONTRACT between agent C (implements libs/hm_context) and agent B (calls it from
// services/homemind-perception). Change only together with both sides.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "hm/common.hpp"

namespace hm {

struct ContextUpdate {
  std::string raw_label;
  double raw_confidence = 0.0;
  double speech_ratio = 0.0;
  json feature_event;  // AUDIO_CONTEXT feature event (rpi5-internal), same fields as Python
  json context_event;  // fused ACTIVITY_CONTEXT event (rpi5-internal), same fields as Python
  double latency_ms = 0.0;
  // Added 2026-10-22 (agent F, backward compatible): live PRESENCE events from submit_presence() that were handed
  // to context-fusion right before this window, in order: [{"event": {...}, "result": "buffered"|"duplicate"|
  // "out_of_order"}]. Empty without live presence. Lets a PC test replay the exact fusion input in Python.
  json presence_events = json::array();
};

struct ContextPipelineOptions {
  std::filesystem::path yamnet_model;   // converted YAMNet (ONNX or whatever agent C ships)
  std::filesystem::path activity_head;  // exported activity head (from activity.joblib)
  std::filesystem::path fusion_policy;  // services/edge-ai/context-fusion/config/audio-policy-*.json
  std::string presence_zone = "UNKNOWN";  // AI zone; != UNKNOWN -> configured presence event (marked simulated)
  double presence_confidence = 0.9;
  bool simulated = false;
  int feature_ttl_seconds = 10;
  int threads = 2;
};

class AudioContextPipeline {
 public:
  explicit AudioContextPipeline(const ContextPipelineOptions& options);
  ~AudioContextPipeline();
  AudioContextPipeline(const AudioContextPipeline&) = delete;
  AudioContextPipeline& operator=(const AudioContextPipeline&) = delete;

  void warmup();
  // window: 16 kHz mono float32 in [-1, 1] (normally 4 s = 64000 samples).
  // timestamp_ms: wall clock of the window end; elapsed_seconds: stream time (Python duration_seconds).
  ContextUpdate process(const float* window, std::size_t samples, std::int64_t timestamp_ms, double elapsed_seconds);
  std::string model_version() const;
  std::string policy_version() const;

  // Live presence (added 2026-10-22, agent F, backward compatible): a camera PRESENCE observation
  // (contracts/rpi5-internal/context-feature-observation, event_type PRESENCE, e.g. homemind-vision over a local
  // FIFO). Thread-safe; may be called while process() runs on another thread. Validated here: throws
  // ctx::ValidationError / std::invalid_argument for an invalid event or another event_type, std::logic_error when
  // the presence_zone override is configured (the two are exclusive). Valid events wait in a small queue (64, the
  // oldest is dropped) and are ingested in timestamp order right before the first window whose timestamp is at or
  // after their own: the audio window is the fusion clock, and ingesting a presence newer than the window would
  // make FeatureStreamFusion report it "future" and lose the previous, still fresh one.
  void submit_presence(const json& event);
  std::size_t pending_presence() const;  // queued, not yet ingested
  std::size_t dropped_presence() const;  // dropped because the queue was full

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hm
