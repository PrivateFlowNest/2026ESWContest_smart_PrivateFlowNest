#include "common.hpp"
#include "rules.hpp"
#include "schema.hpp"

#include <mosquitto.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/statvfs.h>
#include <sys/timex.h>

namespace homemind {
using Json = nlohmann::json;

inline void require_mosquitto(int result, const std::string& operation) {
  if (result != MOSQ_ERR_SUCCESS) {
    throw std::runtime_error(operation + ": " + mosquitto_strerror(result));
  }
}

class Statement {
 public:
  Statement(sqlite3* database, const char* sql) : database_(database) {
    if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(database_));
    }
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  sqlite3_stmt* get() { return statement_; }
  void text(int index, const std::string& value) {
    sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT);
  }
  void nullable_text(int index, const std::optional<std::string>& value) {
    if (value) {
      text(index, *value);
    } else {
      sqlite3_bind_null(statement_, index);
    }
  }
  void integer(int index, std::int64_t value) { sqlite3_bind_int64(statement_, index, value); }
  void real(int index, double value) { sqlite3_bind_double(statement_, index, value); }
  void nullable_real(int index, const std::optional<double>& value) {
    if (value) {
      real(index, *value);
    } else {
      sqlite3_bind_null(statement_, index);
    }
  }
  void nullable_integer(int index, const std::optional<std::int64_t>& value) {
    if (value) {
      integer(index, *value);
    } else {
      sqlite3_bind_null(statement_, index);
    }
  }
  void run() {
    if (sqlite3_step(statement_) != SQLITE_DONE) {
      throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errmsg(database_));
    }
  }

 private:
  sqlite3* database_{nullptr};
  sqlite3_stmt* statement_{nullptr};
};

struct ParsedMessage {
  Json document;
  std::string kind{"unknown"};
  std::optional<std::string> event_id;
  std::optional<std::string> schema_version;
  std::optional<std::string> node_id;
  std::optional<std::string> trace_id;
  bool from_phone{false};  // pfn/v1/mobile/task/action: only homemind-mobile-api may write it (broker ACL)
  bool valid{true};
  std::string error;
};

struct OutboundMessage {
  std::string topic;
  Json document;
  int qos{1};
  bool retained{false};
};

struct StorageStatus {
  std::uint64_t database_bytes{0};
  std::uint64_t filesystem_total_bytes{0};
  std::uint64_t filesystem_available_bytes{0};
  double filesystem_used_percent{0.0};
  int warning_threshold_percent{70};
  bool warning_active{false};
};

struct ClockReading {
  std::int64_t wall_time_ms{0};
  std::int64_t boot_uptime_ms{0};
  bool time_valid{false};
};

bool system_time_is_valid(std::int64_t wall_time_ms) {
  // Reject obviously reset clocks even if the kernel reports an unusual state.
  constexpr std::int64_t earliest_plausible_time_ms = 1735689600000LL;  // 2025-01-01 UTC
  if (wall_time_ms < earliest_plausible_time_ms) {
    return false;
  }
  struct timex status {};
  const int state = adjtimex(&status);
  return state >= 0 && state != TIME_ERROR && (status.status & STA_UNSYNC) == 0;
}

ClockReading read_clock() {
  ClockReading reading;
  reading.wall_time_ms = now_ms();
  reading.boot_uptime_ms = uptime_ms();
  reading.time_valid = system_time_is_valid(reading.wall_time_ms);
  return reading;
}

std::string read_boot_id() {
  std::ifstream input("/proc/sys/kernel/random/boot_id");
  std::string value;
  if (input && std::getline(input, value) && is_uuid(value)) {
    return value;
  }
  // Keep recording even on an unusual platform where Linux boot_id is unavailable.
  return uuid_v4();
}

