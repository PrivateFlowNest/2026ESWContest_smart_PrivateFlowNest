// Port of homemind_context_fusion (models, scoring, engine, policy_config, stream_fusion, validation).
// Arithmetic and string formatting follow the Python reference step by step; compile with -ffp-contract=off.
#include "hm/context/fusion.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <set>

namespace hm::ctx {
namespace {

constexpr std::array<const char*, 7> kContextTypes = {"QUIET",           "SELF_TALK",         "TV_MEDIA", "ZOOM_SPEAKER",
                                                      "ZOOM_HEADPHONES", "IN_PERSON_MEETING", "UNKNOWN"};
constexpr std::array<const char*, 7> kPresenceZones = {"BED",      "DESK",   "BATHROOM", "ENTRANCE",
                                                       "COMMON",   "OUTDOOR", "UNKNOWN"};
constexpr std::array<const char*, 3> kTurnTakingMethods = {"SPEAKER_CHANGE", "SPEECH_BURST_PROXY", "UNKNOWN"};
constexpr std::array<const char*, 3> kAuxiliaryEventTypes = {"TURN_TAKING", "PRESENCE", "DEVICE_STATE"};
constexpr std::size_t kClassifiable = 6;  // CONTEXT_TYPES[:-1]

template <std::size_t N>
bool contains(const std::array<const char*, N>& values, std::string_view value) {
  for (const char* item : values)
    if (item != nullptr && value == item) return true;
  return false;
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  return text;
}

std::vector<std::string> unique_in_order(const std::vector<std::string>& items) {  // list(dict.fromkeys(...))
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto& item : items)
    if (seen.insert(item).second) out.push_back(item);
  return out;
}

std::string activity_for(const std::string& context_type, const std::string& zone) {
  if (is_quiet_required_context(context_type)) return "MEETING";
  if (zone == "BATHROOM") return "BATHROOM_USE";
  if (zone == "BED") return "SLEEPING";
  if (zone == "DESK") return context_type == "QUIET" ? "STUDYING" : "WORKING";
  if (zone == "ENTRANCE") return "ENTERING";
  if (zone == "COMMON" || zone == "OUTDOOR") return "GENERAL_ACTIVITY";
  return "UNKNOWN";
}

// ---- small JSON Schema helpers (only what the rpi5-internal schemas use) ----
[[noreturn]] void invalid(const std::string& message) { throw ValidationError(message); }

void require_object(const json& value, const char* where) {
  if (!value.is_object()) invalid(std::string(where) + " is not of type 'object'");
}

void check_keys(const json& object, std::initializer_list<const char*> allowed, std::initializer_list<const char*> required,
                const char* where) {
  for (const auto& [key, unused] : object.items()) {
    (void)unused;
    if (std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return key == name; }))
      invalid(std::string(where) + ": additional property '" + key + "' is not allowed");
  }
  for (const char* name : required)
    if (!object.contains(name)) invalid(std::string(where) + ": '" + name + "' is a required property");
}

void check_enum(const json& object, const char* key, std::initializer_list<const char*> values) {
  if (!object.contains(key)) return;
  const json& value = object.at(key);
  if (value.is_string() &&
      std::any_of(values.begin(), values.end(), [&](const char* item) { return value.get<std::string>() == item; }))
    return;
  invalid(std::string(key) + " is not one of the allowed values");  // value not quoted (may reach logs)
}

void check_string(const json& object, const char* key, bool non_empty) {
  if (!object.contains(key)) return;
  const json& value = object.at(key);
  if (!value.is_string()) invalid(std::string(key) + " is not of type 'string'");
  if (non_empty && value.get<std::string>().empty()) invalid(std::string(key) + " should be non-empty");
}

void check_number(const json& object, const char* key, std::optional<double> minimum, std::optional<double> maximum) {
  if (!object.contains(key)) return;
  const json& value = object.at(key);
  if (!value.is_number()) invalid(std::string(key) + " is not of type 'number'");
  const double x = value.get<double>();
  if ((minimum && x < *minimum) || (maximum && x > *maximum)) invalid(std::string(key) + " is out of range");
}

void check_boolean(const json& object, const char* key) {
  if (object.contains(key) && !object.at(key).is_boolean()) invalid(std::string(key) + " is not of type 'boolean'");
}

// ---- Python-like conversions for ContextObservation.from_mapping ----
double py_float(const json& value, const char* name) {
  if (value.is_number()) return value.get<double>();
  if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
  if (value.is_string()) {
    const std::string text = value.get<std::string>();
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (!text.empty() && end != nullptr && *end == '\0') return parsed;
  }
  throw std::invalid_argument(std::string(name) + ": could not convert to float");
}

std::string py_text(const json& value) {
  if (value.is_null()) return "None";
  return py_str(value);
}

const json& get_or(const json& object, const char* key, const json& fallback) {
  auto it = object.find(key);
  return it == object.end() ? fallback : *it;
}

void collect_keys(const json& node, std::set<std::string>& out) {
  if (node.is_object()) {
    for (const auto& [key, child] : node.items()) {
      out.insert(key);
      collect_keys(child, out);
    }
  } else if (node.is_array()) {
    for (const auto& child : node) collect_keys(child, out);
  }
}

