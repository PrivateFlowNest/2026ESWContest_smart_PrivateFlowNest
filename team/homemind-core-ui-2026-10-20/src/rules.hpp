#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace homemind {

using RuleJson = nlohmann::json;

inline RuleJson load_rule_file(const std::string& path, const std::string& label) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open " + label + " file: " + path);
  }
  try {
    return RuleJson::parse(input);
  } catch (const std::exception& error) {
    throw std::runtime_error("cannot parse " + label + " file " + path + ": " + error.what());
  }
}

class SensorRuleSet {
 public:
  explicit SensorRuleSet(const std::string& path) : document_(load_rule_file(path, "sensor rules")) {
    if (!document_.is_object() || document_.value("schema_version", "") != "pfn.v1" ||
        !document_.contains("readings") || !document_.at("readings").is_object() ||
        !document_.contains("reject_unknown_readings") ||
        !document_.at("reject_unknown_readings").is_boolean()) {
      throw std::runtime_error("sensor rules have an invalid top-level structure");
    }
  }

  std::optional<std::string> validate(const RuleJson& telemetry) const {
    if (!telemetry.contains("readings") || !telemetry.at("readings").is_object()) {
      return "readings must be an object";
    }
    if (!telemetry.contains("reading_sources") || !telemetry.at("reading_sources").is_object()) {
      return "reading_sources must be an object";
    }
    const RuleJson& readings = telemetry.at("readings");
    const RuleJson& sources = telemetry.at("reading_sources");
    if (telemetry.value("sensor_ready", false) && readings.empty()) {
      return "sensor_ready telemetry must contain at least one reading";
    }
    for (const auto& [name, value] : readings.items()) {
      if (!sources.contains(name) || !sources.at(name).is_string()) {
        return "reading_sources is missing a device ID for " + name;
      }
      if (!document_.at("readings").contains(name)) {
        if (document_.at("reject_unknown_readings").get<bool>()) {
          return "unknown reading: " + name;
        }
        continue;
      }
      const RuleJson& rule = document_.at("readings").at(name);
      const std::string expected_type = rule.value("type", "");
      const bool type_ok =
          (expected_type == "number" && value.is_number()) ||
          (expected_type == "integer" && value.is_number_integer()) ||
          (expected_type == "boolean" && value.is_boolean()) ||
          (expected_type == "string" && value.is_string());
      if (!type_ok) {
        return "reading " + name + " must be " + expected_type;
      }
      if (value.is_number()) {
        const double number = value.get<double>();
        if (rule.contains("minimum") && number < rule.at("minimum").get<double>()) {
          return "reading " + name + " is below minimum";
        }
        if (rule.contains("maximum") && number > rule.at("maximum").get<double>()) {
          return "reading " + name + " is above maximum";
        }
      }
      if (rule.contains("enum")) {
        bool allowed = false;
        for (const auto& candidate : rule.at("enum")) {
          if (candidate == value) {
            allowed = true;
            break;
          }
        }
        if (!allowed) {
          return "reading " + name + " is not an allowed value";
        }
      }
      const std::string source = sources.at(name).get<std::string>();
      bool source_allowed = false;
      if (rule.contains("allowed_source_device_ids") &&
          rule.at("allowed_source_device_ids").is_array()) {
        for (const auto& candidate : rule.at("allowed_source_device_ids")) {
          if (candidate.is_string() && candidate.get<std::string>() == source) {
            source_allowed = true;
            break;
          }
        }
      }
      if (!source_allowed) {
        return "reading " + name + " has an unregistered source device";
      }
    }
    for (const auto& [name, unused] : sources.items()) {
      static_cast<void>(unused);
      if (!readings.contains(name)) {
        return "reading_sources contains an unused key: " + name;
      }
    }
    return std::nullopt;
  }

 private:
  RuleJson document_;
};

class ScenarioRuleSet {
 public:
  explicit ScenarioRuleSet(const std::string& path)
      : document_(load_rule_file(path, "scenario rules")) {
    if (!document_.is_object() || document_.value("schema_version", "") != "pfn.v1" ||
        !document_.contains("scenarios") || !document_.at("scenarios").is_array()) {
      throw std::runtime_error("scenario rules have an invalid top-level structure");
    }
    for (const auto& scenario : document_.at("scenarios")) {
      if (!scenario.is_object() || !scenario.contains("intent") ||
          !scenario.at("intent").is_string() || !scenario.contains("commands") ||
          !scenario.at("commands").is_array() || scenario.at("commands").empty()) {
        throw std::runtime_error("scenario rules contain an invalid scenario");
      }
    }
  }

  const RuleJson& scenarios() const { return document_.at("scenarios"); }

