// What PerceptionService needs from the context model, the speech recogniser and the voice parser, as small
// interfaces so tests and the PC harness can plug in doubles. The real implementations wrap
//   hm::AudioContextPipeline (libs/hm_context, agent C)  -> ContextEngine
//   hm::stt::Transcriber     (libs/hm_stt)                -> SpeechToText
//   hm::VoiceParser          (libs/hm_parser, agent A)    -> VoiceDecider
// The two contract headers are always on the include path; the libraries are linked when CMake finds the
// targets (HM_PERCEPTION_HAVE_CONTEXT / HM_PERCEPTION_HAVE_PARSER), otherwise that path is unavailable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hm/action_map.hpp"
#include "hm/common.hpp"
#include "hm/config.hpp"
#include "hm/context_pipeline.hpp"
#include "hm/stt.hpp"
#include "hm/voice_parser.hpp"
#include "settings.hpp"

namespace hm::perception {

class ContextEngine {
 public:
  virtual ~ContextEngine() = default;
  virtual void warmup() {}
  virtual hm::ContextUpdate process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                                    double elapsed_seconds) = 0;
  virtual std::string model_version() const = 0;
  virtual std::optional<std::string> policy_version() const = 0;
  // Live camera presence (rpi5-internal PRESENCE observation, presence_fifo). Thread-safe. Throws for an invalid
  // event; the default engine has no live presence.
  virtual void submit_presence(const json& event);
};

class SpeechToText {
 public:
  virtual ~SpeechToText() = default;
  virtual hm::stt::SttResult transcribe(const std::vector<float>& samples) = 0;  // throws on failure
  virtual void close() {}
};

class VoiceDecider {
 public:
  virtual ~VoiceDecider() = default;
  // trace_id: the utterance's trace (shared with its partial-intent hints); nullopt = a new one per message set.
  virtual hm::VoiceDecision handle(const std::string& text, const json& context, const std::string& stt_model_version,
                                   std::optional<double> stt_confidence, std::optional<std::string> utterance_id,
                                   std::optional<std::string> trace_id) = 0;
};

// Partial transcripts while an utterance is captured (Vosk via libs/hm_vosk). One session per utterance.
class PartialSession {
 public:
  virtual ~PartialSession() = default;
  virtual std::vector<std::string> accept(const float* samples, std::size_t count) = 0;  // new partial texts
  virtual std::vector<std::string> flush() = 0;  // the last short chunk (file mode)
};

class PartialTranscriber {
 public:
  virtual ~PartialTranscriber() = default;
  virtual std::unique_ptr<PartialSession> start() = 0;
  virtual std::string model_version() const = 0;
};

// Partial-intent classifier (libs/hm_partial): one gate per utterance (StreamIntentGate), DETECTED events become
// PARTIAL_INTENT_HINT perceptions (execution_allowed false).
class IntentGate {
 public:
  virtual ~IntentGate() = default;
  virtual json update(const std::string& partial_text, const std::string& trace_id) = 0;  // partial-intent event
};

class PartialIntentModel {
 public:
  virtual ~PartialIntentModel() = default;
  virtual std::unique_ptr<IntentGate> gate(int confirmation_updates) = 0;
  virtual std::optional<json> hint(const json& event, const hm::ActionMap& action_map, bool simulated) = 0;
  virtual std::string model_version() const = 0;
};

bool context_engine_available();  // built with libs/hm_context
bool voice_parser_available();    // built with libs/hm_parser
bool partial_intent_available();  // built with libs/hm_vosk and libs/hm_partial

// Real implementations. Throw when files are missing (callers decide whether that disables a path).
std::unique_ptr<ContextEngine> make_context_engine(const Settings& settings, bool simulated);
std::unique_ptr<SpeechToText> make_transcriber(const Settings& settings);
std::unique_ptr<VoiceDecider> make_voice_decider(const hm::ActionMap& action_map, const Settings& settings,
                                                 const hm::RuntimeConfig& config, bool simulated);
std::unique_ptr<PartialTranscriber> make_partial_transcriber(const Settings& settings);   // loads libvosk + model
std::unique_ptr<PartialIntentModel> make_partial_intent_model(const Settings& settings);  // ONNX + metadata

}  // namespace hm::perception
