// Partial-intent classifier for streaming ASR partials (NODE-RPI5-02). Port of
// services/edge-ai/partial-intent: features.py (signed FNV-1a char n-gram hashing + Korean cue features),
// runtime.py (IntentRuntime.predict, StreamIntentGate), policy.py and taxonomy.py.
// Output events carry the SHA-256 and length of the normalized text, never the text, and execution_allowed
// is always false: a partial intent is a preparation hint only.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hm/common.hpp"
#include "hm/partial/text.hpp"

namespace hm {
class ActionMap;
}

namespace hm::partial {

// INTENTS (taxonomy.py order)
extern const std::vector<std::string> kIntents;

// vectorize_one(text, dimensions) in float32, same bucket/sign/feature layout as features.py.
std::vector<float> vectorize_one(const CodePoints& text, int dimensions = 4096);

std::string apply_safety_policy(const CodePoints& text, const std::string& predicted_intent);
bool requires_final_transcript(const CodePoints& text);

struct Hints {
  std::vector<std::string> candidate_devices;
  std::vector<std::string> safe_preparations;
};
const Hints& hints_for(const std::string& intent);

struct ModelMetadata {
  std::string model_version;
  std::vector<std::string> classes;
  int feature_dimensions = 4096;
  double confidence_threshold = 0.35;
  std::optional<double> streaming_threshold;
  static ModelMetadata load(const std::filesystem::path& path);
};

class IntentRuntime {
 public:
  // metadata_path empty -> model path with .json suffix (like Python)
  explicit IntentRuntime(const std::filesystem::path& model_path, const std::filesystem::path& metadata_path = {});
  ~IntentRuntime();
  IntentRuntime(const IntentRuntime&) = delete;
  IntentRuntime& operator=(const IntentRuntime&) = delete;

  // Class probabilities in metadata class order (float32 widened to double).
  std::vector<double> probabilities(const CodePoints& text) const;
  // IntentRuntime.predict(): the rpi5-internal partial-intent event (contracts/rpi5-internal/partial-intent.schema.json).
  json predict(std::string_view utf8_text, bool is_final = false, const std::optional<std::string>& trace_id = std::nullopt,
               std::optional<double> threshold = std::nullopt) const;
  json predict(const CodePoints& text, bool is_final, const std::optional<std::string>& trace_id,
               std::optional<double> threshold) const;

  const ModelMetadata& metadata() const { return metadata_; }
  double threshold() const { return metadata_.confidence_threshold; }

 private:
  struct Session;
  ModelMetadata metadata_;
  std::unique_ptr<Session> session_;
};

// Require `confirmation_updates` matching DETECTED updates before an early (non-final) intent is emitted,
// and suppress an intent once it has been emitted.
class StreamIntentGate {
 public:
  explicit StreamIntentGate(const IntentRuntime& runtime, int confirmation_updates = 2,
                            std::optional<double> threshold = std::nullopt);
  json update(std::string_view utf8_text, bool is_final = false, const std::optional<std::string>& trace_id = std::nullopt);
  json update(const CodePoints& text, bool is_final, const std::optional<std::string>& trace_id);
  double threshold() const { return threshold_; }
  void reset();

 private:
  const IntentRuntime& runtime_;
  int confirmation_updates_;
  double threshold_;
  std::optional<std::string> candidate_;
  int count_ = 0;
  std::optional<std::string> emitted_;
};

// Native glue (not in Python): one gate per utterance. A new utterance_id starts a fresh gate, so an intent
// emitted in one utterance is not suppressed in the next (the Python `stream` CLI keeps one gate forever).
class UtteranceIntentStream {
 public:
  explicit UtteranceIntentStream(const IntentRuntime& runtime, int confirmation_updates = 2,
                                 std::optional<double> threshold = std::nullopt);
  json update(std::string_view utf8_text, bool is_final, const std::string& utterance_id);

 private:
  const IntentRuntime& runtime_;
  int confirmation_updates_;
  std::optional<double> threshold_;
  std::string utterance_id_;
  std::unique_ptr<StreamIntentGate> gate_;
};

// pfn.v1 perception PARTIAL_INTENT_HINT for a DETECTED event (nullopt otherwise). AI devices map to registry
// device ids through contracts/pfn.v1/ai-action-map.json (devices without hardware are dropped); the zone is the
// microphone's registry zone.
std::optional<json> hint_perception(const json& event, const hm::ActionMap& action_map, bool simulated,
                                    const std::string& source_device_id = "MIC-INDOOR-01");

// Every term/phrase list and the HINTS table (parity tests compare them with the Python modules).
json term_tables();

// Python round(value, digits) (correctly rounded decimal, half-even), e.g. round(confidence, 6).
double py_round(double value, int digits);
// datetime.now(timezone.utc).isoformat(): "YYYY-MM-DDTHH:MM:SS[.ffffff]+00:00"
std::string utc_isoformat_now();

}  // namespace hm::partial
