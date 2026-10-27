// Port of services/edge-ai/context-fusion (homemind_context_fusion): observation model, explainable scoring,
// ContextFusionEngine hysteresis, versioned ContextFusionPolicy and the asynchronous FeatureStreamFusion.
// Emitted events equal the Python ones field by field (event ids aside).
#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hm/common.hpp"
#include "hm/context/pyfmt.hpp"

namespace hm::ctx {

inline constexpr const char* kFusionModelVersion = "context-fusion-rules-v1";

// jsonschema.ValidationError equivalent (contracts/rpi5-internal schemas).
class ValidationError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

bool is_quiet_required_context(std::string_view context_type);
// contracts/rpi5-internal/context-feature-observation.schema.json (date-time format is not checked,
// like the Python reference without rfc3339-validator).
void validate_feature_event(const json& event);
// contracts/rpi5-internal/context-activity-event.schema.json
void validate_context_event(const json& event);

struct ContextObservation {
  std::string audio_context = "UNKNOWN";
  double audio_confidence = 0.0;
  std::optional<std::map<std::string, double>> audio_class_probabilities;
  double speech_ratio = 0.0;
  std::optional<double> turns_per_minute;
  std::string turn_taking_method = "UNKNOWN";
  double duration_seconds = 0.0;
  std::string presence_zone = "UNKNOWN";
  double presence_confidence = 0.0;
  std::optional<bool> tv_on;
  std::optional<bool> laptop_active;
  std::optional<bool> headphones_connected;
  std::optional<std::string> audio_model_version;
  std::optional<std::string> trace_id;
  Micros timestamp = 0;

  void validate() const;  // ContextObservation.__post_init__ (throws std::invalid_argument)
  // ContextObservation.from_mapping (flat or nested audio/vision/features/device_state); missing timestamp
  // means "now" like Python.
  static ContextObservation from_mapping(const json& value);
};

struct Classification {
  std::string context_type;
  double confidence = 0.0;
  std::vector<std::string> evidence;
  std::vector<std::pair<std::string, double>> scores;  // CLASSIFIABLE_CONTEXTS order
};

Classification classify(const ContextObservation& observation, double quiet_audio_confidence = 0.0,
                        double meeting_tv_margin = 0.0);

class ContextFusionEngine {
 public:
  // Update counts keep their JSON type (int or float) because Python formats them into evidence.
  ContextFusionEngine(json activation_updates = 3, json release_updates = 2, json change_updates = 2,
                      double ttl_seconds = 10.0, double quiet_audio_confidence = 0.0,
                      double meeting_tv_margin = 0.0);
  json update(const ContextObservation& observation);
  static json sanitized_context(const json& event);

  const std::string& current_context() const { return current_context_; }
  double ttl_seconds() const { return ttl_seconds_; }

 private:
  const json& required_updates(bool predicted_quiet) const;

  json activation_updates_, release_updates_, change_updates_;
  double ttl_seconds_, quiet_audio_confidence_, meeting_tv_margin_;
  std::string current_context_ = "UNKNOWN";
  bool current_quiet_required_ = false;
  double current_confidence_ = 0.25;
  std::vector<std::string> current_evidence_{"state:unconfirmed"};
  std::pair<std::string, bool> candidate_{"UNKNOWN", false};
  long long candidate_count_ = 0;
  std::optional<Micros> last_timestamp_;
};

struct ContextFusionPolicy {
  double quiet_audio_confidence = 0.0;
  double meeting_tv_margin = 0.0;
  json activation_updates = 3;
  json release_updates = 2;
  json change_updates = 2;
  double ttl_seconds = 10.0;

  static ContextFusionPolicy from_mapping(const json& value);
  static ContextFusionPolicy from_json(const std::filesystem::path& path, bool require_accepted = true);
  ContextFusionEngine engine() const;
  json as_dict() const;
};

struct StreamIngestResult {
  bool accepted = false;
  std::string reason;
  std::string event_type;
  std::optional<json> context_event;
  std::optional<std::vector<std::pair<std::string, std::string>>> input_status;  // insertion order
  json debug_record() const;
};

class FeatureStreamFusion {
 public:
  explicit FeatureStreamFusion(ContextFusionEngine engine = ContextFusionEngine(),
                               double minimum_turn_confidence = 0.50, double minimum_presence_confidence = 0.50,
                               std::size_t duplicate_window = 4096);
  // Throws ValidationError for contract violations and std::invalid_argument for bad timestamps.
  StreamIngestResult ingest(const json& event);
  ContextFusionEngine& engine() { return engine_; }

 private:
  struct Latest {
    json event;
    Micros timestamp = 0;
    Micros valid_until = 0;
  };
  void remember(const std::string& event_id);

  ContextFusionEngine engine_;
  double minimum_turn_confidence_, minimum_presence_confidence_;
  std::size_t duplicate_window_;
  std::map<std::string, Latest> latest_;
  std::deque<std::string> seen_order_;
  std::unordered_set<std::string> seen_ids_;
};

}  // namespace hm::ctx
