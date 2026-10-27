#include "hm/pfn_schema.hpp"

#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>

#include "hm/pyregex.hpp"
#include "hm/text.hpp"

namespace hm::guard {

namespace {

using Names = std::initializer_list<std::string_view>;

// One schema walk; the first failure is kept.
class Check {
 public:
  bool ok() const { return problem_.empty(); }
  const std::string& problem() const { return problem_; }

  bool fail(const std::string& path, const std::string& what) {
    if (problem_.empty()) problem_ = path + ": " + what;
    return false;
  }

  bool object(const json& value, const std::string& path, Names required, Names allowed) {
    if (!value.is_object()) return fail(path, "is not of type 'object'");
    for (std::string_view name : required) {
      if (!value.contains(std::string(name))) return fail(path, "'" + std::string(name) + "' is a required property");
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
      bool known = false;
      for (std::string_view name : allowed) known |= it.key() == name;
      if (!known) return fail(path, "additional property '" + it.key() + "'");
    }
    return true;
  }

  bool string(const json& value, const std::string& path, std::size_t min_length, std::size_t max_length) {
    if (!value.is_string()) return fail(path, "is not of type 'string'");
    const std::size_t length = text::length(value.get_ref<const std::string&>());
    if (length < min_length) return fail(path, "is too short");
    if (length > max_length) return fail(path, "is too long");
    return true;
  }

  bool optional_string(const json& value, const std::string& path, std::size_t max_length) {
    return value.is_null() || string(value, path, 0, max_length);
  }

  bool uuid(const json& value, const std::string& path) {
    if (!value.is_string()) return fail(path, "is not of type 'string'");
    return jsonschema_uuid(value.get<std::string>()) || fail(path, "is not a 'uuid'");
  }

  bool constant(const json& value, const std::string& path, std::string_view expected) {
    return (value.is_string() && value.get_ref<const std::string&>() == expected) ||
           fail(path, "must be '" + std::string(expected) + "'");
  }

  bool one_of(const json& value, const std::string& path, Names options, bool allow_null = false) {
    if (allow_null && value.is_null()) return true;
    if (value.is_string()) {
      for (std::string_view option : options) {
        if (value.get_ref<const std::string&>() == option) return true;
      }
    }
    return fail(path, "is not one of the allowed values");
  }

  // "integer": int, or a float with an integral value (jsonschema accepts 5.0).
  bool integer(const json& value, const std::string& path, double minimum) {
    const bool is_integer = value.is_number_integer() ||
                            (value.is_number_float() && std::isfinite(value.get<double>()) &&
                             std::floor(value.get<double>()) == value.get<double>());
    if (!is_integer) return fail(path, "is not of type 'integer'");
    return value.get<double>() >= minimum || fail(path, "is less than the minimum");
  }

  bool number(const json& value, const std::string& path, double minimum, double maximum) {
    if (!value.is_number()) return fail(path, "is not of type 'number'");
    const double number = value.get<double>();
    if (number < minimum) return fail(path, "is less than the minimum");
    if (number > maximum) return fail(path, "is greater than the maximum");
    return true;
  }

  bool boolean(const json& value, const std::string& path) {
    return value.is_boolean() || fail(path, "is not of type 'boolean'");
  }