Micros now_micros() {
  using namespace std::chrono;
  return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

std::optional<bool> optional_bool(const json& value, const char* name) {
  if (value.is_null()) return std::nullopt;
  if (!value.is_boolean()) throw std::invalid_argument(std::string(name) + " must be boolean or null");
  return value.get<bool>();
}

double probability(const char* name, double value) {
  if (!(value >= 0.0 && value <= 1.0)) throw std::invalid_argument(std::string(name) + " must be between 0 and 1");
  return value;
}

}  // namespace

bool is_quiet_required_context(std::string_view context_type) {
  return context_type == "ZOOM_SPEAKER" || context_type == "ZOOM_HEADPHONES" || context_type == "IN_PERSON_MEETING";
}

// ---------------------------------------------------------------------------------------------- validation
void validate_feature_event(const json& event) {
  require_object(event, "feature event");
  check_keys(event,
             {"schema_version", "event_id", "trace_id", "timestamp", "valid_until", "source", "source_node_id",
              "event_type", "simulated", "payload"},
             {"schema_version", "event_id", "trace_id", "timestamp", "valid_until", "source", "source_node_id",
              "event_type", "simulated", "payload"},
             "feature event");
  if (event.at("schema_version") != json("1.0")) invalid("schema_version: '1.0' was expected");
  check_string(event, "event_id", true);
  check_string(event, "trace_id", true);
  check_string(event, "timestamp", false);
  check_string(event, "valid_until", false);
  check_enum(event, "source",
             {"pi5.audio-context", "pi5.turn-taking", "pi5.vision-presence", "pi4.device-state", "pc.scenario"});
  check_enum(event, "source_node_id", {"NODE-RPI5-02", "NODE-RPI4-01", "PC-SIMULATION"});
  check_enum(event, "event_type", {"AUDIO_CONTEXT", "TURN_TAKING", "PRESENCE", "DEVICE_STATE"});
  check_boolean(event, "simulated");
  require_object(event.at("payload"), "payload");

  const std::string source = event.at("source").get<std::string>();
  if (source == "pc.scenario") {
    check_enum(event, "source_node_id", {"PC-SIMULATION"});
    if (event.at("simulated") != json(true)) invalid("simulated: True was expected");
  } else {
    check_enum(event, "source_node_id", {"NODE-RPI5-02", "NODE-RPI4-01"});
  }
  const std::string type = event.at("event_type").get<std::string>();
  const json& payload = event.at("payload");
  if (type == "AUDIO_CONTEXT") {
    check_enum(event, "source", {"pi5.audio-context", "pc.scenario"});
    check_keys(payload,
               {"context_type", "confidence", "speech_ratio", "duration_seconds", "model_version",
                "class_probabilities"},
               {"context_type", "confidence", "speech_ratio", "duration_seconds", "model_version"}, "payload");
    check_enum(payload, "context_type",
               {"QUIET", "SELF_TALK", "TV_MEDIA", "ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING", "UNKNOWN"});
    check_number(payload, "confidence", 0.0, 1.0);
    check_number(payload, "speech_ratio", 0.0, 1.0);
    check_number(payload, "duration_seconds", 0.0, std::nullopt);
    check_string(payload, "model_version", true);
    if (payload.contains("class_probabilities")) {
      const json& probabilities = payload.at("class_probabilities");
      require_object(probabilities, "class_probabilities");
      check_keys(probabilities, {"SELF_TALK", "TV_MEDIA", "ZOOM_SPEAKER", "IN_PERSON_MEETING", "UNKNOWN"}, {},
                 "class_probabilities");
      for (const auto& [label, unused] : probabilities.items()) {
        (void)unused;
        check_number(probabilities, label.c_str(), 0.0, 1.0);
      }
    }
  } else if (type == "TURN_TAKING") {
    check_enum(event, "source", {"pi5.turn-taking", "pc.scenario"});
    check_keys(payload, {"turns_per_minute", "confidence", "method"}, {"turns_per_minute", "confidence", "method"},
               "payload");
    check_number(payload, "turns_per_minute", 0.0, std::nullopt);
    check_number(payload, "confidence", 0.0, 1.0);
    check_enum(payload, "method", {"SPEAKER_CHANGE", "SPEECH_BURST_PROXY"});
  } else if (type == "PRESENCE") {
    check_enum(event, "source", {"pi5.vision-presence", "pc.scenario"});
    check_keys(payload, {"presence_zone", "confidence"}, {"presence_zone", "confidence"}, "payload");
    check_enum(payload, "presence_zone", {"BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"});
    check_number(payload, "confidence", 0.0, 1.0);
  } else if (type == "DEVICE_STATE") {
    check_enum(event, "source", {"pi4.device-state", "pc.scenario"});
    check_keys(payload, {"tv_on", "laptop_active", "headphones_connected"},
               {"tv_on", "laptop_active", "headphones_connected"}, "payload");
    check_boolean(payload, "tv_on");
    check_boolean(payload, "laptop_active");
    check_boolean(payload, "headphones_connected");
  }
}