  const RuleJson* find(const std::string& intent) const {
    for (const auto& scenario : document_.at("scenarios")) {
      if (scenario.at("intent").get<std::string>() == intent) {
        return &scenario;
      }
    }
    return nullptr;
  }

 private:
  RuleJson document_;
};


// A task request that passed the structural checks but that core refuses to execute.
// Codes are listed in contracts/pfn.v1/task-policy.json rejection_codes.
struct Rejection {
  std::string code;
  std::string message;
};

inline bool json_array_contains(const RuleJson& array, const std::string& value) {
  if (!array.is_array()) {
    return false;
  }
  for (const auto& item : array) {
    if (item.is_string() && item.get<std::string>() == value) {
      return true;
    }
  }
  return false;
}

// contracts/pfn.v1/device-registry.json: the only source of controller, wiring and capability.
class DeviceRegistry {
 public:
  explicit DeviceRegistry(const std::string& path)
      : document_(load_rule_file(path, "device registry")) {
    if (!document_.is_object() || document_.value("schema_version", "") != "pfn.v1" ||
        !document_.contains("nodes") || !document_.at("nodes").is_array() ||
        !document_.contains("devices") || !document_.at("devices").is_array() ||
        !document_.contains("zones") || !document_.at("zones").is_array()) {
      throw std::runtime_error("device registry has an invalid top-level structure");
    }
    for (const auto& device : document_.at("devices")) {
      if (!device.is_object() || !device.contains("device_id") ||
          !device.at("device_id").is_string() || !device.contains("capabilities") ||
          !device.at("capabilities").is_array() || !device.contains("wiring_status") ||
          !device.at("wiring_status").is_string()) {
        throw std::runtime_error("device registry contains an invalid device");
      }
    }
  }

  const RuleJson* device(const std::string& device_id) const {
    for (const auto& device : document_.at("devices")) {
      if (device.at("device_id").get<std::string>() == device_id) {
        return &device;
      }
    }
    return nullptr;
  }

  bool has_zone(const std::string& zone) const {
    return json_array_contains(document_.at("zones"), zone);
  }

