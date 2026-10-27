#include "hm/validation.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include "hm/config.hpp"
#include "hm/text.hpp"

namespace hm::guard {

namespace fs = std::filesystem;

namespace {

std::string join(const std::vector<std::string>& items, const std::string& separator) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) out += separator;
    out += items[i];
  }
  return out;
}

const ojson& get_or(const ojson& object, const std::string& key, const ojson& fallback) {
  if (!object.is_object()) return fallback;
  auto it = object.find(key);
  return it == object.end() ? fallback : *it;
}

// Literal keys (no std::string temporary, which GCC's -Wdangling-reference would flag).
const ojson& get_or(const ojson& object, const char* key, const ojson& fallback) {
  if (!object.is_object()) return fallback;
  auto it = object.find(key);
  return it == object.end() ? fallback : *it;
}

const ojson kNull = nullptr;
const ojson kEmptyObject = ojson::object();
const ojson kEmptyArray = ojson::array();

bool is_false_literal(const ojson& value) { return value.is_boolean() && !value.get<bool>(); }

bool matches_one(const ojson& value, const std::string& name) {
  if (name == "null") return value.is_null();
  if (name == "string") return value.is_string();
  if (name == "boolean") return value.is_boolean();
  if (name == "object") return value.is_object();
  if (name == "array") return value.is_array();
  if (name == "integer") return py::is_int(value);
  if (name == "number") return py::is_number(value);
  return false;
}

bool matches_type(const ojson& value, const ojson& allowed) {
  if (allowed.is_string()) return matches_one(value, allowed.get<std::string>());
  if (allowed.is_array()) {
    for (const auto& name : allowed) {
      if (name.is_string() && matches_one(value, name.get<std::string>())) return true;
    }
  }
  return false;
}

bool contained_in(const ojson& value, const ojson& options) {
  if (!options.is_array()) return false;
  return std::any_of(options.begin(), options.end(), [&](const ojson& option) { return py::equal(option, value); });
}

double as_float(const ojson& value) {
  if (value.is_number()) return value.get<double>();
  if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
  throw std::invalid_argument("schema bound is not a number");
}

std::vector<std::string> validate_value(const std::string& name, const ojson& value, const ojson& rule) {
  std::vector<std::string> errors;
  if (rule.contains("type") && !matches_type(value, rule["type"])) return {name + ": invalid type"};
  if (rule.contains("enum") && !contained_in(value, rule["enum"])) {
    errors.push_back(name + ": unsupported value " + py::repr(value));
  }
  if (value.is_string() && rule.contains("maxLength")) {
    if (text::length(value.get_ref<const std::string&>()) > static_cast<std::size_t>(as_float(rule["maxLength"]))) {
      errors.push_back(name + ": exceeds maxLength");
    }
  }
  if (py::is_number(value)) {
    if (rule.contains("minimum") && py::less_than(value, as_float(rule["minimum"]))) {
      errors.push_back(name + ": below minimum");
    }
    if (rule.contains("maximum") && py::greater_than(value, as_float(rule["maximum"]))) {
      errors.push_back(name + ": above maximum");
    }
  }
  return errors;
}

// len(value) != len(set(value)); set() raises TypeError on the first unhashable item.
bool has_duplicates(const ojson& items) {
  for (const auto& item : items) {
    if (item.is_array() || item.is_object()) throw PyTypeError("unhashable type: '" + py::type_name(item) + "'");
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    for (std::size_t j = i + 1; j < items.size(); ++j) {
      if (py::equal(items[i], items[j])) return true;
    }
  }
  return false;
}

std::vector<std::string> sorted_difference(const ojson& object, const ojson& known) {
  std::vector<std::string> names;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!known.is_object() || !known.contains(it.key())) names.push_back(it.key());
  }
  std::sort(names.begin(), names.end());  // byte order of UTF-8 == code point order, like sorted()
  return names;
}