std::uint64_t file_size_or_zero(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

StorageStatus inspect_storage(const std::string& database_path, int warning_threshold_percent) {
  StorageStatus status;
  status.warning_threshold_percent = warning_threshold_percent;
  status.database_bytes = file_size_or_zero(database_path) +
                          file_size_or_zero(database_path + "-wal") +
                          file_size_or_zero(database_path + "-shm");

  struct statvfs filesystem_information {};
  if (statvfs(database_path.c_str(), &filesystem_information) != 0) {
    throw std::runtime_error("cannot inspect database filesystem capacity");
  }
  status.filesystem_total_bytes =
      static_cast<std::uint64_t>(filesystem_information.f_blocks) *
      static_cast<std::uint64_t>(filesystem_information.f_frsize);
  status.filesystem_available_bytes =
      static_cast<std::uint64_t>(filesystem_information.f_bavail) *
      static_cast<std::uint64_t>(filesystem_information.f_frsize);
  if (status.filesystem_total_bytes > 0) {
    const auto used = status.filesystem_total_bytes - status.filesystem_available_bytes;
    status.filesystem_used_percent =
        100.0 * static_cast<double>(used) / static_cast<double>(status.filesystem_total_bytes);
  }
  status.warning_active = status.filesystem_used_percent >= warning_threshold_percent;
  return status;
}

std::optional<std::string> string_field(const Json& value, const char* name) {
  if (!value.contains(name) || !value.at(name).is_string()) {
    return std::nullopt;
  }
  return value.at(name).get<std::string>();
}

bool has_integer(const Json& value, const char* name) {
  return value.contains(name) && value.at(name).is_number_integer() &&
         value.at(name).get<std::int64_t>() >= 0;
}

bool has_number_between(const Json& value, const char* name, double minimum, double maximum) {
  if (!value.contains(name) || !value.at(name).is_number()) {
    return false;
  }
  const double number = value.at(name).get<double>();
  return number >= minimum && number <= maximum;
}

bool valid_identifier(const std::optional<std::string>& value) {
  if (!value || value->empty() || value->size() > 64) {
    return false;
  }
  for (const char character : *value) {
    const bool valid = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '-' ||
                       character == '_' || character == '.';
    if (!valid) {
      return false;
    }
  }
  return true;
}

bool one_of(const std::optional<std::string>& value,
            std::initializer_list<const char*> allowed) {
  if (!value) {
    return false;
  }
  for (const char* candidate : allowed) {
    if (*value == candidate) {
      return true;
    }
  }
  return false;
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<std::string> topic_node(const std::string& topic, const std::string& suffix) {
  constexpr const char* prefix = "pfn/v1/device/";
  if (!topic.starts_with(prefix) || !ends_with(topic, suffix)) {
    return std::nullopt;
  }
  const std::size_t begin = std::char_traits<char>::length(prefix);
  const std::size_t length = topic.size() - begin - suffix.size();
  if (length == 0 || length > 64) {
    return std::nullopt;
  }
  return topic.substr(begin, length);
}

void invalidate(ParsedMessage& parsed, const std::string& error) {
  if (parsed.valid) {
    parsed.valid = false;
    parsed.error = error;
  }
}

void require_common(ParsedMessage& parsed) {
  parsed.schema_version = string_field(parsed.document, "schema_version");
  parsed.event_id = string_field(parsed.document, "event_id");
  parsed.trace_id = string_field(parsed.document, "trace_id");
  if (!parsed.schema_version || *parsed.schema_version != "pfn.v1") {
    invalidate(parsed, "schema_version must be pfn.v1");
  } else if (!parsed.event_id || !is_uuid(*parsed.event_id)) {
    invalidate(parsed, "event_id must be a UUID");
  } else if (!parsed.trace_id || !is_uuid(*parsed.trace_id)) {
    invalidate(parsed, "trace_id must be a UUID");
  }
}

bool private_ai_topic(const std::string& topic) {
  // pfn/v1/ai/llm/* carries raw transcripts between the RPI5 nodes only. Core never parses or stores it.
  return topic.starts_with("pfn/v1/ai/llm/");
}

ParsedMessage parse_message(const std::string& topic, const std::string& payload,
                            const SensorRuleSet* sensor_rules = nullptr,
                            const DeviceRegistry* registry = nullptr) {
  ParsedMessage parsed;
  try {
    parsed.document = Json::parse(payload);
  } catch (const std::exception& error) {
    parsed.valid = false;
    parsed.kind = "invalid_json";
    parsed.error = error.what();
    return parsed;
  }
  if (!parsed.document.is_object()) {
    parsed.valid = false;
    parsed.kind = "invalid_json";
    parsed.error = "payload must be a JSON object";
    return parsed;
  }

  if (topic == "pfn/v1/system/heartbeat") {
    parsed.kind = "heartbeat";
  } else if (topic_node(topic, "/command")) {
    parsed.kind = "command";
  } else if (topic_node(topic, "/ack")) {
    parsed.kind = "ack";
  } else if (topic_node(topic, "/telemetry")) {
    parsed.kind = "telemetry";
  } else if (topic == "pfn/v1/ai/perception") {
    parsed.kind = "perception";
  } else if (topic == "pfn/v1/core/task/request") {
    parsed.kind = "task_request";
  } else if (topic == "pfn/v1/core/state/request") {
    parsed.kind = "state_request";
  } else if (topic == "pfn/v1/ui/action") {
    parsed.kind = "ui_action";
  } else if (topic == "pfn/v1/mobile/task/action") {
    parsed.kind = "ui_action";
    parsed.from_phone = true;
  } else if (topic == "pfn/v1/system/safety/alert") {
    parsed.kind = "safety_alert";
  } else if (topic == "pfn/v1/ui/safety/ack") {
    parsed.kind = "safety_ack";
  } else if (topic == "pfn/v1/ui/privacy/set") {
    parsed.kind = "privacy_set";
  } else if (topic == "pfn/v1/system/privacy/state") {
    parsed.kind = "privacy_state";
  } else if (topic == "pfn/v1/ui/feedback") {
    parsed.kind = "feedback";
  } else if (topic == "pfn/v1/system/health") {
    parsed.kind = "system_health";
  } else {
    parsed.event_id = string_field(parsed.document, "event_id");
    parsed.schema_version = string_field(parsed.document, "schema_version");
    parsed.node_id = string_field(parsed.document, "node_id");
    parsed.trace_id = string_field(parsed.document, "trace_id");
    return parsed;
  }

  require_common(parsed);
  if (!parsed.valid) {
    return parsed;
  }

  if (parsed.kind == "command") {
    const auto command_id = string_field(parsed.document, "command_id");
    const auto target = string_field(parsed.document, "target_node_id");
    const auto target_device = string_field(parsed.document, "target_device_id");
    const auto action = string_field(parsed.document, "action");
    const bool task_id_ok = !parsed.document.contains("task_id") ||
                            parsed.document.at("task_id").is_null() ||
                            (parsed.document.at("task_id").is_string() &&
                             is_uuid(parsed.document.at("task_id").get<std::string>()));
    parsed.node_id = target;
    if (!command_id || !is_uuid(*command_id)) {
      invalidate(parsed, "command_id must be a UUID");
    } else if (!task_id_ok) {
      invalidate(parsed, "task_id must be a UUID or null");
    } else if (!valid_identifier(target) || !valid_identifier(target_device)) {
      invalidate(parsed, "target node or device ID is invalid");
    } else if (topic_node(topic, "/command") != target) {
      invalidate(parsed, "topic node and target_node_id differ");
    } else if (!one_of(action, {"set_switch", "set_color", "set_level", "set_position",
                                "move_axis", "home_axis", "set_led", "set_number"})) {
      invalidate(parsed, "unsupported action");
    } else if (!parsed.document.contains("parameters") ||
               !parsed.document.at("parameters").is_object()) {
      invalidate(parsed, "parameters must be an object");
    } else if ((*action == "set_switch" || *action == "set_led") &&
               (!parsed.document.at("parameters").contains("on") ||
                !parsed.document.at("parameters").at("on").is_boolean())) {
      invalidate(parsed, "parameters.on must be boolean");
    } else if (*action == "set_level" &&
               !has_number_between(parsed.document.at("parameters"), "level_percent", 0, 100)) {
      invalidate(parsed, "parameters.level_percent must be between 0 and 100");
    } else if (*action == "set_position" &&
               !has_number_between(parsed.document.at("parameters"), "position_degrees", 0, 180)) {
      invalidate(parsed, "parameters.position_degrees must be between 0 and 180");
    } else if (*action == "set_color" &&
               !one_of(string_field(parsed.document.at("parameters"), "color"),
                       {"off", "green", "red", "amber"})) {
      invalidate(parsed, "parameters.color is invalid");
    } else if (*action == "set_color" && parsed.document.at("parameters").contains("pattern") &&
               !one_of(string_field(parsed.document.at("parameters"), "pattern"),
                       {"steady", "blink"})) {
      invalidate(parsed, "parameters.pattern must be steady or blink");
    } else if (*action == "set_number" &&
               (!has_integer(parsed.document.at("parameters"), "value") ||
                parsed.document.at("parameters").at("value").get<std::int64_t>() > 9999)) {
      invalidate(parsed, "parameters.value must be an integer between 0 and 9999");
    } else if (!has_integer(parsed.document, "issued_at_ms") ||
               !has_integer(parsed.document, "expires_at_ms") ||
               !has_integer(parsed.document, "attempt") ||
               parsed.document.at("attempt").get<std::int64_t>() < 1 ||
               parsed.document.at("attempt").get<std::int64_t>() > 3) {
      invalidate(parsed, "command time or attempt is invalid");
    } else if (parsed.document.at("expires_at_ms").get<std::int64_t>() <=
               parsed.document.at("issued_at_ms").get<std::int64_t>()) {
      invalidate(parsed, "expires_at_ms must be after issued_at_ms");
    }
  } else if (parsed.kind == "ack") {
    const auto command_id = string_field(parsed.document, "command_id");
    const auto target_device = string_field(parsed.document, "target_device_id");
    parsed.node_id = string_field(parsed.document, "node_id");
    const auto status = string_field(parsed.document, "status");
    const bool status_ok = status && (*status == "succeeded" || *status == "failed" ||
                                      *status == "rejected_expired" ||
                                      *status == "rejected_invalid");
    if (!command_id || !is_uuid(*command_id)) {
      invalidate(parsed, "command_id must be a UUID");
    } else if (!parsed.node_id || topic_node(topic, "/ack") != parsed.node_id) {
      invalidate(parsed, "topic node and ACK node_id differ");
    } else if (!valid_identifier(target_device)) {
      invalidate(parsed, "ACK target_device_id is invalid");
    } else if (!parsed.document.contains("ok") || !parsed.document.at("ok").is_boolean() ||
               !parsed.document.contains("duplicate") ||
               !parsed.document.at("duplicate").is_boolean()) {
      invalidate(parsed, "ACK ok and duplicate must be boolean");
    } else if (!status_ok || !has_integer(parsed.document, "executed_at_ms")) {
      invalidate(parsed, "ACK status or executed_at_ms is invalid");
    } else if ((parsed.document.contains("result") &&
                !parsed.document.at("result").is_null() &&
                !parsed.document.at("result").is_object()) ||
               (parsed.document.contains("error") &&
                !parsed.document.at("error").is_null() &&
                !parsed.document.at("error").is_object())) {
      invalidate(parsed, "ACK result and error must be objects or null");
    }
  } else if (parsed.kind == "heartbeat") {
    parsed.node_id = string_field(parsed.document, "node_id");
    const auto node_type = string_field(parsed.document, "node_type");
    const auto status = string_field(parsed.document, "status");
    const bool type_ok = node_type && (*node_type == "pi4" || *node_type == "pi5" ||
                                      *node_type == "esp32" || *node_type == "esp8266" ||
                                      *node_type == "octopus" || *node_type == "pc-test");
    const bool status_ok = status && (*status == "online" || *status == "offline" ||
                                      *status == "degraded");
    if (!parsed.node_id || parsed.node_id->empty() || parsed.node_id->size() > 64) {
      invalidate(parsed, "heartbeat node_id is invalid");
    } else if (!type_ok || !status_ok || !has_integer(parsed.document, "seq") ||
               !has_integer(parsed.document, "sent_at_ms") ||
               !has_integer(parsed.document, "uptime_ms")) {
      invalidate(parsed, "heartbeat fields are invalid");
    }
  } else if (parsed.kind == "telemetry") {
    parsed.node_id = string_field(parsed.document, "node_id");
    if (!parsed.node_id || topic_node(topic, "/telemetry") != parsed.node_id) {
      invalidate(parsed, "topic node and telemetry node_id differ");
    } else if (!has_integer(parsed.document, "seq") ||
               !has_integer(parsed.document, "sent_at_ms") ||
               !has_integer(parsed.document, "uptime_ms") ||
               !parsed.document.contains("sensor_ready") ||
               !parsed.document.at("sensor_ready").is_boolean()) {
      invalidate(parsed, "telemetry fields are invalid");
    } else if (!parsed.document.contains("readings") ||
               !parsed.document.at("readings").is_object() ||
               !parsed.document.contains("reading_sources") ||
               !parsed.document.at("reading_sources").is_object()) {
      invalidate(parsed, "readings and reading_sources must be objects");
    } else if (parsed.document.contains("simulated_value") &&
               !parsed.document.at("simulated_value").is_number()) {
      invalidate(parsed, "simulated_value must be numeric");
    } else if (sensor_rules) {
      const auto sensor_error = sensor_rules->validate(parsed.document);
      if (sensor_error) {
        invalidate(parsed, *sensor_error);
      }
    }
  } else if (parsed.kind == "perception") {
    parsed.node_id = string_field(parsed.document, "source_node_id");
    const auto observation_type = string_field(parsed.document, "observation_type");
    const auto zone = string_field(parsed.document, "zone");
    const auto source_device = string_field(parsed.document, "source_device_id");
    if (!valid_identifier(parsed.node_id) || !valid_identifier(observation_type) ||
        !valid_identifier(zone)) {
      invalidate(parsed, "perception identifiers are invalid");
    } else if (!has_number_between(parsed.document, "confidence", 0.0, 1.0) ||
               !has_integer(parsed.document, "observed_at_ms") ||
               !parsed.document.contains("attributes") ||
               !parsed.document.at("attributes").is_object()) {
      invalidate(parsed, "perception fields are invalid");
    } else if (!one_of(observation_type,
                       {"FIGURE_PRESENCE", "ACTIVITY_CONTEXT", "PARTIAL_INTENT_HINT"})) {
      invalidate(parsed, "unsupported observation_type");
    } else if (*observation_type != "ACTIVITY_CONTEXT" && !source_device) {
      invalidate(parsed, "source_device_id is required for this observation_type");
    } else if (*observation_type == "ACTIVITY_CONTEXT" &&
               (!parsed.document.at("attributes").contains("quiet_required") ||
                !parsed.document.at("attributes").at("quiet_required").is_boolean() ||
                !string_field(parsed.document.at("attributes"), "transition"))) {
      invalidate(parsed, "ACTIVITY_CONTEXT needs quiet_required and transition");
    } else if (registry && !registry->has_zone(*zone)) {
      invalidate(parsed, "zone is not a registry zone");
    } else if (registry && registry->node_role(*parsed.node_id) != "perception") {
      invalidate(parsed, "only the perception node may publish perception");
    } else if (registry && source_device &&
               registry->controller(*source_device) != parsed.node_id) {
      invalidate(parsed, "source_device_id is not attached to source_node_id");
    }
  } else if (parsed.kind == "task_request") {
    parsed.node_id = string_field(parsed.document, "source_node_id");
    const auto task_id = string_field(parsed.document, "task_id");
    const auto intent = string_field(parsed.document, "intent");
    const auto target_device = string_field(parsed.document, "target_device");
    const auto safety_level = string_field(parsed.document, "safety_level");
    const auto resume_policy = string_field(parsed.document, "resume_policy");
    const bool priority_ok = has_integer(parsed.document, "priority") &&
                             parsed.document.at("priority").get<std::int64_t>() <= 100;
    if (!task_id || !is_uuid(*task_id)) {
      invalidate(parsed, "task_id must be a UUID");
    } else if (!valid_identifier(parsed.node_id) || !valid_identifier(intent) ||
               !valid_identifier(target_device)) {
      invalidate(parsed, "task identifiers are invalid");
    } else if (!priority_ok ||
               !one_of(safety_level, {"normal", "caution", "critical"}) ||
               !one_of(resume_policy, {"never", "manual", "when_context_returns", "automatic"})) {
      invalidate(parsed, "task policy fields are invalid");
    } else if (!parsed.document.contains("interruptible") ||
               !parsed.document.at("interruptible").is_boolean() ||
               !has_number_between(parsed.document, "confidence", 0.0, 1.0) ||
               !parsed.document.contains("parameters") ||
               !parsed.document.at("parameters").is_object() ||
               !has_integer(parsed.document, "requested_at_ms")) {
      invalidate(parsed, "task request fields are invalid");
    } else if (parsed.document.at("parameters").contains("origin") &&
               !valid_identifier(string_field(parsed.document.at("parameters"), "origin"))) {
      invalidate(parsed, "parameters.origin must be an identifier");
    } else if (*intent == "device_control") {
      // Structure only (device-control.schema.json); registry and policy checks run in Database.
      const Json& control = parsed.document.at("parameters");
      const auto control_target = string_field(control, "target_device_id");
      const auto control_action = string_field(control, "action");
      const auto null_or_text = [&](const char* name) {
        return control.contains(name) && (control.at(name).is_null() || control.at(name).is_string());
      };
      if (!valid_identifier(control_target) ||
          !one_of(control_action,
                  {"set_switch", "set_color", "set_level", "set_position", "set_number"}) ||
          !control.contains("parameters") || !control.at("parameters").is_object() ||
          !one_of(string_field(control, "origin"), {"voice_parser", "llm", "ui"}) ||
          !null_or_text("ai_device") || !null_or_text("ai_action")) {
        invalidate(parsed, "device_control parameters are invalid");
      } else if (control_target != target_device) {
        invalidate(parsed, "target_device must equal parameters.target_device_id");
      }
    }
  } else if (parsed.kind == "state_request") {
    const auto request_id = string_field(parsed.document, "request_id");
    parsed.node_id = string_field(parsed.document, "client_id");
    if (!request_id || !is_uuid(*request_id) || !valid_identifier(parsed.node_id) ||
        !has_integer(parsed.document, "sent_at_ms")) {
      invalidate(parsed, "state request fields are invalid");
    }
  } else if (parsed.kind == "ui_action") {
    const auto task_id = string_field(parsed.document, "task_id");
    parsed.node_id = string_field(parsed.document, "client_id");
    const auto action = string_field(parsed.document, "action");
    if (!task_id || !is_uuid(*task_id) || !valid_identifier(parsed.node_id) ||
        !one_of(action, {"approve", "reject", "cancel", "resume"}) ||
        !has_integer(parsed.document, "expected_revision") ||
        parsed.document.at("expected_revision").get<std::int64_t>() < 1 ||
        !string_field(parsed.document, "action_token") ||
        string_field(parsed.document, "action_token")->size() < 16 ||
        string_field(parsed.document, "action_token")->size() > 256 ||
        !has_integer(parsed.document, "sent_at_ms")) {
      invalidate(parsed, "UI action fields are invalid");
    } else if (parsed.document.contains("reason") &&
               (!parsed.document.at("reason").is_string() ||
                parsed.document.at("reason").get<std::string>().size() > 256)) {
      invalidate(parsed, "UI action reason is invalid");
    }
  } else if (parsed.kind == "safety_alert") {
    parsed.node_id = string_field(parsed.document, "source_node_id");
    const auto alert_id = string_field(parsed.document, "alert_id");
    const auto alert_type = string_field(parsed.document, "alert_type");
    const auto zone = string_field(parsed.document, "zone");
    const auto severity = string_field(parsed.document, "severity");
    const auto summary = string_field(parsed.document, "summary");
    if (!alert_id || !is_uuid(*alert_id) || !valid_identifier(parsed.node_id) ||
        !one_of(alert_type, {"fire", "gas", "intrusion", "fall", "device_fault", "manual_test"}) ||
        !valid_identifier(zone) || !severity || *severity != "p0" || !summary ||
        summary->empty() || summary->size() > 256 || !has_integer(parsed.document, "raised_at_ms") ||
        !parsed.document.contains("evidence") || !parsed.document.at("evidence").is_object() ||
        !parsed.document.contains("automatic_actions") ||
        !parsed.document.at("automatic_actions").is_array()) {
      invalidate(parsed, "P0 safety alert fields are invalid");
    }
  } else if (parsed.kind == "safety_ack") {
    parsed.node_id = string_field(parsed.document, "client_id");
    const auto alert_id = string_field(parsed.document, "alert_id");
    if (!alert_id || !is_uuid(*alert_id) || !valid_identifier(parsed.node_id) ||
        !one_of(string_field(parsed.document, "action"), {"acknowledge", "dismiss_test"}) ||
        !has_integer(parsed.document, "sent_at_ms")) {
      invalidate(parsed, "safety acknowledgement fields are invalid");
    }
  } else if (parsed.kind == "privacy_set") {
    parsed.node_id = string_field(parsed.document, "client_id");
    if (!valid_identifier(parsed.node_id) ||
        !one_of(string_field(parsed.document, "mode"),
                {"normal", "camera_blocked", "audio_blocked", "private"}) ||
        !has_integer(parsed.document, "sent_at_ms")) {
      invalidate(parsed, "privacy request fields are invalid");
    }
  } else if (parsed.kind == "privacy_state") {
    parsed.node_id = string_field(parsed.document, "source_node_id");
    const auto mode = string_field(parsed.document, "mode");
    if (!valid_identifier(parsed.node_id) ||
        !one_of(mode,
                {"normal", "camera_blocked", "audio_blocked", "private"}) ||
        !parsed.document.contains("camera_allowed") ||
        !parsed.document.at("camera_allowed").is_boolean() ||
        !parsed.document.contains("microphone_allowed") ||
        !parsed.document.at("microphone_allowed").is_boolean() ||
        !parsed.document.contains("recording") || !parsed.document.at("recording").is_boolean() ||
        !parsed.document.contains("local_processing") ||
        !parsed.document.at("local_processing").is_boolean() ||
        !has_integer(parsed.document, "changed_at_ms")) {
      invalidate(parsed, "privacy state fields are invalid");
    } else {
      const bool expected_camera = *mode != "camera_blocked" && *mode != "private";
      const bool expected_microphone = *mode != "audio_blocked" && *mode != "private";
      if (parsed.document.at("camera_allowed").get<bool>() != expected_camera ||
          parsed.document.at("microphone_allowed").get<bool>() != expected_microphone) {
        invalidate(parsed, "privacy state permissions do not match mode");
      }
    }
  } else if (parsed.kind == "feedback") {
    parsed.node_id = string_field(parsed.document, "client_id");
    const bool task_id_ok = !parsed.document.contains("task_id") ||
                            parsed.document.at("task_id").is_null() ||
                            (parsed.document.at("task_id").is_string() &&
                             is_uuid(parsed.document.at("task_id").get<std::string>()));
    const bool observation_id_ok =
        !parsed.document.contains("observation_event_id") ||
        parsed.document.at("observation_event_id").is_null() ||
        (parsed.document.at("observation_event_id").is_string() &&
         is_uuid(parsed.document.at("observation_event_id").get<std::string>()));
    if (!valid_identifier(parsed.node_id) ||
        !task_id_ok || !observation_id_ok ||
        !one_of(string_field(parsed.document, "feedback_type"),
                {"context_feedback", "action_feedback"}) ||
        !one_of(string_field(parsed.document, "rating"), {"positive", "negative", "unknown"}) ||
        !has_integer(parsed.document, "sent_at_ms")) {
      invalidate(parsed, "feedback fields are invalid");
    } else if (parsed.document.contains("note") &&
               (!parsed.document.at("note").is_string() ||
                parsed.document.at("note").get<std::string>().size() > 512)) {
      invalidate(parsed, "feedback note is invalid");
    }
  } else if (parsed.kind == "system_health") {
    parsed.node_id = string_field(parsed.document, "source_node_id");
    const bool storage_ok =
        parsed.document.contains("storage") && parsed.document.at("storage").is_object() &&
        has_number_between(parsed.document.at("storage"), "used_percent", 0, 100) &&
        has_number_between(parsed.document.at("storage"), "warning_threshold_percent", 0, 100) &&
        parsed.document.at("storage").contains("warning") &&
        parsed.document.at("storage").at("warning").is_boolean() &&
        (!parsed.document.at("storage").contains("database_bytes") ||
         has_integer(parsed.document.at("storage"), "database_bytes"));
    if (!valid_identifier(parsed.node_id) ||
        !one_of(string_field(parsed.document, "overall_status"),
                {"healthy", "degraded", "offline"}) ||
        !parsed.document.contains("components") || !parsed.document.at("components").is_array() ||
        !storage_ok ||
        !has_integer(parsed.document, "generated_at_ms")) {
      invalidate(parsed, "system health fields are invalid");
    }
  }
  return parsed;
}

class Database {
 public:
  Database(const std::string& path, std::string boot_id, std::string core_node_id,
           const SensorRuleSet& sensor_rules, const ScenarioRuleSet& scenario_rules,
           const DeviceRegistry& registry, const TaskPolicy& policy,
           const ContextRuleSet& context_rules)
      : boot_id_(std::move(boot_id)),
        core_node_id_(std::move(core_node_id)),
        sensor_rules_(sensor_rules),
        scenario_rules_(scenario_rules),
        registry_(registry),
        policy_(policy),
        context_rules_(context_rules) {
    require_rules_match_registry();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &database_, flags, nullptr) != SQLITE_OK) {
      const std::string error = database_ ? sqlite3_errmsg(database_) : "open failed";
      if (database_) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      throw std::runtime_error("cannot open SQLite database " + path + ": " + error);
    }
    sqlite3_busy_timeout(database_, 5000);
    execute(kSchemaSql);
    migrate_schema();
    update_boot_session(read_clock());
  }

  ~Database() {
    if (database_) {
      sqlite3_close(database_);
    }
  }
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  std::vector<OutboundMessage> record(const std::string& topic, const std::string& payload,
                                      int qos, bool retained, const ClockReading& clock) {
    std::vector<OutboundMessage> outbound;
    if (private_ai_topic(topic)) {
      // Not parsed, not stored, not logged: see contracts/pfn.v1/protocol.json topic_policies.
      return outbound;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ParsedMessage parsed = parse_message(topic, payload, &sensor_rules_, &registry_);
    execute("BEGIN IMMEDIATE;");
    try {
      store_boot_session(clock);
      store_event(parsed, topic, payload, qos, retained, clock);
      if (parsed.valid) {
        if (parsed.kind == "command") {
          store_command(parsed, clock.wall_time_ms);
        } else if (parsed.kind == "ack") {
          const auto task = store_ack(parsed, clock.wall_time_ms);
          if (task) {
            outbound.push_back(make_task_update(*task, *parsed.trace_id, clock.wall_time_ms));
          }
        } else if (parsed.kind == "telemetry") {
          store_telemetry(parsed, clock);
        } else if (parsed.kind == "heartbeat") {
          store_heartbeat(parsed, clock);
        } else if (parsed.kind == "perception") {
          if (store_perception(parsed, clock.wall_time_ms)) {
            const auto context_messages = apply_context_rules(parsed.document, clock.wall_time_ms);
            outbound.insert(outbound.end(), context_messages.begin(), context_messages.end());
          }
        } else if (parsed.kind == "task_request") {
          const auto task_messages = accept_task_request(parsed.document, clock.wall_time_ms, false);
          outbound.insert(outbound.end(), task_messages.begin(), task_messages.end());
        } else if (parsed.kind == "ui_action") {
          const Json task = store_ui_action(parsed, clock.wall_time_ms);
          outbound.push_back(make_task_update(task, *parsed.trace_id, clock.wall_time_ms));
          const auto command_messages = start_commands_if_ready(task, clock.wall_time_ms);
          outbound.insert(outbound.end(), command_messages.begin(), command_messages.end());
        } else if (parsed.kind == "state_request") {
          outbound.push_back(make_state_response(parsed, clock.wall_time_ms));
        } else if (parsed.kind == "safety_alert") {
          store_safety_alert(parsed, clock.wall_time_ms);
        } else if (parsed.kind == "safety_ack") {
          store_safety_ack(parsed, clock.wall_time_ms);
        } else if (parsed.kind == "privacy_set") {
          outbound.push_back(store_privacy_set(parsed, clock.wall_time_ms));
        } else if (parsed.kind == "privacy_state") {
          store_privacy_state(parsed, clock.wall_time_ms);
        } else if (parsed.kind == "feedback") {
          store_feedback(parsed, clock.wall_time_ms);
          const auto override_messages = apply_feedback_overrides(parsed.document, clock.wall_time_ms);
          outbound.insert(outbound.end(), override_messages.begin(), override_messages.end());
        } else if (parsed.kind == "system_health") {
          store_system_health(parsed, clock.wall_time_ms);
        }
      }
      execute("COMMIT;");
    } catch (...) {
      try {
        execute("ROLLBACK;");
      } catch (...) {
      }
      throw;
    }

    if (!parsed.valid) {
      std::cerr << "INVALID topic=" << topic << " reason=" << parsed.error << std::endl;
    }
    return outbound;
  }

  void update_boot_session(const ClockReading& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_boot_session(clock);
  }

  const std::string& boot_id() const { return boot_id_; }

  std::vector<OutboundMessage> maintain_time_sensitive_state(
      const ClockReading& clock, std::int64_t offline_after_ms,
      std::int64_t acknowledgement_timeout_ms, std::int64_t maximum_attempts) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OutboundMessage> outbound;
    Statement statement(database_,
                        "UPDATE devices SET status='offline', updated_at_ms=? "
                        "WHERE status!='offline' AND ("
                        "last_seen_receiver_boot_id IS NULL OR "
                        "last_seen_receiver_boot_id!=? OR "
                        "last_seen_receiver_uptime_ms IS NULL OR "
                        "last_seen_receiver_uptime_ms<?);");
    statement.integer(1, clock.wall_time_ms);
    statement.text(2, boot_id_);
    statement.integer(3, clock.boot_uptime_ms - offline_after_ms);
    statement.run();

    Statement retry_rows(
        database_,
        "SELECT command_id,task_id,trace_id,target_node_id,target_device_id,action,"
        "parameters_json,issued_at_ms,expires_at_ms,latest_attempt FROM commands "
        "WHERE status='pending' ORDER BY rowid;");
    while (sqlite3_step(retry_rows.get()) == SQLITE_ROW) {
      const auto text_column = [&](int index) {
        const auto* value =
            reinterpret_cast<const char*>(sqlite3_column_text(retry_rows.get(), index));
        return value ? std::string(value) : std::string();
      };
      const std::string command_id = text_column(0);
      const std::string task_id = text_column(1);
      const std::string trace_id = text_column(2);
      const std::string target_node_id = text_column(3);
      const std::string target_device_id = text_column(4);
      const std::int64_t expires_at_ms = sqlite3_column_int64(retry_rows.get(), 8);
      const std::int64_t current_attempt = sqlite3_column_int64(retry_rows.get(), 9);
      auto deadline = retry_deadline_uptime_ms_
                          .try_emplace(command_id,
                                       clock.boot_uptime_ms + acknowledgement_timeout_ms)
                          .first;
      const bool timeout_due = clock.boot_uptime_ms >= deadline->second;
      const bool wall_expired = clock.time_valid && expires_at_ms <= clock.wall_time_ms;
      if (wall_expired || (current_attempt >= maximum_attempts && timeout_due)) {
        Statement expire(database_,
                         "UPDATE commands SET status='expired_no_ack',updated_at_ms=? "
                         "WHERE command_id=? AND status='pending';");
        expire.integer(1, clock.wall_time_ms);
        expire.text(2, command_id);
        expire.run();
        retry_deadline_uptime_ms_.erase(command_id);
        continue;
      }
      if (current_attempt >= maximum_attempts || !timeout_due) {
        continue;
      }
      Json parameters = Json::object();
      try {
        parameters = Json::parse(text_column(6));
      } catch (...) {
      }
      const std::int64_t next_attempt = current_attempt + 1;
      const Json command{{"schema_version", "pfn.v1"},
                         {"event_id", uuid_v4()},
                         {"trace_id", trace_id},
                         {"task_id", task_id.empty() ? Json(nullptr) : Json(task_id)},
                         {"command_id", command_id},
                         {"target_node_id", target_node_id},
                         {"target_device_id", target_device_id},
                         {"action", text_column(5)},
                         {"parameters", parameters},
                         {"issued_at_ms", sqlite3_column_int64(retry_rows.get(), 7)},
                         {"expires_at_ms", sqlite3_column_int64(retry_rows.get(), 8)},
                         {"attempt", next_attempt}};
      Statement mark_retry(database_,
                           "UPDATE commands SET latest_attempt=?,updated_at_ms=? "
                           "WHERE command_id=? AND status='pending' AND latest_attempt<?;");
      mark_retry.integer(1, next_attempt);
      mark_retry.integer(2, clock.wall_time_ms);
      mark_retry.text(3, command_id);
      mark_retry.integer(4, next_attempt);
      mark_retry.run();
      if (sqlite3_changes(database_) > 0) {
        retry_deadline_uptime_ms_[command_id] =
            clock.boot_uptime_ms + acknowledgement_timeout_ms;
        outbound.push_back(OutboundMessage{
            "pfn/v1/device/" + target_node_id + "/command", command, 1, false});
      }
    }

    Statement failed_tasks(
        database_,
        "SELECT DISTINCT t.task_id,t.trace_id FROM tasks t JOIN commands c "
        "ON c.task_id=t.task_id WHERE t.state='running' AND "
        "c.status NOT IN ('pending','succeeded');");
    std::vector<std::pair<std::string, std::string>> failures;
    while (sqlite3_step(failed_tasks.get()) == SQLITE_ROW) {
      const auto* task =
          reinterpret_cast<const char*>(sqlite3_column_text(failed_tasks.get(), 0));
      const auto* trace =
          reinterpret_cast<const char*>(sqlite3_column_text(failed_tasks.get(), 1));
      if (task && trace) {
        failures.emplace_back(task, trace);
      }
    }
    for (const auto& [task_id, trace_id] : failures) {
      Statement fail_task(
          database_,
          "UPDATE tasks SET state='failed',updated_at_ms=?,revision=revision+1,"
          "action_token=? WHERE task_id=? AND state='running';");
      fail_task.integer(1, clock.wall_time_ms);
      fail_task.text(2, uuid_v4());
      fail_task.text(3, task_id);
      fail_task.run();
      if (sqlite3_changes(database_) == 0) {
        continue;
      }
      Statement transition(
          database_,
          "INSERT INTO task_transitions(event_id,task_id,trace_id,from_state,to_state,"
          "reason,actor,changed_at_ms) VALUES(?,?,?,'running','failed',"
          "'command_ack_timeout',?,?);");
      transition.text(1, uuid_v4());
      transition.text(2, task_id);
      transition.text(3, trace_id);
      transition.text(4, core_node_id_);
      transition.integer(5, clock.wall_time_ms);
      transition.run();
      outbound.push_back(make_task_update(task_json(task_id), trace_id, clock.wall_time_ms));
    }

    // Waiting tasks past their proposal_ttl_seconds (task-policy.json policy_version 3), once a second.
    if (last_proposal_sweep_uptime_ms_ < 0 || clock.boot_uptime_ms - last_proposal_sweep_uptime_ms_ >= 1000) {
      last_proposal_sweep_uptime_ms_ = clock.boot_uptime_ms;
      Statement proposed(database_, "SELECT task_id FROM tasks WHERE state='proposed' ORDER BY rowid;");
      std::vector<std::string> waiting;
      while (sqlite3_step(proposed.get()) == SQLITE_ROW) {
        const auto* task = reinterpret_cast<const char*>(sqlite3_column_text(proposed.get(), 0));
        if (task) {
          waiting.emplace_back(task);
        }
      }
      for (auto seen = proposal_seen_uptime_ms_.begin(); seen != proposal_seen_uptime_ms_.end();) {
        seen = std::find(waiting.begin(), waiting.end(), seen->first) == waiting.end()
                   ? proposal_seen_uptime_ms_.erase(seen)
                   : std::next(seen);
      }
      for (const auto& task_id : waiting) {
        const Json task = task_json(task_id);
        if (proposal_expired(task, clock.wall_time_ms, clock.boot_uptime_ms)) {
          const Json closed = expire_proposal(task, clock.wall_time_ms);
          outbound.push_back(
              make_task_update(closed, closed.at("trace_id").get<std::string>(), clock.wall_time_ms));
        }
      }
    }
    return outbound;
  }

  StorageStatus update_storage_status(const std::string& database_path,
                                      int warning_threshold_percent,
                                      std::int64_t checked_at_ms) {
    const StorageStatus status = inspect_storage(database_path, warning_threshold_percent);
    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement(database_,
                        "INSERT INTO storage_status(singleton_id,checked_at_ms,database_bytes,"
                        "filesystem_total_bytes,filesystem_available_bytes,filesystem_used_percent,"
                        "warning_threshold_percent,warning_active) VALUES(1,?,?,?,?,?,?,?) "
                        "ON CONFLICT(singleton_id) DO UPDATE SET "
                        "checked_at_ms=excluded.checked_at_ms,database_bytes=excluded.database_bytes,"
                        "filesystem_total_bytes=excluded.filesystem_total_bytes,"
                        "filesystem_available_bytes=excluded.filesystem_available_bytes,"
                        "filesystem_used_percent=excluded.filesystem_used_percent,"
                        "warning_threshold_percent=excluded.warning_threshold_percent,"
                        "warning_active=excluded.warning_active;");
    statement.integer(1, checked_at_ms);
    statement.integer(2, static_cast<std::int64_t>(status.database_bytes));
    statement.integer(3, static_cast<std::int64_t>(status.filesystem_total_bytes));
    statement.integer(4, static_cast<std::int64_t>(status.filesystem_available_bytes));
    statement.real(5, status.filesystem_used_percent);
    statement.integer(6, status.warning_threshold_percent);
    statement.integer(7, status.warning_active ? 1 : 0);
    statement.run();
    return status;
  }

 private:
  void execute(const char* sql) {
    char* message = nullptr;
    if (sqlite3_exec(database_, sql, nullptr, nullptr, &message) != SQLITE_OK) {
      const std::string error = message ? message : sqlite3_errmsg(database_);
      sqlite3_free(message);
      throw std::runtime_error("sqlite exec: " + error);
    }
  }

  bool has_column(const char* table, const char* column) {
    const std::string sql = std::string("PRAGMA table_info(") + table + ");";
    Statement statement(database_, sql.c_str());
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
      const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
      if (name && std::string(name) == column) {
        return true;
      }
    }
    return false;
  }

  void add_column_if_missing(const char* table, const char* column, const char* definition) {
    if (!has_column(table, column)) {
      execute(definition);
    }
  }

  void migrate_schema() {
    // SQLite CREATE TABLE IF NOT EXISTS does not add fields to an existing database.
    add_column_if_missing("mqtt_events", "receiver_boot_id",
                          "ALTER TABLE mqtt_events ADD COLUMN receiver_boot_id TEXT;");
    add_column_if_missing("mqtt_events", "first_received_uptime_ms",
                          "ALTER TABLE mqtt_events ADD COLUMN first_received_uptime_ms INTEGER;");
    add_column_if_missing("mqtt_events", "last_received_uptime_ms",
                          "ALTER TABLE mqtt_events ADD COLUMN last_received_uptime_ms INTEGER;");
    add_column_if_missing(
        "mqtt_events", "first_receiver_time_valid",
        "ALTER TABLE mqtt_events ADD COLUMN first_receiver_time_valid INTEGER "
        "CHECK (first_receiver_time_valid IN (0, 1));");
    add_column_if_missing(
        "mqtt_events", "last_receiver_time_valid",
        "ALTER TABLE mqtt_events ADD COLUMN last_receiver_time_valid INTEGER "
        "CHECK (last_receiver_time_valid IN (0, 1));");
    add_column_if_missing(
        "devices", "last_seen_receiver_boot_id",
        "ALTER TABLE devices ADD COLUMN last_seen_receiver_boot_id TEXT;");
    add_column_if_missing(
        "devices", "last_seen_receiver_uptime_ms",
        "ALTER TABLE devices ADD COLUMN last_seen_receiver_uptime_ms INTEGER;");
    add_column_if_missing(
        "boot_sessions", "time_became_valid_uptime_ms",
        "ALTER TABLE boot_sessions ADD COLUMN time_became_valid_uptime_ms INTEGER;");
    add_column_if_missing("commands", "target_device_id",
                          "ALTER TABLE commands ADD COLUMN target_device_id TEXT;");
    add_column_if_missing("acknowledgements", "target_device_id",
                          "ALTER TABLE acknowledgements ADD COLUMN target_device_id TEXT;");
    add_column_if_missing("tasks", "action_token",
                          "ALTER TABLE tasks ADD COLUMN action_token TEXT;");
    add_column_if_missing("tasks", "rejection_code",
                          "ALTER TABLE tasks ADD COLUMN rejection_code TEXT;");
    add_column_if_missing("tasks", "rejection_message",
                          "ALTER TABLE tasks ADD COLUMN rejection_message TEXT;");
    execute("UPDATE commands SET target_device_id=target_node_id "
            "WHERE target_device_id IS NULL OR target_device_id='';");
    execute("UPDATE acknowledgements SET target_device_id=node_id "
            "WHERE target_device_id IS NULL OR target_device_id='';");
    execute("UPDATE tasks SET action_token=lower(hex(randomblob(16))) "
            "WHERE action_token IS NULL OR action_token='';");
    execute("CREATE INDEX IF NOT EXISTS idx_events_boot_uptime "
            "ON mqtt_events(receiver_boot_id, first_received_uptime_ms);");
    execute("CREATE INDEX IF NOT EXISTS idx_devices_receiver_uptime "
            "ON devices(last_seen_receiver_boot_id, last_seen_receiver_uptime_ms);");
    execute("INSERT INTO metadata(key,value) VALUES('schema_version','5') "
             "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
  }

  void store_boot_session(const ClockReading& clock) {
    Statement statement(
        database_,
        "INSERT INTO boot_sessions(boot_id,first_seen_at_ms,first_seen_uptime_ms,first_time_valid,"
        "last_seen_at_ms,last_seen_uptime_ms,last_time_valid,time_became_valid_at_ms,"
        "time_became_valid_uptime_ms) "
        "VALUES(?,?,?,?,?,?,?,CASE WHEN ?=1 THEN ? ELSE NULL END,"
        "CASE WHEN ?=1 THEN ? ELSE NULL END) "
        "ON CONFLICT(boot_id) DO UPDATE SET last_seen_at_ms=excluded.last_seen_at_ms,"
        "last_seen_uptime_ms=excluded.last_seen_uptime_ms,last_time_valid=excluded.last_time_valid,"
        "time_became_valid_at_ms=CASE WHEN boot_sessions.time_became_valid_at_ms IS NULL "
        "AND excluded.last_time_valid=1 THEN excluded.last_seen_at_ms "
        "ELSE boot_sessions.time_became_valid_at_ms END,"
        "time_became_valid_uptime_ms=CASE "
        "WHEN boot_sessions.time_became_valid_uptime_ms IS NULL "
        "AND excluded.last_time_valid=1 THEN excluded.last_seen_uptime_ms "
        "ELSE boot_sessions.time_became_valid_uptime_ms END;");
    statement.text(1, boot_id_);
    statement.integer(2, clock.wall_time_ms);
    statement.integer(3, clock.boot_uptime_ms);
    statement.integer(4, clock.time_valid ? 1 : 0);
    statement.integer(5, clock.wall_time_ms);
    statement.integer(6, clock.boot_uptime_ms);
    statement.integer(7, clock.time_valid ? 1 : 0);
    statement.integer(8, clock.time_valid ? 1 : 0);
    statement.integer(9, clock.wall_time_ms);
    statement.integer(10, clock.time_valid ? 1 : 0);
    statement.integer(11, clock.boot_uptime_ms);
    statement.run();
  }

  void store_event(const ParsedMessage& parsed, const std::string& topic,
                   const std::string& payload, int qos, bool retained,
                   const ClockReading& clock) {
    Statement statement(database_,
                        "INSERT INTO mqtt_events(event_id,topic,kind,schema_version,node_id,"
                        "trace_id,qos,retained,valid,validation_error,payload_json,"
                        "first_received_at_ms,last_received_at_ms,receiver_boot_id,"
                        "first_received_uptime_ms,last_received_uptime_ms,"
                        "first_receiver_time_valid,last_receiver_time_valid,delivery_count) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1) "
                        "ON CONFLICT(event_id) DO UPDATE SET "
                        "last_received_at_ms=excluded.last_received_at_ms, "
                        "last_received_uptime_ms=excluded.last_received_uptime_ms, "
                        "last_receiver_time_valid=excluded.last_receiver_time_valid, "
                        "delivery_count=mqtt_events.delivery_count+1;");
    statement.nullable_text(1, parsed.event_id);
    statement.text(2, topic);
    statement.text(3, parsed.kind);
    statement.nullable_text(4, parsed.schema_version);
    statement.nullable_text(5, parsed.node_id);
    statement.nullable_text(6, parsed.trace_id);
    statement.integer(7, qos);
    statement.integer(8, retained ? 1 : 0);
    statement.integer(9, parsed.valid ? 1 : 0);
    statement.nullable_text(10, parsed.valid ? std::nullopt
                                             : std::optional<std::string>(parsed.error));
    statement.text(11, payload);
    statement.integer(12, clock.wall_time_ms);
    statement.integer(13, clock.wall_time_ms);
    statement.text(14, boot_id_);
    statement.integer(15, clock.boot_uptime_ms);
    statement.integer(16, clock.boot_uptime_ms);
    statement.integer(17, clock.time_valid ? 1 : 0);
    statement.integer(18, clock.time_valid ? 1 : 0);
    statement.run();
  }

  void store_command(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    const std::string command_id = value.at("command_id").get<std::string>();
    const std::string trace_id = value.at("trace_id").get<std::string>();
    std::optional<std::string> task_id;
    if (value.contains("task_id") && value.at("task_id").is_string()) {
      task_id = value.at("task_id").get<std::string>();
    }
    Statement statement(database_,
                        "INSERT INTO commands(command_id,event_id,task_id,trace_id,target_node_id,"
                        "target_device_id,action,parameters_json,issued_at_ms,expires_at_ms,latest_attempt,status,"
                        "first_received_at_ms,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,"
                        "?,'pending',?,?) ON CONFLICT(command_id) DO UPDATE SET "
                        "event_id=excluded.event_id,latest_attempt=MAX(commands.latest_attempt,"
                        "excluded.latest_attempt),updated_at_ms=excluded.updated_at_ms "
                        "WHERE commands.trace_id=excluded.trace_id AND "
                        "commands.target_node_id=excluded.target_node_id AND "
                        "commands.target_device_id=excluded.target_device_id;");
    statement.text(1, value.at("command_id").get<std::string>());
    statement.text(2, value.at("event_id").get<std::string>());
    statement.nullable_text(3, task_id);
    statement.text(4, trace_id);
    statement.text(5, value.at("target_node_id").get<std::string>());
    statement.text(6, value.at("target_device_id").get<std::string>());
    statement.text(7, value.at("action").get<std::string>());
    statement.text(8, value.at("parameters").dump());
    statement.integer(9, value.at("issued_at_ms").get<std::int64_t>());
    statement.integer(10, value.at("expires_at_ms").get<std::int64_t>());
    statement.integer(11, value.at("attempt").get<std::int64_t>());
    statement.integer(12, received_at_ms);
    statement.integer(13, received_at_ms);
    statement.run();
    if (sqlite3_changes(database_) == 0) {
      Statement invalid(database_,
                        "UPDATE mqtt_events SET valid=0,validation_error="
                        "'command_id reused with a different trace or target' WHERE event_id=?;");
      invalid.text(1, value.at("event_id").get<std::string>());
      invalid.run();
      return;
    }

    Statement reconcile(database_,
                        "UPDATE commands SET status=COALESCE((SELECT status FROM acknowledgements "
                        "WHERE command_id=? AND trace_id=? AND target_device_id=? "
                        "ORDER BY rowid DESC LIMIT 1),"
                        "status),updated_at_ms=? WHERE command_id=?;");
    reconcile.text(1, command_id);
    reconcile.text(2, trace_id);
    reconcile.text(3, value.at("target_device_id").get<std::string>());
    reconcile.integer(4, received_at_ms);
    reconcile.text(5, command_id);
    reconcile.run();
  }

  std::optional<Json> store_ack(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    const std::optional<std::string> result = value.contains("result")
                                                  ? std::optional<std::string>(value.at("result").dump())
                                                  : std::nullopt;
    const std::optional<std::string> error = value.contains("error") && !value.at("error").is_null()
                                                 ? std::optional<std::string>(value.at("error").dump())
                                                 : std::nullopt;
    Statement statement(database_,
                        "INSERT OR IGNORE INTO acknowledgements(event_id,command_id,trace_id,node_id,"
                        "target_device_id,ok,duplicate,status,executed_at_ms,result_json,error_json,received_at_ms) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);");
    statement.text(1, value.at("event_id").get<std::string>());
    statement.text(2, value.at("command_id").get<std::string>());
    statement.text(3, value.at("trace_id").get<std::string>());
    statement.text(4, value.at("node_id").get<std::string>());
    statement.text(5, value.at("target_device_id").get<std::string>());
    statement.integer(6, value.at("ok").get<bool>() ? 1 : 0);
    statement.integer(7, value.at("duplicate").get<bool>() ? 1 : 0);
    statement.text(8, value.at("status").get<std::string>());
    statement.integer(9, value.at("executed_at_ms").get<std::int64_t>());
    statement.nullable_text(10, result);
    statement.nullable_text(11, error);
    statement.integer(12, received_at_ms);
    statement.run();

    Statement update(database_,
                     "UPDATE commands SET status=?,updated_at_ms=? WHERE command_id=? AND trace_id=? "
                     "AND target_node_id=? AND target_device_id=?;");
    update.text(1, value.at("status").get<std::string>());
    update.integer(2, received_at_ms);
    update.text(3, value.at("command_id").get<std::string>());
    update.text(4, value.at("trace_id").get<std::string>());
    update.text(5, value.at("node_id").get<std::string>());
    update.text(6, value.at("target_device_id").get<std::string>());
    update.run();
    if (sqlite3_changes(database_) > 0) {
      retry_deadline_uptime_ms_.erase(value.at("command_id").get<std::string>());
    }

    Statement task_lookup(database_,
                          "SELECT task_id FROM commands WHERE command_id=? AND task_id IS NOT NULL;");
    task_lookup.text(1, value.at("command_id").get<std::string>());
    if (sqlite3_step(task_lookup.get()) != SQLITE_ROW) {
      return std::nullopt;
    }
    const auto* raw_task_id =
        reinterpret_cast<const char*>(sqlite3_column_text(task_lookup.get(), 0));
    if (!raw_task_id) {
      return std::nullopt;
    }
    const std::string task_id(raw_task_id);
    const Json current_task = task_json(task_id);
    if (current_task.at("state") != "running") {
      return std::nullopt;
    }

    Statement counts(database_,
                     "SELECT COUNT(*),SUM(CASE WHEN status='succeeded' THEN 1 ELSE 0 END),"
                     "SUM(CASE WHEN status NOT IN ('pending','succeeded') THEN 1 ELSE 0 END) "
                     "FROM commands WHERE task_id=?;");
    counts.text(1, task_id);
    if (sqlite3_step(counts.get()) != SQLITE_ROW) {
      return std::nullopt;
    }
    const std::int64_t total = sqlite3_column_int64(counts.get(), 0);
    const std::int64_t succeeded = sqlite3_column_int64(counts.get(), 1);
    const std::int64_t failed = sqlite3_column_int64(counts.get(), 2);
    std::optional<std::string> next_state;
    if (failed > 0) {
      next_state = "failed";
    } else if (total > 0 && succeeded == total) {
      next_state = "completed";
    }
    if (!next_state) {
      return std::nullopt;
    }

    Statement task_update(database_,
                          "UPDATE tasks SET state=?,updated_at_ms=?,revision=revision+1,action_token=? "
                          "WHERE task_id=? AND state='running';");
    task_update.text(1, *next_state);
    task_update.integer(2, received_at_ms);
    task_update.text(3, uuid_v4());
    task_update.text(4, task_id);
    task_update.run();
    if (sqlite3_changes(database_) == 0) {
      return std::nullopt;
    }
    Statement transition(
        database_,
        "INSERT INTO task_transitions(event_id,task_id,trace_id,from_state,to_state,reason,"
        "actor,changed_at_ms) VALUES(?,?,?,'running',?,'command_ack',?,?);");
    transition.text(1, uuid_v4());
    transition.text(2, task_id);
    transition.text(3, value.at("trace_id").get<std::string>());
    transition.text(4, *next_state);
    transition.text(5, value.at("node_id").get<std::string>());
    transition.integer(6, received_at_ms);
    transition.run();
    return task_json(task_id);
  }

  void store_telemetry(const ParsedMessage& parsed, const ClockReading& clock) {
    const Json& value = parsed.document;
    const std::int64_t received_at_ms = clock.wall_time_ms;
    std::optional<double> simulated;
    if (value.contains("simulated_value") && value.at("simulated_value").is_number()) {
      simulated = value.at("simulated_value").get<double>();
    }
    std::optional<std::int64_t> led;
    if (value.contains("led_on") && value.at("led_on").is_boolean()) {
      led = value.at("led_on").get<bool>() ? 1 : 0;
    }
    std::optional<std::string> readings;
    if (value.contains("readings") && value.at("readings").is_object()) {
      readings = value.at("readings").dump();
    }
    Statement statement(database_,
                        "INSERT OR IGNORE INTO telemetry(event_id,trace_id,node_id,seq,sent_at_ms,"
                        "uptime_ms,sensor_ready,simulated_value,led_on,readings_json,received_at_ms) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?);");
    statement.text(1, value.at("event_id").get<std::string>());
    statement.text(2, value.at("trace_id").get<std::string>());
    statement.text(3, value.at("node_id").get<std::string>());
    statement.integer(4, value.at("seq").get<std::int64_t>());
    statement.integer(5, value.at("sent_at_ms").get<std::int64_t>());
    statement.integer(6, value.at("uptime_ms").get<std::int64_t>());
    statement.integer(7, value.at("sensor_ready").get<bool>() ? 1 : 0);
    statement.nullable_real(8, simulated);
    statement.nullable_integer(9, led);
    statement.nullable_text(10, readings);
    statement.integer(11, received_at_ms);
    statement.run();
    upsert_device(value.at("node_id").get<std::string>(), std::nullopt, "online",
                  value.at("event_id").get<std::string>(), value.at("seq").get<std::int64_t>(),
                  value.at("sent_at_ms").get<std::int64_t>(), clock, std::nullopt);
  }

  void store_heartbeat(const ParsedMessage& parsed, const ClockReading& clock) {
    const Json& value = parsed.document;
    const std::int64_t received_at_ms = clock.wall_time_ms;
    std::optional<std::string> version;
    if (value.contains("version") && value.at("version").is_string()) {
      version = value.at("version").get<std::string>();
    }
    Statement statement(database_,
                        "INSERT OR IGNORE INTO heartbeats(event_id,trace_id,node_id,node_type,status,"
                        "seq,sent_at_ms,uptime_ms,version,received_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?);");
    statement.text(1, value.at("event_id").get<std::string>());
    statement.text(2, value.at("trace_id").get<std::string>());
    statement.text(3, value.at("node_id").get<std::string>());
    statement.text(4, value.at("node_type").get<std::string>());
    statement.text(5, value.at("status").get<std::string>());
    statement.integer(6, value.at("seq").get<std::int64_t>());
    statement.integer(7, value.at("sent_at_ms").get<std::int64_t>());
    statement.integer(8, value.at("uptime_ms").get<std::int64_t>());
    statement.nullable_text(9, version);
    statement.integer(10, received_at_ms);
    statement.run();
    upsert_device(value.at("node_id").get<std::string>(),
                  value.at("node_type").get<std::string>(),
                  value.at("status").get<std::string>(),
                  value.at("event_id").get<std::string>(), value.at("seq").get<std::int64_t>(),
                  value.at("sent_at_ms").get<std::int64_t>(), clock, version);
  }

  Json task_json(const std::string& task_id) {
    Statement statement(
        database_,
        "SELECT task_id,trace_id,source_node_id,intent,target_device,priority,safety_level,state,"
        "interruptible,resume_policy,confidence,parameters_json,requested_at_ms,created_at_ms,"
        "updated_at_ms,revision,action_token,rejection_code,rejection_message "
        "FROM tasks WHERE task_id=?;");
    statement.text(1, task_id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
      throw std::runtime_error("task not found: " + task_id);
    }
    const auto text_column = [&](int index) {
      const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), index));
      return value ? std::string(value) : std::string();
    };
    Json parameters = Json::object();
    try {
      parameters = Json::parse(text_column(11));
    } catch (...) {
    }
    const std::string state = text_column(7);
    const std::string resume_policy = text_column(9);
    Json task{{"task_id", text_column(0)},
                {"trace_id", text_column(1)},
                {"source_node_id", text_column(2)},
                {"intent", text_column(3)},
                {"target_device", text_column(4)},
                {"priority", sqlite3_column_int64(statement.get(), 5)},
                {"safety_level", text_column(6)},
                {"state", state},
                {"interruptible", sqlite3_column_int(statement.get(), 8) != 0},
                {"resume_policy", resume_policy},
                {"confidence", sqlite3_column_double(statement.get(), 10)},
                {"parameters", parameters},
                {"requested_at_ms", sqlite3_column_int64(statement.get(), 12)},
                {"created_at_ms", sqlite3_column_int64(statement.get(), 13)},
                {"updated_at_ms", sqlite3_column_int64(statement.get(), 14)},
                {"revision", sqlite3_column_int64(statement.get(), 15)},
                {"action_token", text_column(16)},
                {"resume_allowed", state == "deferred" && resume_policy != "never"}};
    if (state == "rejected" && !text_column(17).empty()) {
      task["failure"] = Json{{"code", text_column(17)},
                             {"message", text_column(18).empty() ? text_column(17) : text_column(18)},
                             {"occurred_at_ms", sqlite3_column_int64(statement.get(), 14)},
                             {"retry_allowed", false}};
    }
    if (state == "failed") {
      Statement failure(
          database_,
          "SELECT status,updated_at_ms FROM commands WHERE task_id=? AND "
          "status NOT IN ('pending','succeeded') ORDER BY rowid DESC LIMIT 1;");
      failure.text(1, task_id);
      if (sqlite3_step(failure.get()) == SQLITE_ROW) {
        const auto* raw_status =
            reinterpret_cast<const char*>(sqlite3_column_text(failure.get(), 0));
        const std::string status = raw_status ? raw_status : "failed";
        const bool timed_out = status == "expired_no_ack";
        task["failure"] = Json{
            {"code", timed_out ? "ACK_TIMEOUT" : "COMMAND_FAILED"},
            {"message", timed_out ? "No ACK after three attempts"
                                  : "A device reported that the command failed"},
            {"occurred_at_ms", sqlite3_column_int64(failure.get(), 1)},
            {"retry_allowed", true}};
      }
    }
    return task;
  }

  // Returns false when the task_id already exists (MQTT redelivery).
  bool insert_task(const Json& value, std::int64_t received_at_ms) {
    Statement statement(
        database_,
        "INSERT OR IGNORE INTO tasks(task_id,request_event_id,trace_id,source_node_id,intent,"
        "target_device,priority,safety_level,state,interruptible,resume_policy,confidence,"
        "parameters_json,requested_at_ms,created_at_ms,updated_at_ms,revision,action_token) "
        "VALUES(?,?,?,?,?,?,?,?,'proposed',?,?,?,?,?,?,?,1,?);");
    statement.text(1, value.at("task_id").get<std::string>());
    statement.text(2, value.at("event_id").get<std::string>());
    statement.text(3, value.at("trace_id").get<std::string>());
    statement.text(4, value.at("source_node_id").get<std::string>());
    statement.text(5, value.at("intent").get<std::string>());
    statement.text(6, value.at("target_device").get<std::string>());
    statement.integer(7, value.at("priority").get<std::int64_t>());
    statement.text(8, value.at("safety_level").get<std::string>());
    statement.integer(9, value.at("interruptible").get<bool>() ? 1 : 0);
    statement.text(10, value.at("resume_policy").get<std::string>());
    statement.real(11, value.at("confidence").get<double>());
    statement.text(12, value.at("parameters").dump());
    statement.integer(13, value.at("requested_at_ms").get<std::int64_t>());
    statement.integer(14, received_at_ms);
    statement.integer(15, received_at_ms);
    statement.text(16, uuid_v4());
    statement.run();
    if (sqlite3_changes(database_) == 0) {
      return false;
    }

    Statement transition(
        database_,
        "INSERT OR IGNORE INTO task_transitions(event_id,task_id,trace_id,from_state,to_state,"
        "reason,actor,changed_at_ms) VALUES(?,?,?,NULL,'proposed','task_requested',?,?);");
    transition.text(1, value.at("event_id").get<std::string>());
    transition.text(2, value.at("task_id").get<std::string>());
    transition.text(3, value.at("trace_id").get<std::string>());
    transition.text(4, value.at("source_node_id").get<std::string>());
    transition.integer(5, received_at_ms);
    transition.run();
    return true;
  }

  void record_transition(const std::string& task_id, const std::string& trace_id,
                         const std::string& from_state, const std::string& to_state,
                         const std::string& reason, std::int64_t changed_at_ms) {
    Statement transition(
        database_,
        "INSERT INTO task_transitions(event_id,task_id,trace_id,from_state,to_state,reason,actor,"
        "changed_at_ms) VALUES(?,?,?,?,?,?,?,?);");
    transition.text(1, uuid_v4());
    transition.text(2, task_id);
    transition.text(3, trace_id);
    transition.text(4, from_state);
    transition.text(5, to_state);
    transition.text(6, reason);
    transition.text(7, core_node_id_);
    transition.integer(8, changed_at_ms);
    transition.run();
  }

  // Registry and policy decision for a newly stored task (contracts/pfn.v1/task-policy.json).
  // internal=true is a core-created context_rule task.
  std::vector<OutboundMessage> accept_task_request(const Json& value, std::int64_t received_at_ms,
                                                   bool internal) {
    std::vector<OutboundMessage> outbound;
    const std::string task_id = value.at("task_id").get<std::string>();
    const std::string trace_id = value.at("trace_id").get<std::string>();
    if (!insert_task(value, received_at_ms)) {
      outbound.push_back(make_task_update(task_json(task_id), trace_id, received_at_ms));
      return outbound;
    }
    const std::string intent = value.at("intent").get<std::string>();
    const Json& parameters = value.at("parameters");
    const std::string source_node_id = value.at("source_node_id").get<std::string>();
    const std::string origin =
        internal ? "context_rule" : parameters.value("origin", std::string("unspecified"));
    std::optional<Rejection> rejection;
    std::optional<std::string> device_id;
    std::optional<std::string> position_ref;
    if (!internal && !policy_.source_allowed(origin, source_node_id)) {
      rejection = Rejection{"SOURCE_NOT_ALLOWED",
                            "origin " + origin + " is not allowed from " + source_node_id};
    }
    if (!rejection && intent == "device_control") {
      device_id = parameters.at("target_device_id").get<std::string>();
      rejection = registry_.check_command(*device_id, parameters.at("action").get<std::string>(),
                                          parameters.at("parameters"));
      if (!rejection) {
        position_ref = registry_.position_ref(*device_id, parameters.at("parameters"));
      }
    }
    if (rejection) {
      Statement reject(database_,
                       "UPDATE tasks SET state='rejected',rejection_code=?,rejection_message=?,"
                       "updated_at_ms=?,revision=revision+1,action_token=? "
                       "WHERE task_id=? AND state='proposed';");
      reject.text(1, rejection->code);
      reject.text(2, rejection->message.substr(0, 512));
      reject.integer(3, received_at_ms);
      reject.text(4, uuid_v4());
      reject.text(5, task_id);
      reject.run();
      record_transition(task_id, trace_id, "proposed", "rejected", rejection->code, received_at_ms);
      outbound.push_back(make_task_update(task_json(task_id), trace_id, received_at_ms));
      return outbound;
    }
    const auto decision = policy_.decide(intent, origin, device_id, position_ref);
    if (decision.approval != "auto") {
      outbound.push_back(make_task_update(task_json(task_id), trace_id, received_at_ms));
      return outbound;
    }
    Statement approve(database_,
                      "UPDATE tasks SET state='ready',updated_at_ms=?,revision=revision+1,"
                      "action_token=? WHERE task_id=? AND state='proposed';");
    approve.integer(1, received_at_ms);
    approve.text(2, uuid_v4());
    approve.text(3, task_id);
    approve.run();
    record_transition(task_id, trace_id, "proposed", "ready", "policy_auto:" + decision.rule,
                      received_at_ms);
    const Json ready_task = task_json(task_id);
    outbound.push_back(make_task_update(ready_task, trace_id, received_at_ms));
    const auto command_messages = start_commands_if_ready(ready_task, received_at_ms);
    outbound.insert(outbound.end(), command_messages.begin(), command_messages.end());
    return outbound;
  }

  // contracts/pfn.v1/context-rules.json. Same intent of one group is not repeated back to back.
  std::vector<OutboundMessage> apply_context_rules(const Json& perception,
                                                   std::int64_t received_at_ms) {
    const auto match = context_rules_.match(perception);
    if (!match) {
      return {};
    }
    const auto suppressed = context_suppressed_.find(match->group);
    if (suppressed != context_suppressed_.end()) {
      if (match->name == suppressed->second.until_rule || received_at_ms >= suppressed->second.until_ms) {
        context_suppressed_.erase(suppressed);  // the disputed context ended (or the suppression ran out)
      } else {
        std::cout << "CONTEXT_RULE_SUPPRESSED rule=" << match->name << " by=" << suppressed->second.override_name
                  << std::endl;
        return {};
      }
    }
    auto& last_intent = last_context_intent_[match->group];
    if (last_intent == match->intent) {
      return {};
    }
    last_intent = match->intent;
    return accept_task_request(context_task_request(match->intent, match->name, perception.at("trace_id"),
                                                    perception.at("confidence"), perception.at("event_id"),
                                                    received_at_ms),
                               received_at_ms, true);
  }

  // feedback_overrides: "상황이 달라요" (negative context feedback) while the group's context intent is active
  // undoes it at once and keeps that context from re-applying it until it ends.
  std::vector<OutboundMessage> apply_feedback_overrides(const Json& feedback, std::int64_t received_at_ms) {
    std::vector<OutboundMessage> outbound;
    for (const auto& rule : context_rules_.feedback_overrides()) {
      if (string_field(feedback, "feedback_type") != rule.feedback_type ||
          string_field(feedback, "rating") != rule.rating) {
        continue;
      }
      auto& last_intent = last_context_intent_[rule.group];
      if (last_intent != rule.active_intent) {
        continue;  // nothing of this group is active: the feedback is only stored
      }
      last_intent = rule.intent;
      context_suppressed_[rule.group] =
          ContextSuppression{rule.suppress_until_rule, received_at_ms + rule.max_suppress_ms, rule.name};
      std::cout << "CONTEXT_FEEDBACK_OVERRIDE rule=" << rule.name << " intent=" << rule.intent
                << " suppress_until=" << rule.suppress_until_rule << std::endl;
      const Json observation = feedback.contains("observation_event_id") &&
                                       feedback.at("observation_event_id").is_string()
                                   ? feedback.at("observation_event_id")
                                   : Json(nullptr);
      const auto messages = accept_task_request(
          context_task_request(rule.intent, rule.name, feedback.at("trace_id"), 1.0, observation, received_at_ms),
          received_at_ms, true);
      outbound.insert(outbound.end(), messages.begin(), messages.end());
    }
    return outbound;
  }

  Json context_task_request(const std::string& intent, const std::string& rule, const Json& trace_id,
                            const Json& confidence, const Json& observation_event_id,
                            std::int64_t received_at_ms) const {
    Json parameters{{"origin", "context_rule"}, {"rule", rule}};
    if (observation_event_id.is_string()) {
      parameters["observation_event_id"] = observation_event_id;
    }
    return Json{{"schema_version", "pfn.v1"},
                {"event_id", uuid_v4()},
                {"trace_id", trace_id},
                {"task_id", uuid_v4()},
                {"source_node_id", core_node_id_},
                {"intent", intent},
                {"target_device", "HOME-ENV-01"},
                {"priority", 40},
                {"safety_level", "normal"},
                {"interruptible", true},
                {"resume_policy", "never"},
                {"confidence", confidence},
                {"parameters", parameters},
                {"requested_at_ms", received_at_ms}};
  }

  // Fail at startup instead of at the first command when contracts disagree.
  void require_rules_match_registry() const {
    for (const auto& scenario : scenario_rules_.scenarios()) {
      for (const auto& command : scenario.at("commands")) {
        const std::string device_id = command.value("target_device_id", "");
        Json parameters = command.value("parameters", Json::object());
        parameters.erase("auto_off_after_seconds");
        const auto rejection =
            registry_.check_command(device_id, command.value("action", ""), parameters);
        if (rejection) {
          throw std::runtime_error("scenario " + scenario.value("intent", "") + ": " +
                                   rejection->code + " " + rejection->message);
        }
        if (registry_.controller(device_id) != command.value("target_node_id", "")) {
          throw std::runtime_error("scenario " + scenario.value("intent", "") +
                                   ": target node does not control " + device_id);
        }
      }
    }
    for (const auto& rule : context_rules_.rules()) {
      if (!scenario_rules_.find(rule.at("intent").get<std::string>())) {
        throw std::runtime_error("context rule intent is not a scenario: " +
                                 rule.at("intent").get<std::string>());
      }
    }
  }

  // The task-policy rule accept_task_request chose for this task, asked again.
  TaskPolicy::Decision decision_for(const Json& task) const {
    const std::string intent = task.at("intent").get<std::string>();
    const Json& parameters = task.at("parameters");
    const std::string origin = parameters.value("origin", std::string("unspecified"));
    std::optional<std::string> device_id;
    std::optional<std::string> position_ref;
    if (intent == "device_control" && parameters.contains("target_device_id") &&
        parameters.at("target_device_id").is_string()) {
      device_id = parameters.at("target_device_id").get<std::string>();
      position_ref = registry_.position_ref(*device_id, parameters.value("parameters", Json::object()));
    }
    return policy_.decide(intent, origin, device_id, position_ref);
  }

  bool phone_may_approve(const Json& task) const { return decision_for(task).phone_may_approve; }

  // A proposed task expires when its age by the wall clock, or the time this run of core has seen it waiting,
  // reaches the proposal_ttl_seconds of its rule. The second bound keeps a backwards clock jump (the Pi4 has no RTC)
  // from keeping an old proposal approvable.
  bool proposal_expired(const Json& task, std::int64_t now_ms, std::int64_t now_uptime_ms) {
    if (task.at("state") != "proposed") {
      return false;
    }
    const std::int64_t ttl_ms = decision_for(task).proposal_ttl_ms;
    const auto seen =
        proposal_seen_uptime_ms_.try_emplace(task.at("task_id").get<std::string>(), now_uptime_ms).first;
    return now_ms - task.at("created_at_ms").get<std::int64_t>() >= ttl_ms ||
           now_uptime_ms - seen->second >= ttl_ms;
  }

  Json expire_proposal(const Json& task, std::int64_t now_ms) {
    const std::string task_id = task.at("task_id").get<std::string>();
    Statement expire(database_,
                     "UPDATE tasks SET state='rejected',rejection_code='PROPOSAL_EXPIRED',rejection_message=?,"
                     "updated_at_ms=?,revision=revision+1,action_token=? WHERE task_id=? AND state='proposed';");
    expire.text(1, "not approved within " + std::to_string(decision_for(task).proposal_ttl_ms / 1000) + " s");
    expire.integer(2, now_ms);
    expire.text(3, uuid_v4());
    expire.text(4, task_id);
    expire.run();
    if (sqlite3_changes(database_) > 0) {
      record_transition(task_id, task.at("trace_id").get<std::string>(), "proposed", "rejected",
                        "PROPOSAL_EXPIRED", now_ms);
      std::cout << "PROPOSAL_EXPIRED task_id=" << task_id << std::endl;
    }
    proposal_seen_uptime_ms_.erase(task_id);
    return task_json(task_id);
  }

  Json store_ui_action(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    const std::string event_id = value.at("event_id").get<std::string>();
    const std::string task_id = value.at("task_id").get<std::string>();
    const std::string action = value.at("action").get<std::string>();
    std::optional<std::string> next_state;
    std::optional<std::string> error;
    Json current_task = task_json(task_id);
    if (proposal_expired(current_task, received_at_ms, uptime_ms())) {
      // Arrived after the proposal's time: close it now (the sweep may not have run yet), approve nothing.
      current_task = expire_proposal(current_task, received_at_ms);
      error = "proposal_expired";
    }
    const std::string current_state = current_task.at("state").get<std::string>();
    const std::int64_t current_revision = current_task.at("revision").get<std::int64_t>();
    const std::string current_token = current_task.at("action_token").get<std::string>();
    const std::string resume_policy = current_task.at("resume_policy").get<std::string>();
    const std::int64_t expected_revision = value.at("expected_revision").get<std::int64_t>();
    const std::string action_token = value.at("action_token").get<std::string>();
    if (error) {
      // proposal_expired above
    } else if (expected_revision != current_revision) {
      error = "revision_conflict";
    } else if (action_token != current_token) {
      error = "invalid_action_token";
    } else if (parsed.from_phone && (action == "approve" || action == "resume") &&
               !phone_may_approve(current_task)) {
      // task-policy.json phone_may_approve false: opening the entrance door is approved in the house only
      error = "phone_approval_not_allowed";
      std::cout << "PHONE_APPROVAL_REFUSED task_id=" << task_id << " action=" << action << std::endl;
    } else if (action == "approve" && current_state == "proposed") {
      next_state = "ready";
    } else if (action == "reject" &&
               (current_state == "proposed" || current_state == "ready" ||
                current_state == "deferred")) {
      next_state = "rejected";
    } else if (action == "cancel" && current_state != "completed" &&
               current_state != "failed" && current_state != "rejected" &&
               current_state != "cancelled") {
      next_state = "cancelled";
    } else if (action == "resume" && current_state == "deferred" &&
               resume_policy != "never") {
      next_state = "ready";
    } else {
      error = "transition_not_allowed";
    }
    const std::optional<std::string> reason = string_field(value, "reason");

    Statement feedback(
        database_,
        "INSERT OR IGNORE INTO user_feedback(event_id,trace_id,task_id,client_id,action,reason,"
        "accepted,error_text,sent_at_ms,received_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?);");
    feedback.text(1, event_id);
    feedback.text(2, value.at("trace_id").get<std::string>());
    feedback.text(3, task_id);
    feedback.text(4, value.at("client_id").get<std::string>());
    feedback.text(5, action);
    feedback.nullable_text(6, reason);
    feedback.integer(7, next_state ? 1 : 0);
    feedback.nullable_text(8, error);
    feedback.integer(9, value.at("sent_at_ms").get<std::int64_t>());
    feedback.integer(10, received_at_ms);
    feedback.run();
    const bool first_delivery = sqlite3_changes(database_) > 0;

    if (next_state && first_delivery) {
      Statement update(database_,
                       "UPDATE tasks SET state=?,updated_at_ms=?,revision=revision+1,action_token=? "
                       "WHERE task_id=? AND state=? AND revision=? AND action_token=?;");
      update.text(1, *next_state);
      update.integer(2, received_at_ms);
      update.text(3, uuid_v4());
      update.text(4, task_id);
      update.text(5, current_state);
      update.integer(6, current_revision);
      update.text(7, current_token);
      update.run();
      if (sqlite3_changes(database_) > 0) {
        Statement transition(
            database_,
            "INSERT OR IGNORE INTO task_transitions(event_id,task_id,trace_id,from_state,to_state,"
            "reason,actor,changed_at_ms) VALUES(?,?,?,?,?,?,?,?);");
        transition.text(1, event_id);
        transition.text(2, task_id);
        transition.text(3, value.at("trace_id").get<std::string>());
        transition.text(4, current_state);
        transition.text(5, *next_state);
        transition.text(6, reason.value_or(action));
        transition.text(7, value.at("client_id").get<std::string>());
        transition.integer(8, received_at_ms);
        transition.run();
      }
    }
    return task_json(task_id);
  }

  // device_control: one command to the registry controller. Scenario intents: scenario-rules.json.
  std::vector<OutboundMessage> start_commands_if_ready(const Json& task,
                                                       std::int64_t issued_at_ms) {
    std::vector<OutboundMessage> outbound;
    if (task.at("state") != "ready") {
      return outbound;
    }
    const std::string intent = task.at("intent").get<std::string>();
    Json command_rules = Json::array();
    std::string started_reason = "scenario_commands_started";
    if (intent == "device_control") {
      const Json& control = task.at("parameters");
      const std::string device_id = control.value("target_device_id", "");
      const auto controller = registry_.controller(device_id);
      if (!controller || registry_.check_command(device_id, control.value("action", ""),
                                                 control.value("parameters", Json::object()))) {
        return outbound;
      }
      command_rules.push_back(Json{{"target_node_id", *controller},
                                   {"target_device_id", device_id},
                                   {"action", control.at("action")},
                                   {"parameters", control.at("parameters")}});
      started_reason = "device_control_command_started";
    } else if (const Json* scenario = scenario_rules_.find(intent)) {
      command_rules = scenario->at("commands");
    } else {
      return outbound;
    }
    const std::string task_id = task.at("task_id").get<std::string>();
    const std::string trace_id = task.at("trace_id").get<std::string>();
    const std::int64_t revision = task.at("revision").get<std::int64_t>();

    Statement update(database_,
                     "UPDATE tasks SET state='running',updated_at_ms=?,revision=revision+1,"
                     "action_token=? WHERE task_id=? AND state='ready' AND revision=?;");
    update.integer(1, issued_at_ms);
    update.text(2, uuid_v4());
    update.text(3, task_id);
    update.integer(4, revision);
    update.run();
    if (sqlite3_changes(database_) == 0) {
      return outbound;
    }

    Statement transition(
        database_,
        "INSERT INTO task_transitions(event_id,task_id,trace_id,from_state,to_state,reason,actor,"
        "changed_at_ms) VALUES(?,?,?,'ready','running',?,?,?);");
    transition.text(1, uuid_v4());
    transition.text(2, task_id);
    transition.text(3, trace_id);
    transition.text(4, started_reason);
    transition.text(5, core_node_id_);
    transition.integer(6, issued_at_ms);
    transition.run();

    const Json running_task = task_json(task_id);
    outbound.push_back(make_task_update(running_task, trace_id, issued_at_ms));
    for (const auto& command_rule : command_rules) {
      const std::string target_node_id =
          command_rule.at("target_node_id").get<std::string>();
      const Json command{{"schema_version", "pfn.v1"},
                         {"event_id", uuid_v4()},
                         {"trace_id", trace_id},
                         {"task_id", task_id},
                         {"command_id", uuid_v4()},
                         {"target_node_id", target_node_id},
                         {"target_device_id", command_rule.at("target_device_id")},
                         {"action", command_rule.at("action")},
                         {"parameters", command_rule.at("parameters")},
                         {"issued_at_ms", issued_at_ms},
                         {"expires_at_ms", issued_at_ms + 10000},
                         {"attempt", 1}};
      const std::string topic = "pfn/v1/device/" + target_node_id + "/command";
      ParsedMessage parsed_command = parse_message(topic, command.dump(), &sensor_rules_, &registry_);
      if (!parsed_command.valid) {
        throw std::runtime_error("invalid generated scenario command: " + parsed_command.error);
      }
      store_command(parsed_command, issued_at_ms);
      outbound.push_back(OutboundMessage{topic, command, 1, false});
    }
    return outbound;
  }

  void store_safety_alert(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    Statement statement(
        database_,
        "INSERT OR IGNORE INTO safety_alerts(alert_id,event_id,trace_id,source_node_id,"
        "alert_type,zone,severity,summary,evidence_json,automatic_actions_json,status,"
        "raised_at_ms,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,'active',?,?);");
    statement.text(1, value.at("alert_id").get<std::string>());
    statement.text(2, value.at("event_id").get<std::string>());
    statement.text(3, value.at("trace_id").get<std::string>());
    statement.text(4, value.at("source_node_id").get<std::string>());
    statement.text(5, value.at("alert_type").get<std::string>());
    statement.text(6, value.at("zone").get<std::string>());
    statement.text(7, value.at("severity").get<std::string>());
    statement.text(8, value.at("summary").get<std::string>());
    statement.text(9, value.at("evidence").dump());
    statement.text(10, value.at("automatic_actions").dump());
    statement.integer(11, value.at("raised_at_ms").get<std::int64_t>());
    statement.integer(12, received_at_ms);
    statement.run();
  }

  void store_safety_ack(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    Statement lookup(database_, "SELECT alert_type,status FROM safety_alerts WHERE alert_id=?;");
    lookup.text(1, value.at("alert_id").get<std::string>());
    const bool exists = sqlite3_step(lookup.get()) == SQLITE_ROW;
    std::string alert_type;
    std::string current_status;
    if (exists) {
      const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(lookup.get(), 0));
      const auto* status = reinterpret_cast<const char*>(sqlite3_column_text(lookup.get(), 1));
      alert_type = type ? type : "";
      current_status = status ? status : "";
    }
    const std::string action = value.at("action").get<std::string>();
    const bool action_allowed = exists && current_status == "active" &&
                                (action == "acknowledge" ||
                                 (action == "dismiss_test" && alert_type == "manual_test"));
    const std::optional<std::string> error =
        action_allowed ? std::nullopt
                       : std::optional<std::string>(exists ? "action_not_allowed"
                                                           : "alert_not_found");
    Statement acknowledgement(
        database_,
        "INSERT OR IGNORE INTO safety_acknowledgements(event_id,trace_id,alert_id,client_id,"
        "action,accepted,error_text,sent_at_ms,received_at_ms) VALUES(?,?,?,?,?,?,?,?,?);");
    acknowledgement.text(1, value.at("event_id").get<std::string>());
    acknowledgement.text(2, value.at("trace_id").get<std::string>());
    acknowledgement.text(3, value.at("alert_id").get<std::string>());
    acknowledgement.text(4, value.at("client_id").get<std::string>());
    acknowledgement.text(5, action);
    acknowledgement.integer(6, action_allowed ? 1 : 0);
    acknowledgement.nullable_text(7, error);
    acknowledgement.integer(8, value.at("sent_at_ms").get<std::int64_t>());
    acknowledgement.integer(9, received_at_ms);
    acknowledgement.run();
    const bool first_delivery = sqlite3_changes(database_) > 0;
    if (action_allowed && first_delivery) {
      Statement update(database_,
                       "UPDATE safety_alerts SET status='acknowledged',acknowledged_at_ms=?,"
                       "updated_at_ms=? WHERE alert_id=? AND status='active';");
      update.integer(1, received_at_ms);
      update.integer(2, received_at_ms);
      update.text(3, value.at("alert_id").get<std::string>());
      update.run();
    }
  }

  OutboundMessage store_privacy_set(const ParsedMessage& parsed,
                                    std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    const std::string mode = value.at("mode").get<std::string>();
    const bool camera_allowed = mode != "camera_blocked" && mode != "private";
    const bool microphone_allowed = mode != "audio_blocked" && mode != "private";
    const bool recording = false;
    const bool local_processing = true;
    Statement request(
        database_,
        "INSERT OR IGNORE INTO privacy_events(event_id,trace_id,source_id,event_kind,mode,"
        "camera_allowed,microphone_allowed,recording,local_processing,event_at_ms,received_at_ms) "
        "VALUES(?,?,?,'request',?,?,?,?,?,?,?);");
    request.text(1, value.at("event_id").get<std::string>());
    request.text(2, value.at("trace_id").get<std::string>());
    request.text(3, value.at("client_id").get<std::string>());
    request.text(4, mode);
    request.integer(5, camera_allowed ? 1 : 0);
    request.integer(6, microphone_allowed ? 1 : 0);
    request.integer(7, recording ? 1 : 0);
    request.integer(8, local_processing ? 1 : 0);
    request.integer(9, value.at("sent_at_ms").get<std::int64_t>());
    request.integer(10, received_at_ms);
    request.run();

    const Json state{{"schema_version", "pfn.v1"},
                     {"event_id", uuid_v4()},
                     {"trace_id", value.at("trace_id")},
                     {"source_node_id", core_node_id_},
                     {"mode", mode},
                     {"camera_allowed", camera_allowed},
                     {"microphone_allowed", microphone_allowed},
                     {"recording", recording},
                     {"local_processing", local_processing},
                     {"changed_at_ms", received_at_ms}};
    upsert_privacy_state(state, received_at_ms);
    return OutboundMessage{"pfn/v1/system/privacy/state", state, 1, false};
  }

  void upsert_privacy_state(const Json& value, std::int64_t received_at_ms) {
    Statement current(
        database_,
        "INSERT INTO privacy_current(singleton_id,event_id,trace_id,source_node_id,mode,"
        "camera_allowed,microphone_allowed,recording,local_processing,changed_at_ms,updated_at_ms) "
        "VALUES(1,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(singleton_id) DO UPDATE SET "
        "event_id=excluded.event_id,trace_id=excluded.trace_id,"
        "source_node_id=excluded.source_node_id,mode=excluded.mode,"
        "camera_allowed=excluded.camera_allowed,microphone_allowed=excluded.microphone_allowed,"
        "recording=excluded.recording,local_processing=excluded.local_processing,"
        "changed_at_ms=excluded.changed_at_ms,updated_at_ms=excluded.updated_at_ms "
        "WHERE excluded.changed_at_ms>=privacy_current.changed_at_ms;");
    current.text(1, value.at("event_id").get<std::string>());
    current.text(2, value.at("trace_id").get<std::string>());
    current.text(3, value.at("source_node_id").get<std::string>());
    current.text(4, value.at("mode").get<std::string>());
    current.integer(5, value.at("camera_allowed").get<bool>() ? 1 : 0);
    current.integer(6, value.at("microphone_allowed").get<bool>() ? 1 : 0);
    current.integer(7, value.at("recording").get<bool>() ? 1 : 0);
    current.integer(8, value.at("local_processing").get<bool>() ? 1 : 0);
    current.integer(9, value.at("changed_at_ms").get<std::int64_t>());
    current.integer(10, received_at_ms);
    current.run();
  }

  void store_privacy_state(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    Statement event(
        database_,
        "INSERT OR IGNORE INTO privacy_events(event_id,trace_id,source_id,event_kind,mode,"
        "camera_allowed,microphone_allowed,recording,local_processing,event_at_ms,received_at_ms) "
        "VALUES(?,?,?,'state',?,?,?,?,?,?,?);");
    event.text(1, value.at("event_id").get<std::string>());
    event.text(2, value.at("trace_id").get<std::string>());
    event.text(3, value.at("source_node_id").get<std::string>());
    event.text(4, value.at("mode").get<std::string>());
    event.integer(5, value.at("camera_allowed").get<bool>() ? 1 : 0);
    event.integer(6, value.at("microphone_allowed").get<bool>() ? 1 : 0);
    event.integer(7, value.at("recording").get<bool>() ? 1 : 0);
    event.integer(8, value.at("local_processing").get<bool>() ? 1 : 0);
    event.integer(9, value.at("changed_at_ms").get<std::int64_t>());
    event.integer(10, received_at_ms);
    event.run();
    upsert_privacy_state(value, received_at_ms);
  }

  void store_feedback(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    const auto optional_uuid = [&](const char* name) -> std::optional<std::string> {
      if (value.contains(name) && value.at(name).is_string()) {
        const std::string candidate = value.at(name).get<std::string>();
        if (is_uuid(candidate)) {
          return candidate;
        }
      }
      return std::nullopt;
    };
    Statement statement(
        database_,
        "INSERT OR IGNORE INTO feedback_events(event_id,trace_id,client_id,feedback_type,task_id,"
        "observation_event_id,rating,note,sent_at_ms,received_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?);");
    statement.text(1, value.at("event_id").get<std::string>());
    statement.text(2, value.at("trace_id").get<std::string>());
    statement.text(3, value.at("client_id").get<std::string>());
    statement.text(4, value.at("feedback_type").get<std::string>());
    statement.nullable_text(5, optional_uuid("task_id"));
    statement.nullable_text(6, optional_uuid("observation_event_id"));
    statement.text(7, value.at("rating").get<std::string>());
    statement.nullable_text(8, string_field(value, "note"));
    statement.integer(9, value.at("sent_at_ms").get<std::int64_t>());
    statement.integer(10, received_at_ms);
    statement.run();
  }

  void store_system_health(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    Statement statement(
        database_,
        "INSERT OR IGNORE INTO system_health_reports(event_id,trace_id,source_node_id,"
        "overall_status,components_json,storage_json,generated_at_ms,received_at_ms) "
        "VALUES(?,?,?,?,?,?,?,?);");
    statement.text(1, value.at("event_id").get<std::string>());
    statement.text(2, value.at("trace_id").get<std::string>());
    statement.text(3, value.at("source_node_id").get<std::string>());
    statement.text(4, value.at("overall_status").get<std::string>());
    statement.text(5, value.at("components").dump());
    statement.text(6, value.at("storage").dump());
    statement.integer(7, value.at("generated_at_ms").get<std::int64_t>());
    statement.integer(8, received_at_ms);
    statement.run();
  }

  bool store_perception(const ParsedMessage& parsed, std::int64_t received_at_ms) {
    const Json& value = parsed.document;
    Statement perception(
        database_,
        "INSERT OR IGNORE INTO perceptions(event_id,trace_id,source_node_id,observation_type,"
        "zone,confidence,attributes_json,observed_at_ms,received_at_ms) VALUES(?,?,?,?,?,?,?,?,?);");
    perception.text(1, value.at("event_id").get<std::string>());
    perception.text(2, value.at("trace_id").get<std::string>());
    perception.text(3, value.at("source_node_id").get<std::string>());
    perception.text(4, value.at("observation_type").get<std::string>());
    perception.text(5, value.at("zone").get<std::string>());
    perception.real(6, value.at("confidence").get<double>());
    perception.text(7, value.at("attributes").dump());
    perception.integer(8, value.at("observed_at_ms").get<std::int64_t>());
    perception.integer(9, received_at_ms);
    perception.run();
    if (sqlite3_changes(database_) == 0) {
      return false;
    }
    const std::string state_key = value.at("zone").get<std::string>() + "." +
                                  value.at("observation_type").get<std::string>();
    Statement state(
        database_,
        "INSERT INTO world_state(state_key,value_json,confidence,source_node_id,trace_id,"
        "observation_event_id,observed_at_ms,updated_at_ms) VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(state_key) DO UPDATE SET value_json=excluded.value_json,"
        "confidence=excluded.confidence,source_node_id=excluded.source_node_id,"
        "trace_id=excluded.trace_id,observation_event_id=excluded.observation_event_id,"
        "observed_at_ms=excluded.observed_at_ms,updated_at_ms=excluded.updated_at_ms "
        "WHERE excluded.observed_at_ms>=world_state.observed_at_ms;");
    state.text(1, state_key);
    state.text(2, value.at("attributes").dump());
    state.real(3, value.at("confidence").get<double>());
    state.text(4, value.at("source_node_id").get<std::string>());
    state.text(5, value.at("trace_id").get<std::string>());
    state.text(6, value.at("event_id").get<std::string>());
    state.integer(7, value.at("observed_at_ms").get<std::int64_t>());
    state.integer(8, received_at_ms);
    state.run();
    return true;
  }

  OutboundMessage make_task_update(const Json& task, const std::string& trace_id,
                                   std::int64_t published_at_ms) const {
    return OutboundMessage{
        "pfn/v1/core/task/update",
        Json{{"schema_version", "pfn.v1"},
             {"event_id", uuid_v4()},
             {"trace_id", trace_id},
             {"source_node_id", core_node_id_},
             {"published_at_ms", published_at_ms},
             {"task", task}},
        1,
        false};
  }

  OutboundMessage make_state_response(const ParsedMessage& parsed,
                                      std::int64_t generated_at_ms) {
    Json tasks = Json::array();
    Statement task_rows(
        database_,
        "SELECT task_id FROM tasks ORDER BY "
        "CASE WHEN state IN ('running','proposed','ready','deferred') THEN 0 ELSE 1 END,"
        "rowid DESC LIMIT 100;");
    while (sqlite3_step(task_rows.get()) == SQLITE_ROW) {
      const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(task_rows.get(), 0));
      if (id) {
        tasks.push_back(task_json(id));
      }
    }

    Json devices = Json::array();
    Statement device_rows(
        database_,
        "SELECT node_id,node_type,status,last_seen_at_ms,version FROM devices ORDER BY node_id;");
    while (sqlite3_step(device_rows.get()) == SQLITE_ROW) {
      const auto text_column = [&](int index) {
        const auto* value =
            reinterpret_cast<const char*>(sqlite3_column_text(device_rows.get(), index));
        return value ? Json(value) : Json(nullptr);
      };
      devices.push_back(Json{{"node_id", text_column(0)},
                             {"node_type", text_column(1)},
                             {"status", text_column(2)},
                             {"last_seen_at_ms", sqlite3_column_int64(device_rows.get(), 3)},
                             {"version", text_column(4)}});
    }

    Json world = Json::array();
    Statement world_rows(
        database_,
        "SELECT state_key,value_json,confidence,source_node_id,trace_id,observation_event_id,"
        "observed_at_ms,updated_at_ms FROM world_state ORDER BY state_key;");
    while (sqlite3_step(world_rows.get()) == SQLITE_ROW) {
      const auto text_column = [&](int index) {
        const auto* value =
            reinterpret_cast<const char*>(sqlite3_column_text(world_rows.get(), index));
        return value ? std::string(value) : std::string();
      };
      Json state_value = Json::object();
      try {
        state_value = Json::parse(text_column(1));
      } catch (...) {
      }
      world.push_back(Json{{"state_key", text_column(0)},
                           {"value", state_value},
                           {"confidence", sqlite3_column_double(world_rows.get(), 2)},
                           {"source_node_id", text_column(3)},
                           {"trace_id", text_column(4)},
                           {"observation_event_id", text_column(5)},
                           {"observed_at_ms", sqlite3_column_int64(world_rows.get(), 6)},
                           {"updated_at_ms", sqlite3_column_int64(world_rows.get(), 7)}});
    }

    Json privacy{{"configured", false}};
    Statement privacy_row(
        database_,
        "SELECT mode,camera_allowed,microphone_allowed,recording,local_processing,changed_at_ms "
        "FROM privacy_current WHERE singleton_id=1;");
    if (sqlite3_step(privacy_row.get()) == SQLITE_ROW) {
      const auto* mode = reinterpret_cast<const char*>(sqlite3_column_text(privacy_row.get(), 0));
      privacy = Json{{"configured", true},
                     {"mode", mode ? mode : "normal"},
                     {"camera_allowed", sqlite3_column_int(privacy_row.get(), 1) != 0},
                     {"microphone_allowed", sqlite3_column_int(privacy_row.get(), 2) != 0},
                     {"recording", sqlite3_column_int(privacy_row.get(), 3) != 0},
                     {"local_processing", sqlite3_column_int(privacy_row.get(), 4) != 0},
                     {"changed_at_ms", sqlite3_column_int64(privacy_row.get(), 5)}};
    }

    Json active_alerts = Json::array();
    Statement alert_rows(
        database_,
        "SELECT alert_id,alert_type,zone,summary,raised_at_ms FROM safety_alerts "
        "WHERE status='active' ORDER BY rowid DESC;");
    while (sqlite3_step(alert_rows.get()) == SQLITE_ROW) {
      const auto text_column = [&](int index) {
        const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(alert_rows.get(), index));
        return value ? std::string(value) : std::string();
      };
      active_alerts.push_back(Json{{"alert_id", text_column(0)},
                                   {"alert_type", text_column(1)},
                                   {"zone", text_column(2)},
                                   {"summary", text_column(3)},
                                   {"raised_at_ms", sqlite3_column_int64(alert_rows.get(), 4)}});
    }
    const Json safety{{"active_count", active_alerts.size()}, {"active_alerts", active_alerts}};

    Json system_health{{"available", false}};
    Statement health_row(
        database_,
        "SELECT overall_status,components_json,storage_json,generated_at_ms "
        "FROM system_health_reports ORDER BY rowid DESC LIMIT 1;");
    if (sqlite3_step(health_row.get()) == SQLITE_ROW) {
      const auto text_column = [&](int index) {
        const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(health_row.get(), index));
        return value ? std::string(value) : std::string();
      };
      Json components = Json::array();
      Json storage = Json::object();
      try {
        components = Json::parse(text_column(1));
        storage = Json::parse(text_column(2));
      } catch (...) {
      }
      system_health = Json{{"available", true},
                           {"overall_status", text_column(0)},
                           {"components", components},
                           {"storage", storage},
                           {"generated_at_ms", sqlite3_column_int64(health_row.get(), 3)}};
    }

    Json clock{{"valid", false},
               {"boot_id", boot_id_},
               {"uptime_ms", 0},
               {"became_valid_at_ms", nullptr}};
    Statement clock_row(
        database_,
        "SELECT last_time_valid,last_seen_uptime_ms,time_became_valid_at_ms "
        "FROM boot_sessions WHERE boot_id=? LIMIT 1;");
    clock_row.text(1, boot_id_);
    if (sqlite3_step(clock_row.get()) == SQLITE_ROW) {
      clock["valid"] = sqlite3_column_int(clock_row.get(), 0) != 0;
      clock["uptime_ms"] = sqlite3_column_int64(clock_row.get(), 1);
      if (sqlite3_column_type(clock_row.get(), 2) != SQLITE_NULL) {
        clock["became_valid_at_ms"] = sqlite3_column_int64(clock_row.get(), 2);
      }
    }

    const Json& request = parsed.document;
    return OutboundMessage{
        "pfn/v1/core/state/response/" + request.at("client_id").get<std::string>(),
        Json{{"schema_version", "pfn.v1"},
             {"event_id", uuid_v4()},
             {"trace_id", request.at("trace_id")},
             {"request_id", request.at("request_id")},
             {"source_node_id", core_node_id_},
             {"generated_at_ms", generated_at_ms},
             {"tasks", tasks},
             {"devices", devices},
             {"world_state", world},
             {"privacy", privacy},
             {"safety", safety},
             {"clock", clock},
             {"system_health", system_health}},
        1,
        false};
  }

  void upsert_device(const std::string& node_id, const std::optional<std::string>& node_type,
                     const std::string& status, const std::string& event_id, std::int64_t seq,
                     std::int64_t sent_at_ms, const ClockReading& clock,
                     const std::optional<std::string>& version) {
    Statement statement(database_,
                        "INSERT INTO devices(node_id,node_type,status,last_event_id,last_seq,"
                        "last_sent_at_ms,last_seen_at_ms,version,updated_at_ms,"
                        "last_seen_receiver_boot_id,last_seen_receiver_uptime_ms) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(node_id) DO UPDATE SET "
                        "node_type=COALESCE(excluded.node_type,devices.node_type),status=excluded.status,"
                        "last_event_id=excluded.last_event_id,last_seq=excluded.last_seq,"
                        "last_sent_at_ms=excluded.last_sent_at_ms,last_seen_at_ms=excluded.last_seen_at_ms,"
                        "version=COALESCE(excluded.version,devices.version),"
                        "updated_at_ms=excluded.updated_at_ms,"
                        "last_seen_receiver_boot_id=excluded.last_seen_receiver_boot_id,"
                        "last_seen_receiver_uptime_ms=excluded.last_seen_receiver_uptime_ms;");
    statement.text(1, node_id);
    statement.nullable_text(2, node_type);
    statement.text(3, status);
    statement.text(4, event_id);
    statement.integer(5, seq);
    statement.integer(6, sent_at_ms);
    statement.integer(7, clock.wall_time_ms);
    statement.nullable_text(8, version);
    statement.integer(9, clock.wall_time_ms);
    statement.text(10, boot_id_);
    statement.integer(11, clock.boot_uptime_ms);
    statement.run();
  }

  sqlite3* database_{nullptr};
  std::string boot_id_;
  std::string core_node_id_;
  const SensorRuleSet& sensor_rules_;
  const ScenarioRuleSet& scenario_rules_;
  const DeviceRegistry& registry_;
  const TaskPolicy& policy_;
  const ContextRuleSet& context_rules_;
  std::unordered_map<std::string, std::string> last_context_intent_;
  struct ContextSuppression {
    std::string until_rule;
    std::int64_t until_ms = 0;
    std::string override_name;
  };
  std::unordered_map<std::string, ContextSuppression> context_suppressed_;
  std::unordered_map<std::string, std::int64_t> retry_deadline_uptime_ms_;
  // Proposed task -> core uptime when this run of core first saw it waiting (proposal_expired).
  std::unordered_map<std::string, std::int64_t> proposal_seen_uptime_ms_;
  std::int64_t last_proposal_sweep_uptime_ms_ = -1;
  std::mutex mutex_;
};