void validate_context_event(const json& event) {
  require_object(event, "context event");
  check_keys(event,
             {"schema_version", "event_id", "trace_id", "timestamp", "source", "source_node_id", "event_type",
              "context_type", "presence_zone", "activity", "quiet_required", "confidence", "evidence", "transition",
              "valid_until", "model_version"},
             {"schema_version", "event_id", "trace_id", "timestamp", "source", "source_node_id", "event_type",
              "context_type", "presence_zone", "activity", "quiet_required", "confidence", "evidence", "transition",
              "valid_until", "model_version"},
             "context event");
  if (event.at("schema_version") != json("1.0")) invalid("schema_version: '1.0' was expected");
  check_string(event, "event_id", true);
  check_string(event, "trace_id", true);
  check_string(event, "timestamp", false);
  check_string(event, "valid_until", false);
  check_enum(event, "source", {"pi5.context-fusion"});
  check_enum(event, "source_node_id", {"NODE-RPI5-02"});
  check_enum(event, "event_type", {"ACTIVITY_CONTEXT"});
  check_enum(event, "context_type",
             {"QUIET", "SELF_TALK", "TV_MEDIA", "ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING", "UNKNOWN"});
  check_enum(event, "presence_zone", {"BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"});
  check_enum(event, "activity",
             {"RESTING", "SLEEPING", "STUDYING", "WORKING", "MEETING", "BATHROOM_USE", "ENTERING", "LEAVING",
              "GENERAL_ACTIVITY", "UNKNOWN"});
  check_boolean(event, "quiet_required");
  check_number(event, "confidence", 0.0, 1.0);
  const json& evidence = event.at("evidence");
  if (!evidence.is_array() || evidence.empty()) invalid("evidence must be a non-empty array");
  std::set<std::string> seen;
  for (const json& item : evidence) {
    if (!item.is_string() || item.get<std::string>().empty()) invalid("evidence items must be non-empty strings");
    if (!seen.insert(item.get<std::string>()).second) invalid("evidence has non-unique elements");
  }
  check_enum(event, "transition", {"ACTIVATED", "UPDATED", "DEACTIVATED", "UNCHANGED"});
  check_string(event, "model_version", true);
}

// ---------------------------------------------------------------------------------------------- observation
void ContextObservation::validate() const {
  if (!contains(kContextTypes, audio_context))
    throw std::invalid_argument("unsupported audio_context: " + audio_context);
  if (!contains(kPresenceZones, presence_zone))
    throw std::invalid_argument("unsupported presence_zone: " + presence_zone);
  if (!contains(kTurnTakingMethods, turn_taking_method))
    throw std::invalid_argument("unsupported turn_taking_method: " + turn_taking_method);
  probability("audio_confidence", audio_confidence);
  if (audio_class_probabilities) {
    for (const auto& [label, value] : *audio_class_probabilities) {
      if (!contains(kContextTypes, label)) throw std::invalid_argument("unsupported audio probability labels");
      probability("audio_class_probabilities", value);
    }
  }
  probability("speech_ratio", speech_ratio);
  probability("presence_confidence", presence_confidence);
  if (turns_per_minute && *turns_per_minute < 0) throw std::invalid_argument("turns_per_minute cannot be negative");
  if (duration_seconds < 0) throw std::invalid_argument("duration_seconds cannot be negative");
}

ContextObservation ContextObservation::from_mapping(const json& value) {
  if (!value.is_object()) throw std::invalid_argument("observation must be an object");
  static const char* forbidden[] = {"raw_audio", "audio_bytes", "image",       "image_bytes",
                                    "frame",     "transcript",  "coordinates", "identity"};
  std::set<std::string> keys;
  collect_keys(value, keys);
  for (const char* name : forbidden)
    if (keys.count(name)) throw std::invalid_argument("raw or identifying input is forbidden");
  if (value.contains("source_direction") ||
      (value.contains("features") && value.at("features").is_object() &&
       value.at("features").contains("source_direction")))
    throw std::invalid_argument("source_direction was removed: the single USB microphone has no direction capability");
  static const json empty = json::object();
  static const json null_value = nullptr;
  const json& audio = get_or(value, "audio", empty);
  const json& vision = get_or(value, "vision", empty);
  const json& features = get_or(value, "features", empty);
  const json& devices = get_or(value, "device_state", empty);
  if (!audio.is_object() || !vision.is_object() || !features.is_object() || !devices.is_object())
    throw std::invalid_argument("audio, vision, features, and device_state must be objects");
  const json default_unknown = "UNKNOWN";
  const json zero = 0.0;

  ContextObservation o;
  o.audio_context = py_text(get_or(value, "audio_context", get_or(audio, "context_type", default_unknown)));
  o.audio_confidence = py_float(get_or(value, "audio_confidence", get_or(audio, "confidence", zero)), "audio_confidence");
  const json& probabilities = get_or(value, "audio_class_probabilities", get_or(audio, "class_probabilities", null_value));
  if (!probabilities.is_null()) {
    if (!probabilities.is_object()) throw std::invalid_argument("audio_class_probabilities must be an object");
    std::map<std::string, double> parsed;
    for (const auto& [label, item] : probabilities.items()) parsed[label] = py_float(item, "audio_class_probabilities");
    o.audio_class_probabilities = parsed;
  }
  o.speech_ratio = py_float(get_or(value, "speech_ratio", get_or(features, "speech_ratio", zero)), "speech_ratio");
  const json& turns = get_or(value, "turns_per_minute", get_or(features, "turns_per_minute", null_value));
  if (!turns.is_null()) o.turns_per_minute = py_float(turns, "turns_per_minute");
  o.turn_taking_method =
      py_text(get_or(value, "turn_taking_method", get_or(features, "turn_taking_method", default_unknown)));
  o.duration_seconds =
      py_float(get_or(value, "duration_seconds", get_or(features, "duration_seconds", zero)), "duration_seconds");
  o.presence_zone = py_text(
      get_or(value, "presence_zone", get_or(vision, "presence_zone", get_or(vision, "zone", default_unknown))));
  o.presence_confidence =
      py_float(get_or(value, "presence_confidence", get_or(vision, "confidence", zero)), "presence_confidence");
  o.tv_on = optional_bool(get_or(value, "tv_on", get_or(devices, "tv_on", null_value)), "tv_on");
  o.laptop_active =
      optional_bool(get_or(value, "laptop_active", get_or(devices, "laptop_active", null_value)), "laptop_active");
  o.headphones_connected = optional_bool(
      get_or(value, "headphones_connected", get_or(devices, "headphones_connected", null_value)), "headphones_connected");
  const json& model_version = get_or(value, "audio_model_version", get_or(audio, "model_version", null_value));
  if (!model_version.is_null()) o.audio_model_version = py_text(model_version);
  const json& trace = get_or(value, "trace_id", get_or(audio, "trace_id", null_value));
  if (!trace.is_null()) o.trace_id = py_text(trace);
  const json& timestamp = get_or(value, "timestamp", get_or(audio, "timestamp", null_value));
  o.timestamp = timestamp.is_null() ? now_micros() : parse_iso(py_text(timestamp));
  o.validate();
  return o;
}