  std::optional<std::string> node_role(const std::string& node_id) const {
    for (const auto& node : document_.at("nodes")) {
      if (node.value("node_id", "") == node_id) {
        return node.value("role", "");
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> controller(const std::string& device_id) const {
    const RuleJson* found = device(device_id);
    if (!found || !found->contains("controller_node_id") ||
        !found->at("controller_node_id").is_string()) {
      return std::nullopt;
    }
    return found->at("controller_node_id").get<std::string>();
  }

  // "open" or "closed" when the angle is one of the registered servo positions.
  std::optional<std::string> position_ref(const std::string& device_id,
                                          const RuleJson& parameters) const {
    const RuleJson* found = device(device_id);
    if (!found || !found->contains("positions") || !parameters.is_object() ||
        !parameters.contains("position_degrees") ||
        !parameters.at("position_degrees").is_number()) {
      return std::nullopt;
    }
    const double degrees = parameters.at("position_degrees").get<double>();
    for (const char* name : {"open", "closed"}) {
      const RuleJson& positions = found->at("positions");
      if (positions.contains(name) && positions.at(name).is_number() &&
          positions.at(name).get<double>() == degrees) {
        return std::string(name);
      }
    }
    return std::nullopt;
  }

  // Same rules as contracts/validate-contracts.py check_device_action.
  std::optional<Rejection> check_command(const std::string& device_id, const std::string& action,
                                         const RuleJson& parameters) const {
    const RuleJson* found = device(device_id);
    if (!found) {
      return Rejection{"DEVICE_UNKNOWN", device_id + " is not in the device registry"};
    }
    const std::string wiring = found->at("wiring_status").get<std::string>();
    if (wiring != "wired") {
      return Rejection{"DEVICE_NOT_WIRED", device_id + " wiring_status is " + wiring};
    }
    if (!json_array_contains(found->at("capabilities"), action)) {
      return Rejection{"CAPABILITY_NOT_ALLOWED", action + " is not a capability of " + device_id};
    }
    if (!parameters.is_object()) {
      return Rejection{"PARAMETERS_INVALID", "parameters must be an object"};
    }
    const auto number_between = [&](const char* name, double minimum, double maximum) {
      return parameters.contains(name) && parameters.at(name).is_number() &&
             parameters.at(name).get<double>() >= minimum &&
             parameters.at(name).get<double>() <= maximum;
    };
    if (action == "set_switch" &&
        (!parameters.contains("on") || !parameters.at("on").is_boolean())) {
      return Rejection{"PARAMETERS_INVALID", "set_switch needs boolean on"};
    }
    if (action == "set_level" && !number_between("level_percent", 0, 100)) {
      return Rejection{"PARAMETERS_INVALID", "set_level needs level_percent 0..100"};
    }
    if (action == "set_number" &&
        (!parameters.contains("value") || !parameters.at("value").is_number_integer() ||
         parameters.at("value").get<std::int64_t>() < 0 ||
         parameters.at("value").get<std::int64_t>() > 9999)) {
      return Rejection{"PARAMETERS_INVALID", "set_number needs integer value 0..9999"};
    }
    if (action == "set_color") {
      if (!parameters.contains("color") || !parameters.at("color").is_string()) {
        return Rejection{"PARAMETERS_INVALID", "set_color needs color"};
      }
      const std::string color = parameters.at("color").get<std::string>();
      if (color != "off" && color != "green" && color != "red" && color != "amber") {
        return Rejection{"PARAMETERS_INVALID", "unknown colour " + color};
      }
      if (parameters.contains("pattern") &&
          (!parameters.at("pattern").is_string() ||
           (parameters.at("pattern") != "steady" && parameters.at("pattern") != "blink"))) {
        return Rejection{"PARAMETERS_INVALID", "pattern must be steady or blink"};
      }
      if (found->contains("supported_colors") &&
          !json_array_contains(found->at("supported_colors"), color)) {
        return Rejection{"COLOR_NOT_SUPPORTED", color + " is not supported by " + device_id};
      }
    }
    if (action == "set_position") {
      if (!number_between("position_degrees", 0, 180)) {
        return Rejection{"PARAMETERS_INVALID", "set_position needs position_degrees 0..180"};
      }
      if (!position_ref(device_id, parameters)) {
        return Rejection{"POSITION_NOT_REGISTERED",
                         "angle is not a registered open/closed position of " + device_id};
      }
    }
    return std::nullopt;
  }

 private:
  RuleJson document_;
};

// contracts/pfn.v1/task-policy.json: who may send which origin, and which tasks run without UI approval.
class TaskPolicy {
 public:
  struct Decision {
    std::string approval;
    std::string rule;
    bool phone_may_approve = true;  // false: approve / resume only from the 7-inch UI, never pfn/v1/mobile/task/action
    std::int64_t proposal_ttl_ms = 600000;  // a proposed task older than this expires (PROPOSAL_EXPIRED)
  };

  explicit TaskPolicy(const std::string& path) : document_(load_rule_file(path, "task policy")) {
    if (!document_.is_object() || document_.value("schema_version", "") != "pfn.v1" ||
        !document_.contains("origins") || !document_.at("origins").is_object() ||
        !document_.contains("rules") || !document_.at("rules").is_array() ||
        document_.value("default_approval", "") != "ui_approval") {
      throw std::runtime_error("task policy has an invalid top-level structure");
    }
    // policy_version 3. A version 2 file (no proposal_ttl_seconds) keeps the 600 s default instead of stopping core.
    if (document_.contains("proposal_ttl_seconds")) {
      default_ttl_ms_ = ttl_ms(document_.at("proposal_ttl_seconds"));
    }
    for (const auto& rule : document_.at("rules")) {
      const std::string approval = rule.value("approval", "");
      if (!rule.is_object() || !rule.contains("name") ||
          (approval != "auto" && approval != "ui_approval") ||
          (rule.contains("phone_may_approve") && !rule.at("phone_may_approve").is_boolean())) {
        throw std::runtime_error("task policy contains an invalid rule");
      }
      if (rule.contains("proposal_ttl_seconds")) {
        ttl_ms(rule.at("proposal_ttl_seconds"));
      }
    }
  }

  std::int64_t default_proposal_ttl_ms() const { return default_ttl_ms_; }

  // "unspecified" has no node restriction (it can never be auto-approved). Unknown origins fail.
  bool source_allowed(const std::string& origin, const std::string& source_node_id) const {
    if (origin == "unspecified") {
      return true;
    }
    const RuleJson& origins = document_.at("origins");
    if (!origins.contains(origin)) {
      return false;
    }
    return json_array_contains(origins.at(origin).value("allowed_source_nodes", RuleJson::array()),
                               source_node_id);
  }

  Decision decide(const std::string& intent, const std::string& origin,
                  const std::optional<std::string>& device_id,
                  const std::optional<std::string>& position_ref) const {
    for (const auto& rule : document_.at("rules")) {
      if (rule.contains("intents") && !json_array_contains(rule.at("intents"), intent)) {
        continue;
      }
      if (rule.contains("origins") && !json_array_contains(rule.at("origins"), origin)) {
        continue;
      }
      if (rule.contains("target_device_ids") &&
          (!device_id || !json_array_contains(rule.at("target_device_ids"), *device_id))) {
        continue;
      }
      if (rule.contains("position_refs") &&
          (!position_ref || !json_array_contains(rule.at("position_refs"), *position_ref))) {
        continue;
      }
      return Decision{rule.at("approval").get<std::string>(), rule.at("name").get<std::string>(),
                      rule.value("phone_may_approve", true),
                      rule.contains("proposal_ttl_seconds") ? ttl_ms(rule.at("proposal_ttl_seconds"))
                                                            : default_ttl_ms_};
    }
    return Decision{"ui_approval", "default", true, default_ttl_ms_};
  }

 private:
  // task-policy.schema.json: whole seconds, 30..86400.
  static std::int64_t ttl_ms(const RuleJson& value) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < 30 || value.get<std::int64_t>() > 86400) {
      throw std::runtime_error("task policy proposal_ttl_seconds must be an integer 30..86400");
    }
    return value.get<std::int64_t>() * 1000;
  }

  RuleJson document_;
  std::int64_t default_ttl_ms_ = 600000;
};

// contracts/pfn.v1/context-rules.json: perception attributes -> scenario intent.
class ContextRuleSet {
 public:
  struct Match {
    std::string name;
    std::string group;
    std::string intent;
  };