struct Options {
  std::string broker_host{"10.10.10.2"};
  int broker_port{1883};
  std::string database_path{"/data/share/homemind/db/homemind.db"};
  std::string node_id{"NODE-RPI4-01"};
  std::string sensor_rules_path{"/data/share/homemind/config/sensor-rules.json"};
  std::string scenario_rules_path{"/data/share/homemind/config/scenario-rules.json"};
  std::string device_registry_path{"/data/share/homemind/config/device-registry.json"};
  std::string task_policy_path{"/data/share/homemind/config/task-policy.json"};
  std::string context_rules_path{"/data/share/homemind/config/context-rules.json"};
  int storage_warning_percent{70};
  std::optional<std::string> username;
  // Path only. The password itself never appears in argv, logs or the repository.
  std::optional<std::string> password_file;
};

std::string read_password_file(const std::string& path) {
  std::ifstream input(path);
  std::string value;
  if (!input || !std::getline(input, value)) {
    throw std::runtime_error("cannot read MQTT password file: " + path);
  }
  while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  if (value.empty()) {
    throw std::runtime_error("MQTT password file is empty: " + path);
  }
  return value;
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (index + 1 >= argc) {
      throw std::runtime_error("missing value for " + argument);
    }
    const std::string value = argv[++index];
    if (argument == "--broker-host") {
      options.broker_host = value;
    } else if (argument == "--broker-port") {
      options.broker_port = std::stoi(value);
    } else if (argument == "--database") {
      options.database_path = value;
    } else if (argument == "--node-id") {
      options.node_id = value;
    } else if (argument == "--sensor-rules") {
      options.sensor_rules_path = value;
    } else if (argument == "--scenario-rules") {
      options.scenario_rules_path = value;
    } else if (argument == "--device-registry") {
      options.device_registry_path = value;
    } else if (argument == "--task-policy") {
      options.task_policy_path = value;
    } else if (argument == "--context-rules") {
      options.context_rules_path = value;
    } else if (argument == "--storage-warning-percent") {
      options.storage_warning_percent = std::stoi(value);
    } else if (argument == "--username") {
      options.username = value;
    } else if (argument == "--password") {
      throw std::runtime_error("--password is not accepted; use --password-file <path>");
    } else if (argument == "--password-file") {
      options.password_file = value;
    } else {
      throw std::runtime_error("unknown option: " + argument);
    }
  }
  // Atlas images installed before the shared registry used this legacy service argument.
  // Normalize it here because some Atlas root filesystems keep /etc systemd units read-only.
  if (options.node_id == "homemind-pi4-core-01") {
    options.node_id = "NODE-RPI4-01";
  }
  if (options.node_id.empty() || options.node_id.size() > 64 || options.broker_port < 1 ||
      options.broker_port > 65535 || options.storage_warning_percent < 1 ||
      options.storage_warning_percent > 99) {
    throw std::runtime_error("invalid node ID, broker port, or storage warning percentage");
  }
  return options;
}