// ---------------------------------------------------------------------------------------------- scoring
Classification classify(const ContextObservation& observation, double quiet_audio_confidence,
                        double meeting_tv_margin) {
  if (!(quiet_audio_confidence >= 0.0 && quiet_audio_confidence <= 1.0))
    throw std::invalid_argument("quiet_audio_confidence must be between 0 and 1");
  if (!(meeting_tv_margin >= 0.0 && meeting_tv_margin <= 1.0))
    throw std::invalid_argument("meeting_tv_margin must be between 0 and 1");
  std::array<double, kClassifiable> scores;
  scores.fill(0.04);
  std::array<std::vector<std::string>, kClassifiable> evidence;
  auto index = [](std::string_view label) -> std::size_t {
    for (std::size_t i = 0; i < kClassifiable; ++i)
      if (label == kContextTypes[i]) return i;
    return kClassifiable;
  };
  auto add = [&](const char* label, double amount, const std::string& reason) {
    const std::size_t i = index(label);
    scores[i] += amount;
    evidence[i].push_back(reason);
  };
  const std::size_t QUIET = 0;
  const ContextObservation& o = observation;

  static const std::map<std::string, double> no_probabilities;
  const auto& probabilities = o.audio_class_probabilities ? *o.audio_class_probabilities : no_probabilities;
  auto probability_of = [&](const char* label) {
    auto it = probabilities.find(label);
    return it == probabilities.end() ? 0.0 : it->second;
  };
  const double meeting_probability = probability_of("IN_PERSON_MEETING");
  const double tv_probability = probability_of("TV_MEDIA");
  const bool meeting_tv_ambiguous = o.audio_context == "IN_PERSON_MEETING" && probabilities.count("IN_PERSON_MEETING") &&
                                    probabilities.count("TV_MEDIA") &&
                                    meeting_probability - tv_probability < meeting_tv_margin;
  const bool quiet_audio_suppressed =
      meeting_tv_ambiguous ||
      (is_quiet_required_context(o.audio_context) && o.audio_confidence < quiet_audio_confidence);
  const std::string suppression_reason =
      meeting_tv_ambiguous ? "guard:meeting_tv_margin:" + py_fixed(meeting_probability - tv_probability, 2) + "<" +
                                 py_fixed(meeting_tv_margin, 2)
                           : "guard:quiet_audio_below_threshold:" + py_fixed(o.audio_confidence, 2) + "<" +
                                 py_fixed(quiet_audio_confidence, 2);
  const std::size_t audio_index = index(o.audio_context);
  if (audio_index < kClassifiable && !quiet_audio_suppressed) {
    add(kContextTypes[audio_index], 0.72 * o.audio_confidence,
        "audio:" + o.audio_context + ":" + py_fixed(o.audio_confidence, 2));
    if (o.audio_model_version && !o.audio_model_version->empty())
      evidence[audio_index].push_back("audio_model:" + *o.audio_model_version);
  } else if (quiet_audio_suppressed) {
    evidence[QUIET].push_back(suppression_reason);
    if (meeting_tv_ambiguous) add("TV_MEDIA", 0.72 * tv_probability, suppression_reason);
  }

  static const char* speech_labels[] = {"SELF_TALK", "ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING"};
  if (o.speech_ratio < 0.08) {
    add("QUIET", 0.48, "speech_ratio:low:" + py_fixed(o.speech_ratio, 2));
  } else if (o.speech_ratio >= 0.18) {
    for (const char* label : speech_labels) add(label, 0.08, "speech_ratio:active:" + py_fixed(o.speech_ratio, 2));
  }

  if (o.turns_per_minute && *o.turns_per_minute >= 3.0) {
    const bool proxy = o.turn_taking_method == "SPEECH_BURST_PROXY";
    const std::string turns = py_fixed(*o.turns_per_minute, 1);
    add("IN_PERSON_MEETING", proxy ? 0.10 : 0.27, std::string("turn_taking:") + (proxy ? "proxy" : "high") + ":" + turns);
    add("ZOOM_SPEAKER", proxy ? 0.06 : 0.15, std::string("turn_taking:") + (proxy ? "proxy" : "present") + ":" + turns);
  } else if (o.turns_per_minute && *o.turns_per_minute < 1.5 && o.speech_ratio >= 0.12) {
    const std::string turns = py_fixed(*o.turns_per_minute, 1);
    add("SELF_TALK", 0.22, "turn_taking:low:" + turns);
    add("TV_MEDIA", 0.10, "turn_taking:low:" + turns);
  }

  if (o.duration_seconds >= 20.0) {
    for (const char* label : {"TV_MEDIA", "ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING"})
      add(label, 0.08, "duration:sustained:" + py_fixed(o.duration_seconds, 0) + "s");
  }

  if ((o.presence_zone == "DESK" || o.presence_zone == "COMMON") && o.presence_confidence >= 0.4) {
    const std::string reason = "presence:" + lower(o.presence_zone);
    add("IN_PERSON_MEETING", 0.10, reason);
    add("ZOOM_SPEAKER", 0.09, reason);
    add("ZOOM_HEADPHONES", 0.09, reason);
  }

  if (o.tv_on == true) add("TV_MEDIA", 0.38, "device:tv_on");
  if (o.laptop_active == true) {
    add("ZOOM_SPEAKER", 0.17, "device:laptop_active");
    add("ZOOM_HEADPHONES", 0.17, "device:laptop_active");
  }
  if (o.headphones_connected == true) add("ZOOM_HEADPHONES", 0.36, "device:headphones_connected");

  if (o.speech_ratio < 0.08) {
    for (const char* label : speech_labels) scores[index(label)] *= 0.20;
    evidence[QUIET].push_back("guard:no_speech_context_suppressed");
  }
  if (o.tv_on == true) {
    for (const char* label : {"ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING"}) scores[index(label)] *= 0.35;
    evidence[index("TV_MEDIA")].push_back("guard:tv_meeting_suppressed");
  }

  // sorted(scores, key=scores.get, reverse=True) is stable.
  std::array<std::size_t, kClassifiable> ranking{0, 1, 2, 3, 4, 5};
  std::stable_sort(ranking.begin(), ranking.end(), [&](std::size_t a, std::size_t b) { return scores[a] > scores[b]; });
  const std::size_t winner = ranking[0], runner_up = ranking[1];
  const double top = scores[winner], second = scores[runner_up];
  const double margin = top - second;

  Classification result;
  for (std::size_t i = 0; i < kClassifiable; ++i) result.scores.emplace_back(kContextTypes[i], py_round(scores[i], 4));
  if (top < 0.22 || margin < 0.025) {
    result.context_type = "UNKNOWN";
    result.confidence = py_round(std::max(0.25, std::min(0.60, 0.35 + margin)), 4);
    if (quiet_audio_suppressed) result.evidence.push_back(suppression_reason);
    result.evidence.push_back(std::string("uncertain:") + kContextTypes[winner] + ":" + py_fixed(top, 3));
    result.evidence.push_back("margin:" + py_fixed(margin, 3));
    return result;
  }
  const double confidence = std::min(0.99, 0.42 + 0.32 * std::min(top, 1.0) + 0.34 * std::min(margin, 1.0));
  std::vector<std::string> reasons = evidence[winner];
  if (reasons.empty()) reasons.push_back(std::string("score:") + kContextTypes[winner] + ":" + py_fixed(top, 3));
  reasons.push_back("margin:" + py_fixed(margin, 3));
  result.context_type = kContextTypes[winner];
  result.confidence = py_round(confidence, 4);
  result.evidence = unique_in_order(reasons);
  return result;
}