std::vector<std::string> validate_structure(const ojson& proposal, const ojson& schema) {
  if (!proposal.is_object()) return {"proposal: expected object"};
  std::vector<std::string> errors;
  std::set<std::string> required;
  for (const auto& name : get_or(schema, "required", kEmptyArray)) {
    if (name.is_string()) required.insert(name.get<std::string>());
  }
  const ojson& properties = get_or(schema, "properties", kEmptyObject);
  for (const auto& name : required) {  // std::set iterates sorted
    if (!proposal.contains(name)) errors.push_back("missing field: " + name);
  }
  if (is_false_literal(get_or(schema, "additionalProperties", kNull))) {
    for (const auto& name : sorted_difference(proposal, properties)) errors.push_back("unknown field: " + name);
  }
  for (auto it = proposal.begin(); it != proposal.end(); ++it) {
    const std::string& name = it.key();
    const ojson& value = it.value();
    const ojson& rule = get_or(properties, name, kNull);
    if (!rule.is_object()) continue;
    auto value_errors = validate_value(name, value, rule);
    errors.insert(errors.end(), value_errors.begin(), value_errors.end());
    if (value.is_array()) {
      if (py::truthy(get_or(rule, "uniqueItems", kNull)) && has_duplicates(value)) {
        errors.push_back(name + ": duplicate values");
      }
      const ojson& item_rule = get_or(rule, "items", kEmptyObject);
      for (std::size_t index = 0; index < value.size(); ++index) {
        auto item_errors = validate_value(name + "[" + std::to_string(index) + "]", value[index],
                                          item_rule.is_object() ? item_rule : kEmptyObject);
        errors.insert(errors.end(), item_errors.begin(), item_errors.end());
      }
    }
    if (value.is_object()) {
      const ojson& nested = get_or(rule, "properties", kEmptyObject);
      if (is_false_literal(get_or(rule, "additionalProperties", kNull))) {
        for (const auto& extra : sorted_difference(value, nested)) errors.push_back(name + ": unknown field " + extra);
      }
      for (auto child = value.begin(); child != value.end(); ++child) {
        const ojson& child_rule = get_or(nested, child.key(), kNull);
        if (child_rule.is_object()) {
          auto child_errors = validate_value(name + "." + child.key(), child.value(), child_rule);
          errors.insert(errors.end(), child_errors.begin(), child_errors.end());
        }
      }
    }
  }
  return errors;
}

std::string as_text(const ojson& value) {
  // f"{value}" for the str values that reach the semantic checks
  return value.is_string() ? value.get<std::string>() : py::repr(value);
}

std::vector<std::string> validate_semantics(const ojson& proposal, const ojson& capabilities) {
  std::vector<std::string> errors;
  const ojson& intent = get_or(proposal, "intent", kNull);
  const ojson& device = get_or(proposal, "device", kNull);
  const ojson& action = get_or(proposal, "action", kNull);
  const ojson& parameters = get_or(proposal, "parameters", kNull);
  const ojson& ambiguous = get_or(proposal, "ambiguous", kNull);
  const ojson& question = get_or(proposal, "clarification_question", kNull);
  const ojson& flags = get_or(proposal, "safety_flags", kNull);
  if (!parameters.is_object() || !flags.is_array()) return errors;

  auto is = [](const ojson& value, const char* text) { return value.is_string() && value.get_ref<const std::string&>() == text; };

  if (ambiguous.is_boolean() && ambiguous.get<bool>()) {
    if (!is(intent, "CLARIFY")) errors.push_back("ambiguous=true requires intent=CLARIFY");
    if (!question.is_string() || text::strip(question.get_ref<const std::string&>()).empty()) {
      errors.push_back("CLARIFY requires a non-empty clarification_question");
    }
  } else if (is(intent, "CLARIFY")) {
    errors.push_back("intent=CLARIFY requires ambiguous=true");
  } else if (!question.is_null()) {
    errors.push_back("unambiguous proposals require clarification_question=null");
  }

  const bool actionable = is(intent, "DEVICE_CONTROL") || is(intent, "DEVICE_QUERY") || is(intent, "CONTEXT_SUGGESTION");
  if (actionable) {
    if (!device.is_string() || !action.is_string()) {
      errors.push_back("actionable intent requires device and action");
    } else {
      const ojson& devices = get_or(capabilities, "devices", kEmptyObject);
      const ojson& info = get_or(devices, device.get_ref<const std::string&>(), kNull);
      if (!info.is_object() || !py::truthy(get_or(info, "enabled", kNull))) {
        errors.push_back("device is unavailable: " + as_text(device));
      } else if (!contained_in(action, get_or(info, "actions", kEmptyArray))) {
        errors.push_back("action " + as_text(action) + " is not allowed for " + as_text(device));
      }
    }
  } else if (!device.is_null() || !action.is_null() || py::truthy(parameters)) {
    errors.push_back("CLARIFY/NO_ACTION must not contain device, action, or parameters");
  }

  if (is(intent, "DEVICE_QUERY") && !is(action, "QUERY_STATUS")) errors.push_back("DEVICE_QUERY requires action=QUERY_STATUS");
  if (is(action, "QUERY_STATUS") && !is(intent, "DEVICE_QUERY")) errors.push_back("QUERY_STATUS requires intent=DEVICE_QUERY");
  if (is(intent, "CONTEXT_SUGGESTION") && (!is(device, "ALL_QUIET_DEVICES") || !is(action, "SET_QUIET_MODE"))) {
    errors.push_back("CONTEXT_SUGGESTION requires ALL_QUIET_DEVICES/SET_QUIET_MODE");
  }

  const char* required_parameter = nullptr;
  if (is(action, "SET_BRIGHTNESS")) required_parameter = "brightness";
  if (is(action, "SET_VOLUME")) required_parameter = "volume";
  if (is(action, "SET_LEVEL")) required_parameter = "level";
  if (is(action, "SET_TEMPERATURE")) required_parameter = "temperature_celsius";
  if (required_parameter && !parameters.contains(required_parameter)) {
    errors.push_back(action.get<std::string>() + " requires parameters." + required_parameter);
  }

  auto has_flag = [&](const char* flag) { return contained_in(ojson(flag), flags); };
  if (has_flag("NEGATED_REQUEST") && !is(intent, "NO_ACTION")) errors.push_back("NEGATED_REQUEST must use intent=NO_ACTION");
  static const std::vector<std::string> kClarifyOnly = {
      "AMBIGUOUS_ACTION", "AMBIGUOUS_TARGET",   "CONFLICTING_REQUEST", "HIGH_RISK",
      "MODEL_OUTPUT_INVALID", "MODEL_UNAVAILABLE", "UNSUPPORTED_ACTION",  "UNSUPPORTED_DEVICE",
  };  // sorted, like sorted(set.intersection(...))
  std::vector<std::string> invalid_for_intent;
  for (const auto& flag : kClarifyOnly) {
    if (has_flag(flag.c_str())) invalid_for_intent.push_back(flag);
  }
  if (!invalid_for_intent.empty() && !is(intent, "CLARIFY")) {
    errors.push_back(join(invalid_for_intent, ", ") + " must use intent=CLARIFY");
  }
  return errors;
}

}  // namespace