struct CoreContext {
  Database* database{nullptr};
  std::atomic<bool> connected{false};
};

void on_connect(mosquitto* client, void* userdata, int result) {
  auto& context = *static_cast<CoreContext*>(userdata);
  if (result != 0) {
    context.connected = false;
    std::cerr << "MQTT_CONNECT_FAILED code=" << result << std::endl;
    return;
  }
  context.connected = true;
  // contracts/pfn.v1/protocol.json core_subscriptions. Never pfn/v1/# : ai/llm/* must not reach core.
  static const char* const subscriptions[] = {
      "pfn/v1/device/+/command", "pfn/v1/device/+/ack",     "pfn/v1/device/+/telemetry",
      "pfn/v1/system/#",         "pfn/v1/core/task/request", "pfn/v1/core/state/request",
      "pfn/v1/ui/#",             "pfn/v1/ai/perception",     "pfn/v1/mobile/task/action"};
  for (const char* topic : subscriptions) {
    const int subscribe_result = mosquitto_subscribe(client, nullptr, topic, 1);
    if (subscribe_result != MOSQ_ERR_SUCCESS) {
      context.connected = false;
      std::cerr << "MQTT_SUBSCRIBE_FAILED topic=" << topic
                << " error=" << mosquitto_strerror(subscribe_result) << std::endl;
      return;
    }
  }
  std::cout << "MQTT_CONNECTED" << std::endl;
}