  // feedback_overrides (rules_version 2): a negative context feedback undoes the group's active intent.
  struct FeedbackOverride {
    std::string name;
    std::string group;
    std::string feedback_type;
    std::string rating;
    std::string active_intent;
    std::string intent;
    std::string suppress_until_rule;
    std::int64_t max_suppress_ms = 0;
  };

  explicit ContextRuleSet(const std::string& path)
      : document_(load_rule_file(path, "context rules")) {
    if (!document_.is_object() || document_.value("schema_version", "") != "pfn.v1" ||
        !document_.contains("rules") || !document_.at("rules").is_array()) {
      throw std::runtime_error("context rules have an invalid top-level structure");
    }
    for (const auto& rule : document_.at("rules")) {
      if (!rule.is_object() || !rule.contains("match") || !rule.at("match").is_object() ||
          !rule.contains("intent") || !rule.at("intent").is_string() ||
          !rule.contains("observation_type") || !rule.contains("group")) {
        throw std::runtime_error("context rules contain an invalid rule");
      }
    }
    if (document_.contains("feedback_overrides")) {
      if (!document_.at("feedback_overrides").is_array()) {
        throw std::runtime_error("context rules feedback_overrides must be a list");
      }
      for (const auto& item : document_.at("feedback_overrides")) {
        const char* const keys[] = {"name", "group", "feedback_type", "rating", "active_intent", "intent",
                                    "suppress_until_rule"};
        bool valid = item.is_object() && item.contains("max_suppress_seconds") &&
                     item.at("max_suppress_seconds").is_number_integer() &&
                     item.at("max_suppress_seconds").get<std::int64_t>() > 0;
        for (const char* key : keys) {
          valid = valid && item.contains(key) && item.at(key).is_string();
        }
        if (!valid) {
          throw std::runtime_error("context rules contain an invalid feedback override");
        }
        overrides_.push_back(FeedbackOverride{
            item.at("name").get<std::string>(), item.at("group").get<std::string>(),
            item.at("feedback_type").get<std::string>(), item.at("rating").get<std::string>(),
            item.at("active_intent").get<std::string>(), item.at("intent").get<std::string>(),
            item.at("suppress_until_rule").get<std::string>(),
            item.at("max_suppress_seconds").get<std::int64_t>() * 1000});
      }
    }
  }

  std::optional<Match> match(const RuleJson& perception) const {
    const std::string observation_type = perception.value("observation_type", "");
    if (!perception.contains("attributes") || !perception.at("attributes").is_object()) {
      return std::nullopt;
    }
    const RuleJson& attributes = perception.at("attributes");
    for (const auto& rule : document_.at("rules")) {
      if (rule.at("observation_type") != observation_type) {
        continue;
      }
      bool matched = true;
      for (const auto& [key, expected] : rule.at("match").items()) {
        if (!attributes.contains(key) || attributes.at(key) != expected) {
          matched = false;
          break;
        }
      }
      if (matched) {
        return Match{rule.value("name", ""), rule.at("group").get<std::string>(),
                     rule.at("intent").get<std::string>()};
      }
    }
    return std::nullopt;
  }

  const RuleJson& rules() const { return document_.at("rules"); }
  const std::vector<FeedbackOverride>& feedback_overrides() const { return overrides_; }

 private:
  RuleJson document_;
  std::vector<FeedbackOverride> overrides_;
};

}  // namespace homemind