// ---------------------------------------------------------------------------------------------- engine
ContextFusionEngine::ContextFusionEngine(json activation_updates, json release_updates, json change_updates,
                                         double ttl_seconds, double quiet_audio_confidence, double meeting_tv_margin)
    : activation_updates_(std::move(activation_updates)),
      release_updates_(std::move(release_updates)),
      change_updates_(std::move(change_updates)),
      ttl_seconds_(ttl_seconds),
      quiet_audio_confidence_(quiet_audio_confidence),
      meeting_tv_margin_(meeting_tv_margin) {
  const double lowest = std::min({number(activation_updates_, "activation_updates"),
                                  number(release_updates_, "release_updates"), number(change_updates_, "change_updates")});
  if (lowest < 1) throw std::invalid_argument("hysteresis update counts must be positive");
  if (!(ttl_seconds_ > 0)) throw std::invalid_argument("ttl_seconds must be positive");
  if (!(quiet_audio_confidence_ >= 0.0 && quiet_audio_confidence_ <= 1.0))
    throw std::invalid_argument("quiet_audio_confidence must be between 0 and 1");
  if (!(meeting_tv_margin_ >= 0.0 && meeting_tv_margin_ <= 1.0))
    throw std::invalid_argument("meeting_tv_margin must be between 0 and 1");
}