void on_disconnect(mosquitto*, void* userdata, int result) {
  auto& context = *static_cast<CoreContext*>(userdata);
  context.connected = false;
  std::cerr << "MQTT_DISCONNECTED code=" << result << std::endl;
}

void on_message(mosquitto* client, void* userdata, const mosquitto_message* message) {
  auto& context = *static_cast<CoreContext*>(userdata);
  try {
    if (!message || !message->topic || message->payloadlen < 0 ||
        message->payloadlen > 262144) {
      throw std::runtime_error("invalid MQTT message or payload too large");
    }
    std::string payload;
    if (message->payloadlen > 0) {
      const auto* begin = static_cast<const char*>(message->payload);
      payload.assign(begin, begin + message->payloadlen);
    }
    const auto outbound = context.database->record(
        message->topic, payload, message->qos, message->retain != 0, read_clock());
    for (const auto& item : outbound) {
      const std::string output_payload = item.document.dump();
      const int result = mosquitto_publish(
          client, nullptr, item.topic.c_str(), static_cast<int>(output_payload.size()),
          output_payload.data(), item.qos, item.retained);
      if (result != MOSQ_ERR_SUCCESS) {
        std::cerr << "PUBLISH_FAILED topic=" << item.topic
                  << " error=" << mosquitto_strerror(result) << std::endl;
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "STORE_FAILED error=" << error.what() << std::endl;
  }
}

std::atomic<bool> running{true};
void stop_handler(int) { running = false; }

Json make_heartbeat(const Options& options, const std::string& status, std::uint64_t sequence) {
  return Json{{"schema_version", "pfn.v1"},
              {"event_id", uuid_v4()},
              {"trace_id", uuid_v4()},
              {"node_id", options.node_id},
              {"node_type", "pi4"},
              {"status", status},
              {"seq", sequence},
              {"sent_at_ms", now_ms()},
              {"uptime_ms", uptime_ms()},
              {"version", "pi4-core-0.6.4"}};
}

void publish_heartbeat(mosquitto* client, const Options& options, const std::string& status,
                       std::uint64_t sequence) {
  const std::string payload = make_heartbeat(options, status, sequence).dump();
  const int result = mosquitto_publish(client, nullptr, "pfn/v1/system/heartbeat",
                                       static_cast<int>(payload.size()), payload.data(), 0, false);
  if (result != MOSQ_ERR_SUCCESS && result != MOSQ_ERR_NO_CONN) {
    std::cerr << "HEARTBEAT_FAILED error=" << mosquitto_strerror(result) << std::endl;
  }
}

Json make_system_health(const Options& options, const StorageStatus& storage,
                        std::int64_t generated_at_ms, bool mqtt_connected) {
  const std::string overall_status =
      (!mqtt_connected || storage.warning_active) ? "degraded" : "healthy";
  return Json{{"schema_version", "pfn.v1"},
              {"event_id", uuid_v4()},
              {"trace_id", uuid_v4()},
              {"source_node_id", options.node_id},
              {"overall_status", overall_status},
              {"components",
               Json::array({Json{{"component", "core"}, {"status", "online"}},
                            Json{{"component", "mqtt"},
                                 {"status", mqtt_connected ? "online" : "offline"}},
                            Json{{"component", "sqlite"}, {"status", "online"}}})},
              {"storage",
               Json{{"used_percent", storage.filesystem_used_percent},
                    {"warning_threshold_percent", storage.warning_threshold_percent},
                    {"warning", storage.warning_active},
                    {"database_bytes", storage.database_bytes}}},
              {"generated_at_ms", generated_at_ms}};
}

void publish_system_health(mosquitto* client, const Options& options,
                           const StorageStatus& storage, std::int64_t generated_at_ms,
                           bool mqtt_connected) {
  const std::string payload =
      make_system_health(options, storage, generated_at_ms, mqtt_connected).dump();
  const int result = mosquitto_publish(client, nullptr, "pfn/v1/system/health",
                                       static_cast<int>(payload.size()), payload.data(), 0, false);
  if (result != MOSQ_ERR_SUCCESS && result != MOSQ_ERR_NO_CONN) {
    std::cerr << "SYSTEM_HEALTH_FAILED error=" << mosquitto_strerror(result) << std::endl;
  }
}

int run(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  const SensorRuleSet sensor_rules(options.sensor_rules_path);
  const ScenarioRuleSet scenario_rules(options.scenario_rules_path);
  const DeviceRegistry registry(options.device_registry_path);
  const TaskPolicy policy(options.task_policy_path);
  const ContextRuleSet context_rules(options.context_rules_path);
  Database database(options.database_path, read_boot_id(), options.node_id, sensor_rules,
                    scenario_rules, registry, policy, context_rules);
  const std::optional<std::string> password =
      options.password_file ? std::optional<std::string>(read_password_file(*options.password_file))
                            : std::nullopt;
  CoreContext context{&database};

  require_mosquitto(mosquitto_lib_init(), "mosquitto_lib_init");
  const std::string client_id = options.node_id + "-store";
  std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)> client(
      mosquitto_new(client_id.c_str(), true, &context), mosquitto_destroy);
  if (!client) {
    throw std::runtime_error("cannot create MQTT client");
  }
  mosquitto_connect_callback_set(client.get(), on_connect);
  mosquitto_disconnect_callback_set(client.get(), on_disconnect);
  mosquitto_message_callback_set(client.get(), on_message);
  mosquitto_reconnect_delay_set(client.get(), 1, 30, true);
  if (options.username) {
    require_mosquitto(mosquitto_username_pw_set(client.get(), options.username->c_str(),
                                                password ? password->c_str() : nullptr),
                      "set MQTT credentials");
  }
  const std::string last_will = make_heartbeat(options, "offline", 0).dump();
  require_mosquitto(mosquitto_will_set(client.get(), "pfn/v1/system/heartbeat",
                                       static_cast<int>(last_will.size()), last_will.data(), 0, false),
                    "set last will");
  std::signal(SIGINT, stop_handler);
  std::signal(SIGTERM, stop_handler);
  while (running) {
    const int connect_result =
        mosquitto_connect(client.get(), options.broker_host.c_str(), options.broker_port, 30);
    if (connect_result == MOSQ_ERR_SUCCESS) {
      break;
    }
    std::cerr << "MQTT_WAITING error=" << mosquitto_strerror(connect_result) << std::endl;
    for (int count = 0; count < 10 && running; ++count) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (!running) {
    client.reset();
    mosquitto_lib_cleanup();
    return 0;
  }
  require_mosquitto(mosquitto_loop_start(client.get()), "start MQTT loop");
  for (int count = 0; count < 20 && running && !context.connected; ++count) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  std::cout << "CORE_STARTED database=" << options.database_path
            << " boot_id=" << database.boot_id() << std::endl;
  std::uint64_t heartbeat_sequence = 0;
  std::int64_t last_heartbeat_uptime_ms = -5000;
  std::int64_t last_storage_check_uptime_ms = -60000;
  std::int64_t last_storage_warning_log_uptime_ms = -3600000;
  bool previous_storage_warning = false;
  bool storage_status_initialized = false;
  bool health_published = false;
  std::optional<bool> previous_time_valid;
  while (running) {
    const ClockReading clock = read_clock();
    const std::int64_t current_time_ms = clock.wall_time_ms;
    database.update_boot_session(clock);
    if (!previous_time_valid || *previous_time_valid != clock.time_valid) {
      std::cout << "TIME_STATUS valid=" << (clock.time_valid ? "true" : "false")
                << " boot_id=" << database.boot_id()
                << " uptime_ms=" << clock.boot_uptime_ms << std::endl;
      previous_time_valid = clock.time_valid;
    }
    if (context.connected &&
        clock.boot_uptime_ms - last_heartbeat_uptime_ms >= 5000) {
      publish_heartbeat(client.get(), options, "online", heartbeat_sequence++);
      last_heartbeat_uptime_ms = clock.boot_uptime_ms;
    }
    const auto maintenance =
        database.maintain_time_sensitive_state(clock, 15000, 3000, 3);
    for (const auto& item : maintenance) {
      const std::string payload = item.document.dump();
      const int result = mosquitto_publish(
          client.get(), nullptr, item.topic.c_str(), static_cast<int>(payload.size()),
          payload.data(), item.qos, item.retained);
      if (result != MOSQ_ERR_SUCCESS) {
        std::cerr << "PUBLISH_FAILED topic=" << item.topic
                  << " error=" << mosquitto_strerror(result) << std::endl;
      }
    }
    if (clock.boot_uptime_ms - last_storage_check_uptime_ms >= 60000 ||
        (!health_published && context.connected)) {
      const StorageStatus storage =
          database.update_storage_status(options.database_path,
                                         options.storage_warning_percent, current_time_ms);
      if (context.connected) {
        publish_system_health(client.get(), options, storage, current_time_ms, true);
        health_published = true;
      }
      last_storage_check_uptime_ms = clock.boot_uptime_ms;
      const auto database_kib = storage.database_bytes / 1024;
      const auto available_mib = storage.filesystem_available_bytes / (1024 * 1024);
      if (storage.warning_active &&
          (!previous_storage_warning ||
           clock.boot_uptime_ms - last_storage_warning_log_uptime_ms >= 3600000)) {
        std::cerr << "STORAGE_WARNING used_percent=" << storage.filesystem_used_percent
                  << " threshold_percent=" << storage.warning_threshold_percent
                  << " database_kib=" << database_kib
                  << " available_mib=" << available_mib
                  << " automatic_delete=false" << std::endl;
        last_storage_warning_log_uptime_ms = clock.boot_uptime_ms;
      } else if (!storage.warning_active &&
                 (!storage_status_initialized || previous_storage_warning)) {
        std::cout << "STORAGE_OK used_percent=" << storage.filesystem_used_percent
                  << " threshold_percent=" << storage.warning_threshold_percent
                  << " database_kib=" << database_kib
                  << " available_mib=" << available_mib
                  << " automatic_delete=false" << std::endl;
      }
      previous_storage_warning = storage.warning_active;
      storage_status_initialized = true;
    }
    for (int count = 0; count < 10 && running; ++count) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (context.connected) {
    publish_heartbeat(client.get(), options, "offline", heartbeat_sequence);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  mosquitto_disconnect(client.get());
  mosquitto_loop_stop(client.get(), true);
  client.reset();
  mosquitto_lib_cleanup();
  std::cout << "CORE_STOPPED" << std::endl;
  return 0;
}

}  // namespace homemind

int main(int argc, char** argv) {
  try {
    return homemind::run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "FATAL " << error.what() << std::endl;
    return 1;
  }
}