ProposalValidationError::ProposalValidationError(std::vector<std::string> errors)
    : std::runtime_error(join(errors, "; ")), errors_(std::move(errors)) {}

fs::path default_schema_path(const fs::path& contracts_dir) {
  return (contracts_dir.empty() ? default_contracts_dir() : contracts_dir) / "rpi5-internal" /
         "task-proposal.schema.json";
}

fs::path default_activity_context_schema_path(const fs::path& contracts_dir) {
  return (contracts_dir.empty() ? default_contracts_dir() : contracts_dir) / "rpi5-internal" /
         "activity-context.schema.json";
}

fs::path default_capabilities_path() {
  if (const char* env = std::getenv("HM_CAPABILITIES_PATH"); env && *env) return env;
#ifdef HM_CAPABILITIES_DEFAULT
  if (fs::exists(HM_CAPABILITIES_DEFAULT)) return HM_CAPABILITIES_DEFAULT;
#endif
  const char* base = std::getenv("HM_BASE_DIR");
  return fs::path(base && *base ? base : fs::current_path().string()) / "services" / "exaone-edge" / "config" /
         "device-capabilities.json";
}

ojson load_json_object(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string bytes = buffer.str();
  if (auto error = text::utf8_decode_error(bytes)) throw std::runtime_error(path.string() + ": " + *error);
  ojson value;
  try {
    value = py::loads(bytes);
  } catch (const py::JsonDecodeError& error) {
    throw std::runtime_error(path.string() + ": " + error.what());
  }
  if (!value.is_object()) throw std::runtime_error("Expected a JSON object in " + path.string());
  return value;
}

ojson default_activity_context() {
  ojson context = ojson::object();
  context["presence_zone"] = "UNKNOWN";
  context["activity"] = "UNKNOWN";
  context["audio_context"] = "UNKNOWN";
  context["quiet_required"] = false;
  context["confidence"] = 0.0;
  return context;
}

ojson validate_proposal(const ojson& proposal, const ojson& schema, const ojson& capabilities) {
  std::vector<std::string> errors = validate_structure(proposal, schema);
  if (errors.empty() && proposal.is_object()) errors = validate_semantics(proposal, capabilities);
  if (!errors.empty()) throw ProposalValidationError(std::move(errors));
  return proposal;
}

ojson validate_activity_context(const ojson& context, const ojson& schema) {
  ojson normalized = default_activity_context();
  if (py::truthy(context)) {
    if (!context.is_object()) throw std::invalid_argument("context must be an object");
    for (auto it = context.begin(); it != context.end(); ++it) normalized[it.key()] = it.value();
  }
  std::vector<std::string> errors = validate_structure(normalized, schema);
  if (!errors.empty()) throw ProposalValidationError(std::move(errors));
  return normalized;
}

ojson safe_fallback(bool unavailable, const std::optional<std::string>& question) {
  ojson proposal = ojson::object();
  proposal["intent"] = "CLARIFY";
  proposal["device"] = nullptr;
  proposal["action"] = nullptr;
  proposal["parameters"] = ojson::object();
  proposal["ambiguous"] = true;
  proposal["safety_flags"] = ojson::array({unavailable ? "MODEL_UNAVAILABLE" : "MODEL_OUTPUT_INVALID"});
  proposal["clarification_question"] =
      question && !question->empty() ? *question
                                     : std::string("요청을 안전하게 해석하지 못했습니다. 대상 기기와 원하는 동작을 다시 말해 주세요.");
  return proposal;
}

}  // namespace hm::guard