const json& ContextFusionEngine::required_updates(bool predicted_quiet) const {
  if (predicted_quiet && !current_quiet_required_) return activation_updates_;
  if (current_quiet_required_ && !predicted_quiet) return release_updates_;
  return change_updates_;
}

json ContextFusionEngine::update(const ContextObservation& observation) {
  if (last_timestamp_ && observation.timestamp < *last_timestamp_)
    throw std::invalid_argument("observations must be ordered by timestamp");
  // timedelta.total_seconds() is an exactly rounded int / 10**6 division.
  const bool expired =
      last_timestamp_ && static_cast<double>(observation.timestamp - *last_timestamp_) / 1e6 > ttl_seconds_;
  if (expired) {
    current_context_ = "UNKNOWN";
    current_quiet_required_ = false;
    current_confidence_ = 0.25;
    current_evidence_ = {"state:expired"};
    candidate_ = {"UNKNOWN", false};
    candidate_count_ = 0;
  }

  const Classification result = classify(observation, quiet_audio_confidence_, meeting_tv_margin_);
  const std::string predicted_activity = activity_for(result.context_type, observation.presence_zone);
  const bool predicted_quiet =
      is_quiet_required_context(result.context_type) ||
      (result.context_type == "QUIET" && (predicted_activity == "SLEEPING" || predicted_activity == "STUDYING") &&
       observation.presence_confidence >= 0.5);
  const std::pair<std::string, bool> predicted{result.context_type, predicted_quiet};
  const std::pair<std::string, bool> current{current_context_, current_quiet_required_};
  const bool previous_quiet = current_quiet_required_;
  const json& required = required_updates(predicted_quiet);

  if (predicted == current) {
    candidate_ = predicted;
    candidate_count_ = 0;
  } else if (predicted != candidate_) {
    candidate_ = predicted;
    candidate_count_ = 1;
  } else {
    candidate_count_ += 1;
  }
  const bool changed = candidate_ != current && static_cast<double>(candidate_count_) >= required.get<double>();
  if (changed) {
    current_context_ = candidate_.first;
    current_quiet_required_ = candidate_.second;
    candidate_count_ = 0;
  }
  const std::pair<std::string, bool> now_state{current_context_, current_quiet_required_};
  if (changed || predicted == now_state) {
    current_confidence_ = result.confidence;
    current_evidence_ = result.evidence;
    if (predicted_quiet && result.context_type == "QUIET")
      current_evidence_.push_back("policy:" + lower(predicted_activity) + "_requires_quiet");
  }

  const bool now_quiet = current_quiet_required_;
  std::string transition = "UNCHANGED";
  if (changed && now_quiet && !previous_quiet) transition = "ACTIVATED";
  else if (changed && previous_quiet && !now_quiet) transition = "DEACTIVATED";
  else if (changed) transition = "UPDATED";

  std::vector<std::string> evidence = current_evidence_;
  if (now_state != predicted) {
    evidence.push_back("candidate:" + result.context_type + ":" + std::to_string(candidate_count_) + "/" +
                       py_str(required));
    evidence.insert(evidence.end(), result.evidence.begin(), result.evidence.end());
  }
  if (expired) evidence.insert(evidence.begin(), "state:expired");

  const std::string trace_id =
      observation.trace_id && !observation.trace_id->empty() ? *observation.trace_id : "trace-" + new_id();
  json event = {
      {"schema_version", "1.0"},
      {"event_id", "ctx-" + new_id()},
      {"trace_id", trace_id},
      {"timestamp", iso_millis_z(observation.timestamp)},
      {"source", "pi5.context-fusion"},
      {"source_node_id", "NODE-RPI5-02"},
      {"event_type", "ACTIVITY_CONTEXT"},
      {"context_type", current_context_},
      {"presence_zone", observation.presence_zone},
      {"activity", activity_for(current_context_, observation.presence_zone)},
      {"quiet_required", now_quiet},
      {"confidence", current_confidence_},
      {"evidence", unique_in_order(evidence)},
      {"transition", transition},
      {"valid_until", iso_millis_z(observation.timestamp + seconds_to_micros(ttl_seconds_))},
      {"model_version", kFusionModelVersion},
  };
  last_timestamp_ = observation.timestamp;
  return event;
}

json ContextFusionEngine::sanitized_context(const json& event) {
  return {{"presence_zone", event.at("presence_zone")},
          {"activity", event.at("activity")},
          {"audio_context", event.at("context_type")},
          {"quiet_required", event.at("quiet_required")},
          {"confidence", event.at("confidence")}};
}