 private:
  std::string problem_;
};

constexpr double kInf = std::numeric_limits<double>::infinity();

// int(text, 16) as Python parses it (after uuid.UUID removed '-', '{', '}', 'urn:', 'uuid:').
bool python_int16(const std::string& value) {
  std::string ascii;
  for (char32_t c : text::decode(value)) {
    if (text::is_space(c)) {
      ascii.push_back(' ');
    } else if (c < 0x80) {
      ascii.push_back(static_cast<char>(c));
    } else if (const int digit = text::decimal_value(c); digit >= 0) {
      ascii.push_back(static_cast<char>('0' + digit));
    } else {
      return false;
    }
  }
  std::size_t i = 0;
  const std::size_t n = ascii.size();
  auto is_hex = [&](std::size_t k) { return k < n && std::isxdigit(static_cast<unsigned char>(ascii[k])); };
  while (i < n && ascii[i] == ' ') ++i;
  if (i < n && (ascii[i] == '+' || ascii[i] == '-')) ++i;
  if (i + 1 < n && ascii[i] == '0' && (ascii[i + 1] == 'x' || ascii[i + 1] == 'X')) {
    i += 2;
    if (i < n && ascii[i] == '_') ++i;
  }
  if (!is_hex(i)) return false;
  while (is_hex(i) || (i < n && ascii[i] == '_' && is_hex(i + 1))) ++i;
  while (i < n && ascii[i] == ' ') ++i;
  return i == n;
}

std::string replace_all(std::string text_value, std::string_view from) {
  for (std::size_t at = text_value.find(from); at != std::string::npos; at = text_value.find(from)) {
    text_value.erase(at, from.size());
  }
  return text_value;
}

bool device_control(const json& parameters, Check& check) {
  static const py::Regex kDeviceId(R"(^[A-Z0-9]+(?:-[A-Z0-9]+)*$)");
  const std::string path = "parameters";
  if (!check.object(parameters, path, {"target_device_id", "action", "parameters", "origin", "ai_device", "ai_action"},
                    {"target_device_id", "action", "parameters", "origin", "ai_device", "ai_action"})) {
    return false;
  }
  const json& id = parameters["target_device_id"];
  if (!check.string(id, path + ".target_device_id", 0, 64)) return false;
  if (!kDeviceId.search(text::sanitize(id.get<std::string>()))) return check.fail(path + ".target_device_id", "pattern");
  if (!check.one_of(parameters["action"], path + ".action",
                    {"set_switch", "set_color", "set_level", "set_position", "set_number"})) {
    return false;
  }
  if (!parameters["parameters"].is_object()) return check.fail(path + ".parameters", "is not of type 'object'");
  if (!check.one_of(parameters["origin"], path + ".origin", {"voice_parser", "llm", "ui"})) return false;
  if (!check.optional_string(parameters["ai_device"], path + ".ai_device", 64)) return false;
  if (!check.optional_string(parameters["ai_action"], path + ".ai_action", 64)) return false;
  const std::string action = parameters["action"];
  const json& inner = parameters["parameters"];
  const std::string inner_path = path + ".parameters";
  auto need = [&](const char* key) {
    return inner.contains(key) || check.fail(inner_path, std::string("'") + key + "' is a required property");
  };
  if (action == "set_switch") return need("on") && check.boolean(inner["on"], inner_path + ".on");
  if (action == "set_position") {
    return need("position_degrees") && check.number(inner["position_degrees"], inner_path + ".position_degrees", 0, 180);
  }
  if (action == "set_level") {
    return need("level_percent") && check.number(inner["level_percent"], inner_path + ".level_percent", 0, 100);
  }
  if (action == "set_color") {
    if (!need("color") || !check.one_of(inner["color"], inner_path + ".color", {"off", "green", "red", "amber"})) {
      return false;
    }
    return !inner.contains("pattern") || check.one_of(inner["pattern"], inner_path + ".pattern", {"steady", "blink"});
  }
  if (action == "set_number") {
    if (!need("value")) return false;
    const json& value = inner["value"];
    return check.integer(value, inner_path + ".value", 0) && check.number(value, inner_path + ".value", 0, 9999);
  }
  return true;
}

}  // namespace

bool jsonschema_uuid(const std::string& value) {
  std::string hex = replace_all(replace_all(value, "urn:"), "uuid:");
  const auto first = hex.find_first_not_of("{}");
  const auto last = hex.find_last_not_of("{}");
  hex = first == std::string::npos ? std::string() : hex.substr(first, last - first + 1);
  hex = replace_all(hex, "-");
  if (text::length(hex) != 32 || !python_int16(hex)) return false;
  const std::u32string cps = text::decode(value);
  for (std::size_t position : {8u, 13u, 18u, 23u}) {
    if (position >= cps.size() || cps[position] != U'-') return false;
  }
  return true;
}

std::string validate_llm_request(const json& request) {
  Check check;
  const bool ok = [&] {
    if (!check.object(request, "request",
                      {"schema_version", "event_id", "trace_id", "request_id", "source_node_id", "utterance_id",
                       "transcript", "parser", "requested_at_ms", "expires_at_ms"},
                      {"schema_version", "event_id", "trace_id", "request_id", "source_node_id", "utterance_id",
                       "transcript", "parser", "context", "requested_at_ms", "expires_at_ms", "simulated"})) {
      return false;
    }
    if (!check.constant(request["schema_version"], "schema_version", "pfn.v1")) return false;
    for (const char* key : {"event_id", "trace_id", "request_id", "utterance_id"}) {
      if (!check.uuid(request[key], key)) return false;
    }
    if (!check.constant(request["source_node_id"], "source_node_id", "NODE-RPI5-02")) return false;
    const json& transcript = request["transcript"];
    if (!check.object(transcript, "transcript", {"text", "language", "stt_model_version", "stt_confidence"},
                      {"text", "language", "stt_model_version", "stt_confidence"}) ||
        !check.string(transcript["text"], "transcript.text", 1, 500) ||
        !check.constant(transcript["language"], "transcript.language", "ko") ||
        !check.string(transcript["stt_model_version"], "transcript.stt_model_version", 1, 64)) {
      return false;
    }
    if (!transcript["stt_confidence"].is_null() &&
        !check.number(transcript["stt_confidence"], "transcript.stt_confidence", 0, 1)) {
      return false;
    }
    const json& parser = request["parser"];
    if (!check.object(parser, "parser", {"result", "rule_version"}, {"result", "rule_version"}) ||
        !check.one_of(parser["result"], "parser.result", {"NO_MATCH", "AMBIGUOUS", "LOW_CONFIDENCE"}) ||
        !check.string(parser["rule_version"], "parser.rule_version", 1, 64)) {
      return false;
    }
    if (request.contains("context")) {
      const json& context = request["context"];
      const Names keys = {"presence_zone", "activity", "audio_context", "quiet_required", "confidence"};
      if (!check.object(context, "context", keys, keys) ||
          !check.one_of(context["presence_zone"], "context.presence_zone",
                        {"BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"}) ||
          !check.one_of(context["activity"], "context.activity",
                        {"RESTING", "SLEEPING", "STUDYING", "WORKING", "MEETING", "BATHROOM_USE", "ENTERING",
                         "LEAVING", "GENERAL_ACTIVITY", "UNKNOWN"}) ||
          !check.one_of(context["audio_context"], "context.audio_context",
                        {"QUIET", "SELF_TALK", "TV_MEDIA", "ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING",
                         "UNKNOWN"}) ||
          !check.boolean(context["quiet_required"], "context.quiet_required") ||
          !check.number(context["confidence"], "context.confidence", 0, 1)) {
        return false;
      }
    }
    if (!check.integer(request["requested_at_ms"], "requested_at_ms", 0) ||
        !check.integer(request["expires_at_ms"], "expires_at_ms", 0)) {
      return false;
    }
    return !request.contains("simulated") || check.boolean(request["simulated"], "simulated");
  }();
  return ok ? std::string() : "llm-request.schema.json: " + check.problem();
}

std::string validate_task_request(const json& request) {
  Check check;
  const Names keys = {"schema_version", "event_id", "trace_id", "task_id", "source_node_id", "intent",
                      "target_device", "priority", "safety_level", "interruptible", "resume_policy", "confidence",
                      "parameters", "requested_at_ms"};
  const bool ok = [&] {
    if (!check.object(request, "request", keys, keys)) return false;
    if (!check.constant(request["schema_version"], "schema_version", "pfn.v1")) return false;
    for (const char* key : {"event_id", "trace_id", "task_id"}) {
      if (!check.uuid(request[key], key)) return false;
    }
    for (const char* key : {"source_node_id", "intent", "target_device"}) {
      if (!check.string(request[key], key, 1, 64)) return false;
    }
    if (!check.integer(request["priority"], "priority", 0) || !check.number(request["priority"], "priority", 0, 100)) {
      return false;
    }
    if (!check.one_of(request["safety_level"], "safety_level", {"normal", "caution", "critical"}) ||
        !check.boolean(request["interruptible"], "interruptible") ||
        !check.one_of(request["resume_policy"], "resume_policy", {"never", "manual", "when_context_returns", "automatic"}) ||
        !check.number(request["confidence"], "confidence", 0, 1) ||
        !check.integer(request["requested_at_ms"], "requested_at_ms", 0)) {
      return false;
    }
    if (!request["parameters"].is_object()) return check.fail("parameters", "is not of type 'object'");
    if (request["intent"] == "device_control") return device_control(request["parameters"], check);
    return true;
  }();
  if (ok) return {};
  return "task-request.schema.json: " + check.problem();
}

std::string validate_llm_result(const json& result) {
  Check check;
  const Names required = {"schema_version", "event_id", "trace_id", "request_id", "source_node_id", "status",
                          "task_id", "ai_intent", "ai_device", "ai_action", "safety_flags",
                          "clarification_question", "model", "completed_at_ms"};
  const Names allowed = {"schema_version", "event_id", "trace_id", "request_id", "source_node_id", "status",
                         "task_id", "ai_intent", "ai_device", "ai_action", "safety_flags", "clarification_question",
                         "model", "completed_at_ms", "simulated"};
  const bool ok = [&] {
    if (!check.object(result, "result", required, allowed)) return false;
    if (!check.constant(result["schema_version"], "schema_version", "pfn.v1")) return false;
    for (const char* key : {"event_id", "trace_id", "request_id"}) {
      if (!check.uuid(result[key], key)) return false;
    }
    if (!check.constant(result["source_node_id"], "source_node_id", "NODE-RPI5-01")) return false;
    if (!check.one_of(result["status"], "status",
                      {"task_requested", "clarify", "no_action", "rejected_guard", "invalid_output",
                       "model_unavailable", "expired"})) {
      return false;
    }
    const json& task_id = result["task_id"];
    if (result["status"] == "task_requested") {
      if (!check.uuid(task_id, "task_id")) return false;
    } else if (!task_id.is_null()) {
      return check.fail("task_id", "must be null");
    }
    if (!check.one_of(result["ai_intent"], "ai_intent",
                      {"DEVICE_CONTROL", "DEVICE_QUERY", "CONTEXT_SUGGESTION", "CLARIFY", "NO_ACTION"}, true) ||
        !check.optional_string(result["ai_device"], "ai_device", 64) ||
        !check.optional_string(result["ai_action"], "ai_action", 64) ||
        !check.optional_string(result["clarification_question"], "clarification_question", 200)) {
      return false;
    }
    const json& flags = result["safety_flags"];
    if (!flags.is_array()) return check.fail("safety_flags", "is not of type 'array'");
    std::set<std::string> seen;
    for (const auto& flag : flags) {
      if (!check.one_of(flag, "safety_flags[]",
                        {"AMBIGUOUS_TARGET", "AMBIGUOUS_ACTION", "UNSUPPORTED_DEVICE", "UNSUPPORTED_ACTION",
                         "PRIVACY_SENSITIVE", "HIGH_RISK", "CONFLICTING_REQUEST", "NEGATED_REQUEST",
                         "MODEL_OUTPUT_INVALID", "MODEL_UNAVAILABLE"})) {
        return false;
      }
      if (!seen.insert(flag.get<std::string>()).second) return check.fail("safety_flags", "has non-unique elements");
    }
    const json& model = result["model"];
    if (!check.object(model, "model", {"name", "origin", "latency_ms"}, {"name", "origin", "latency_ms"}) ||
        !check.string(model["name"], "model.name", 1, 128) ||
        !check.one_of(model["origin"], "model.origin", {"model", "guard", "model_guard", "guard_fallback", "none"}) ||
        !check.number(model["latency_ms"], "model.latency_ms", 0, kInf) ||
        !check.integer(result["completed_at_ms"], "completed_at_ms", 0)) {
      return false;
    }
    return !result.contains("simulated") || check.boolean(result["simulated"], "simulated");
  }();
  return ok ? std::string() : "llm-result.schema.json: " + check.problem();
}

}  // namespace hm::guard