// ---------------------------------------------------------------------------------------------- policy
ContextFusionPolicy ContextFusionPolicy::from_mapping(const json& value) {
  if (!value.is_object()) throw std::invalid_argument("policy config must be a JSON object");
  const json& settings = value.contains("engine") ? value.at("engine") : value;
  if (!settings.is_object()) throw std::invalid_argument("policy config engine must be an object");
  static const char* allowed[] = {"quiet_audio_confidence", "meeting_tv_margin", "activation_updates",
                                  "release_updates",        "change_updates",    "ttl_seconds"};
  std::vector<std::string> unknown;
  for (const auto& [key, unused] : settings.items()) {
    (void)unused;
    if (std::none_of(std::begin(allowed), std::end(allowed), [&](const char* name) { return key == name; }))
      unknown.push_back(key);
  }
  if (!unknown.empty()) {
    std::string names;
    for (const auto& name : unknown) names += (names.empty() ? "'" : ", '") + name + "'";
    throw std::invalid_argument("unknown policy config fields: [" + names + "]");
  }
  ContextFusionPolicy policy;
  if (settings.contains("quiet_audio_confidence"))
    policy.quiet_audio_confidence = number(settings.at("quiet_audio_confidence"), "quiet_audio_confidence");
  if (settings.contains("meeting_tv_margin"))
    policy.meeting_tv_margin = number(settings.at("meeting_tv_margin"), "meeting_tv_margin");
  for (auto [name, field] : {std::pair{"activation_updates", &policy.activation_updates},
                             std::pair{"release_updates", &policy.release_updates},
                             std::pair{"change_updates", &policy.change_updates}}) {
    if (!settings.contains(name)) continue;
    number(settings.at(name), name);
    *field = settings.at(name);
  }
  if (settings.contains("ttl_seconds")) policy.ttl_seconds = number(settings.at("ttl_seconds"), "ttl_seconds");
  if (!(policy.quiet_audio_confidence >= 0.0 && policy.quiet_audio_confidence <= 1.0))
    throw std::invalid_argument("quiet_audio_confidence must be between 0 and 1");
  if (!(policy.meeting_tv_margin >= 0.0 && policy.meeting_tv_margin <= 1.0))
    throw std::invalid_argument("meeting_tv_margin must be between 0 and 1");
  if (std::min({policy.activation_updates.get<double>(), policy.release_updates.get<double>(),
                policy.change_updates.get<double>()}) < 1)
    throw std::invalid_argument("policy update counts must be positive");
  if (!(policy.ttl_seconds > 0)) throw std::invalid_argument("ttl_seconds must be positive");
  return policy;
}

ContextFusionPolicy ContextFusionPolicy::from_json(const std::filesystem::path& path, bool require_accepted) {
  const json value = read_json_file(path);
  if (!value.is_object()) throw std::invalid_argument("policy config must be a JSON object");
  if (require_accepted && value.contains("acceptance") && value.at("acceptance").is_object()) {
    const json& acceptance = value.at("acceptance");
    std::string failed;
    for (const char* name : {"calibration_passed", "holdout_passed"}) {
      if (acceptance.contains(name) && acceptance.at(name) == json(true)) continue;
      failed += (failed.empty() ? "" : ", ") + std::string(name);
    }
    if (!failed.empty()) throw std::invalid_argument("policy config is not accepted for runtime: " + failed);
  }
  return from_mapping(value);
}

ContextFusionEngine ContextFusionPolicy::engine() const {
  return ContextFusionEngine(activation_updates, release_updates, change_updates, ttl_seconds, quiet_audio_confidence,
                             meeting_tv_margin);
}

json ContextFusionPolicy::as_dict() const {
  return {{"quiet_audio_confidence", quiet_audio_confidence}, {"meeting_tv_margin", meeting_tv_margin},
          {"activation_updates", activation_updates},         {"release_updates", release_updates},
          {"change_updates", change_updates},                 {"ttl_seconds", ttl_seconds}};
}

// ---------------------------------------------------------------------------------------------- stream fusion
json StreamIngestResult::debug_record() const {
  json value = {{"accepted", accepted}, {"reason", reason}, {"event_type", event_type}};
  if (input_status) {
    json status = json::object();
    for (const auto& [name, state] : *input_status) status[name] = state;
    value["input_status"] = status;
  }
  if (context_event) value["context_event"] = *context_event;
  return value;
}

FeatureStreamFusion::FeatureStreamFusion(ContextFusionEngine engine, double minimum_turn_confidence,
                                         double minimum_presence_confidence, std::size_t duplicate_window)
    : engine_(std::move(engine)),
      minimum_turn_confidence_(minimum_turn_confidence),
      minimum_presence_confidence_(minimum_presence_confidence),
      duplicate_window_(duplicate_window) {
  for (double value : {minimum_turn_confidence, minimum_presence_confidence})
    if (!(value >= 0.0 && value <= 1.0))
      throw std::invalid_argument("minimum confidence values must be between 0 and 1");
  if (duplicate_window < 1) throw std::invalid_argument("duplicate_window must be positive");
}

void FeatureStreamFusion::remember(const std::string& event_id) {
  seen_ids_.insert(event_id);
  seen_order_.push_back(event_id);
  while (seen_order_.size() > duplicate_window_) {
    seen_ids_.erase(seen_order_.front());
    seen_order_.pop_front();
  }
}

StreamIngestResult FeatureStreamFusion::ingest(const json& event) {
  validate_feature_event(event);
  const std::string event_type = event.at("event_type").get<std::string>();
  const std::string event_id = event.at("event_id").get<std::string>();
  const Micros timestamp = parse_iso(event.at("timestamp").get<std::string>());
  const Micros valid_until = parse_iso(event.at("valid_until").get<std::string>());
  if (valid_until <= timestamp) throw std::invalid_argument("valid_until must be later than timestamp");
  if (seen_ids_.count(event_id)) return {false, "duplicate", event_type, std::nullopt, std::nullopt};

  auto previous = latest_.find(event_type);
  if (previous != latest_.end() && timestamp < previous->second.timestamp) {
    remember(event_id);
    return {false, "out_of_order", event_type, std::nullopt, std::nullopt};
  }
  remember(event_id);
  latest_[event_type] = Latest{event, timestamp, valid_until};
  if (event_type != "AUDIO_CONTEXT") return {true, "buffered", event_type, std::nullopt, std::nullopt};

  std::vector<std::pair<std::string, std::string>> input_status{{"AUDIO_CONTEXT", "fresh"}};
  std::map<std::string, const json*> auxiliary;
  for (const char* name : kAuxiliaryEventTypes) {
    const json* fresh = nullptr;
    std::string status = "missing";
    auto it = latest_.find(name);
    if (it != latest_.end()) {
      if (it->second.timestamp > timestamp) status = "future";
      else if (it->second.valid_until < timestamp) status = "stale";
      else {
        status = "fresh";
        fresh = &it->second.event;
      }
    }
    auxiliary[name] = fresh;
    input_status.emplace_back(name, status);
  }
  auto set_status = [&](const char* name, const char* status) {
    for (auto& item : input_status)
      if (item.first == name) item.second = status;
  };

  static const json empty = json::object();
  const json* turn_event = auxiliary["TURN_TAKING"];
  const json& turn_payload = turn_event ? turn_event->at("payload") : empty;
  std::optional<double> turns_per_minute;
  if (turn_event && turn_payload.at("confidence").get<double>() < minimum_turn_confidence_) {
    set_status("TURN_TAKING", "low_confidence");
  } else if (turn_event) {
    turns_per_minute = turn_payload.at("turns_per_minute").get<double>();
  }

  const json* presence_event = auxiliary["PRESENCE"];
  const json& presence_payload = presence_event ? presence_event->at("payload") : empty;
  const double presence_confidence = presence_event ? presence_payload.at("confidence").get<double>() : 0.0;
  std::string presence_zone = "UNKNOWN";
  if (presence_event && presence_confidence < minimum_presence_confidence_) {
    set_status("PRESENCE", "low_confidence");
  } else if (presence_event) {
    presence_zone = presence_payload.at("presence_zone").get<std::string>();
  }

  const json* device_event = auxiliary["DEVICE_STATE"];
  const json& device_payload = device_event ? device_event->at("payload") : empty;
  auto device_flag = [&](const char* name) -> std::optional<bool> {
    if (!device_payload.contains(name) || device_payload.at(name).is_null()) return std::nullopt;
    return device_payload.at(name).get<bool>();
  };

  const json& audio_payload = event.at("payload");
  ContextObservation observation;
  observation.audio_context = audio_payload.at("context_type").get<std::string>();
  observation.audio_confidence = audio_payload.at("confidence").get<double>();
  if (audio_payload.contains("class_probabilities") && !audio_payload.at("class_probabilities").is_null()) {
    std::map<std::string, double> probabilities;
    for (const auto& [label, value] : audio_payload.at("class_probabilities").items())
      probabilities[label] = value.get<double>();
    observation.audio_class_probabilities = probabilities;
  }
  observation.speech_ratio = audio_payload.at("speech_ratio").get<double>();
  observation.turns_per_minute = turns_per_minute;
  observation.turn_taking_method = turn_event ? turn_payload.at("method").get<std::string>() : "UNKNOWN";
  observation.duration_seconds = audio_payload.at("duration_seconds").get<double>();
  observation.presence_zone = presence_zone;
  observation.presence_confidence = presence_zone != "UNKNOWN" ? presence_confidence : 0.0;
  observation.tv_on = device_flag("tv_on");
  observation.laptop_active = device_flag("laptop_active");
  observation.headphones_connected = device_flag("headphones_connected");
  observation.audio_model_version = audio_payload.at("model_version").get<std::string>();
  observation.trace_id = event.at("trace_id").get<std::string>();
  observation.timestamp = timestamp;
  observation.validate();

  json context_event = engine_.update(observation);
  std::vector<std::string> evidence = context_event.at("evidence").get<std::vector<std::string>>();
  for (const auto& [name, status] : input_status) evidence.push_back("input:" + lower(name) + ":" + status);
  evidence = unique_in_order(evidence);
  std::string simulated;
  for (const char* name : {"AUDIO_CONTEXT", "TURN_TAKING", "PRESENCE", "DEVICE_STATE"}) {
    const json* item = std::string_view(name) == "AUDIO_CONTEXT" ? &event : auxiliary[name];
    if (item && !item->empty() && item->at("simulated").get<bool>())
      simulated += (simulated.empty() ? "" : ",") + lower(name);
  }
  if (!simulated.empty()) evidence.push_back("input:simulated:" + simulated);
  context_event["evidence"] = evidence;
  validate_context_event(context_event);
  return {true, "fused", event_type, context_event, input_status};
}

}  // namespace hm::ctx
