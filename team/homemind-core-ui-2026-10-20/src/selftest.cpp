#include "common.hpp"

#include <mosquitto.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace homemind {
using Json = nlohmann::json;

inline void require_mosquitto(int result, const std::string& operation) {
  if (result != MOSQ_ERR_SUCCESS) {
    throw std::runtime_error(operation + ": " + mosquitto_strerror(result));
  }
}

struct Options {
  std::string broker_host{"10.10.10.2"};
  int broker_port{1883};
  std::string database_path{"/data/share/homemind/db/homemind.db"};
  std::string node_id{"homemind-esp32-sim-01"};
};

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
    } else {
      throw std::runtime_error("unknown option: " + argument);
    }
  }
  return options;
}

void publish_json(mosquitto* client, const std::string& topic, const Json& value, int qos) {
  const std::string payload = value.dump();
  require_mosquitto(mosquitto_publish(client, nullptr, topic.c_str(),
                                      static_cast<int>(payload.size()), payload.data(), qos, false),
                    "publish " + topic);
}

struct SimulatorContext {
  std::string node_id;
  std::atomic<bool> ready{false};
  std::mutex mutex;
  std::unordered_set<std::string> executed_commands;
};

struct SenderContext {
  std::string node_id;
  std::atomic<bool> ready{false};
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<Json> acknowledgements;
  std::vector<Json> task_updates;
  std::vector<Json> state_responses;
  std::vector<Json> privacy_states;
};

void simulator_connect(mosquitto* client, void*, int result) {
  if (result != 0) {
    std::cerr << "SIM_CONNECT_FAILED code=" << result << std::endl;
    return;
  }
  const std::string topic = "pfn/v1/device/+/command";
  const int subscribe_result = mosquitto_subscribe(client, nullptr, topic.c_str(), 1);
  if (subscribe_result != MOSQ_ERR_SUCCESS) {
    std::cerr << "SIM_SUBSCRIBE_FAILED error=" << mosquitto_strerror(subscribe_result) << std::endl;
  }
}

void simulator_subscribed(mosquitto*, void* userdata, int, int, const int*) {
  static_cast<SimulatorContext*>(userdata)->ready = true;
}

void simulator_message(mosquitto* client, void* userdata, const mosquitto_message* message) {
  auto& context = *static_cast<SimulatorContext*>(userdata);
  try {
    if (!message || !message->payload || message->payloadlen <= 0) {
      return;
    }
    const auto* begin = static_cast<const char*>(message->payload);
    const Json command = Json::parse(begin, begin + message->payloadlen);
    const std::string command_id = command.at("command_id").get<std::string>();
    const std::string trace_id = command.at("trace_id").get<std::string>();
    const std::string target_node_id = command.at("target_node_id").get<std::string>();
    const std::string target_device_id = command.at("target_device_id").get<std::string>();
    bool duplicate = false;
    {
      std::lock_guard<std::mutex> lock(context.mutex);
      duplicate = !context.executed_commands.insert(command_id).second;
    }
    const Json acknowledgement{{"schema_version", "pfn.v1"},
                               {"event_id", uuid_v4()},
                               {"command_id", command_id},
                               {"trace_id", trace_id},
                               {"node_id", target_node_id},
                               {"target_device_id", target_device_id},
                               {"ok", true},
                               {"duplicate", duplicate},
                               {"status", "succeeded"},
                               {"executed_at_ms", now_ms()},
                               {"result", {{"simulated", true}, {"action", command.at("action")}}},
                               {"error", nullptr}};
    publish_json(client, "pfn/v1/device/" + target_node_id + "/ack", acknowledgement, 1);
  } catch (const std::exception& error) {
    std::cerr << "SIM_COMMAND_FAILED error=" << error.what() << std::endl;
  }
}

void sender_connect(mosquitto* client, void*, int result) {
  if (result != 0) {
    std::cerr << "SENDER_CONNECT_FAILED code=" << result << std::endl;
    return;
  }
  const int subscribe_result = mosquitto_subscribe(client, nullptr, "pfn/v1/#", 1);
  if (subscribe_result != MOSQ_ERR_SUCCESS) {
    std::cerr << "SENDER_SUBSCRIBE_FAILED error=" << mosquitto_strerror(subscribe_result)
              << std::endl;
  }
}

void sender_subscribed(mosquitto*, void* userdata, int, int, const int*) {
  static_cast<SenderContext*>(userdata)->ready = true;
}

void sender_message(mosquitto*, void* userdata, const mosquitto_message* message) {
  auto& context = *static_cast<SenderContext*>(userdata);
  try {
    if (!message || !message->payload || message->payloadlen <= 0) {
      return;
    }
    const auto* begin = static_cast<const char*>(message->payload);
    Json document = Json::parse(begin, begin + message->payloadlen);
    const std::string topic = message->topic ? message->topic : "";
    {
      std::lock_guard<std::mutex> lock(context.mutex);
      if (topic == "pfn/v1/device/" + context.node_id + "/ack") {
        context.acknowledgements.push_back(std::move(document));
      } else if (topic == "pfn/v1/core/task/update") {
        context.task_updates.push_back(std::move(document));
      } else if (topic.starts_with("pfn/v1/core/state/response/")) {
        context.state_responses.push_back(std::move(document));
      } else if (topic == "pfn/v1/system/privacy/state") {
        context.privacy_states.push_back(std::move(document));
      } else {
        return;
      }
    }
    context.condition.notify_all();
  } catch (const std::exception& error) {
    std::cerr << "ACK_PARSE_FAILED error=" << error.what() << std::endl;
  }
}

void wait_ready(const std::atomic<bool>& first, const std::atomic<bool>& second) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while ((!first || !second) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!first || !second) {
    throw std::runtime_error("MQTT subscription timeout");
  }
}

Json wait_ack(SenderContext& context, const std::string& command_id,
              std::size_t occurrence) {
  const auto matching_count = [&] {
    return static_cast<std::size_t>(std::count_if(
        context.acknowledgements.begin(), context.acknowledgements.end(),
        [&](const Json& acknowledgement) {
          return acknowledgement.value("command_id", "") == command_id;
        }));
  };
  std::unique_lock<std::mutex> lock(context.mutex);
  if (!context.condition.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return matching_count() >= occurrence; })) {
    throw std::runtime_error("ACK timeout");
  }
  std::size_t matched = 0;
  for (const auto& acknowledgement : context.acknowledgements) {
    if (acknowledgement.value("command_id", "") == command_id &&
        ++matched == occurrence) {
      return acknowledgement;
    }
  }
  throw std::runtime_error("matching ACK disappeared");
}

Json wait_task_update(SenderContext& context, std::size_t count) {
  std::unique_lock<std::mutex> lock(context.mutex);
  if (!context.condition.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return context.task_updates.size() >= count; })) {
    throw std::runtime_error("task update timeout");
  }
  return context.task_updates.at(count - 1);
}

Json wait_state_response(SenderContext& context, const std::string& request_id) {
  const auto matching_response = [&]() -> const Json* {
    const auto found = std::find_if(
        context.state_responses.begin(), context.state_responses.end(),
        [&](const Json& response) { return response.value("request_id", "") == request_id; });
    return found == context.state_responses.end() ? nullptr : &*found;
  };
  std::unique_lock<std::mutex> lock(context.mutex);
  if (!context.condition.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return matching_response() != nullptr; })) {
    throw std::runtime_error("state response timeout");
  }
  return *matching_response();
}

Json wait_privacy_state(SenderContext& context, std::size_t count) {
  std::unique_lock<std::mutex> lock(context.mutex);
  if (!context.condition.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return context.privacy_states.size() >= count; })) {
    throw std::runtime_error("privacy state timeout");
  }
  return context.privacy_states.at(count - 1);
}

std::int64_t scalar(sqlite3* database, const std::string& sql,
                    const std::vector<std::string>& parameters) {
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("query prepare: ") + sqlite3_errmsg(database));
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    sqlite3_bind_text(statement.get(), static_cast<int>(index + 1), parameters[index].c_str(), -1,
                      SQLITE_TRANSIENT);
  }
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error(std::string("query step: ") + sqlite3_errmsg(database));
  }
  return sqlite3_column_int64(statement.get(), 0);
}

void prepare_deferred_task(const std::string& database_path, const std::string& task_id,
                           const std::string& action_token) {
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(database_path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    throw std::runtime_error("cannot prepare deferred task");
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
  sqlite3_busy_timeout(database.get(), 5000);
  sqlite3_stmt* statement = nullptr;
  const char* sql = "UPDATE tasks SET state='deferred',revision=revision+1,action_token=? "
                    "WHERE task_id=? AND state='ready';";
  if (sqlite3_prepare_v2(database.get(), sql, -1, &statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error("cannot prepare task deferral update");
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> holder(statement, sqlite3_finalize);
  sqlite3_bind_text(statement, 1, action_token.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, task_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(statement) != SQLITE_DONE || sqlite3_changes(database.get()) != 1) {
    throw std::runtime_error("cannot place task into deferred state");
  }
}

Json device_control_request(const std::string& task_id, const std::string& source_node_id,
                            const std::string& origin, const std::string& device_id,
                            const std::string& action, const Json& parameters) {
  return Json{{"schema_version", "pfn.v1"},
              {"event_id", uuid_v4()},
              {"trace_id", uuid_v4()},
              {"task_id", task_id},
              {"source_node_id", source_node_id},
              {"intent", "device_control"},
              {"target_device", device_id},
              {"priority", 50},
              {"safety_level", "normal"},
              {"interruptible", true},
              {"resume_policy", "never"},
              {"confidence", 0.9},
              {"parameters",
               {{"target_device_id", device_id},
                {"action", action},
                {"parameters", parameters},
                {"origin", origin},
                {"ai_device", nullptr},
                {"ai_action", nullptr}}},
              {"requested_at_ms", now_ms()}};
}

void expect_task(const Json& update, const std::string& task_id, const std::string& state,
                 const std::string& label, const std::string& failure_code = "") {
  const Json& task = update.at("task");
  const bool failure_ok =
      failure_code.empty() ? !task.contains("failure")
                           : task.contains("failure") && task.at("failure").at("code") == failure_code &&
                                 task.at("failure").at("retry_allowed") == false;
  if (task.at("task_id") != task_id || task.at("state") != state || !failure_ok) {
    std::cerr << "TASK_DEBUG " << update.dump() << std::endl;
    throw std::runtime_error(label + ": expected " + state + " " + failure_code);
  }
  std::cout << "POLICY " << label << " state=" << state
            << (failure_code.empty() ? "" : " code=" + failure_code) << std::endl;
}

struct PolicyEvidence {
  std::string voice_task_id;
  std::string door_task_id;
  std::string llm_task_id;
  std::string phone_llm_task_id;
  std::string phone_door_approve_event_id;
  std::string ui_task_id;
  std::string mismatch_event_id;
  std::string foreign_perception_event_id;
  std::string transcript_marker;
};

void verify_database(const Options& options, const std::string& command_event_id,
                     const std::string& command_id, const std::string& telemetry_event_id,
                     const std::string& invalid_telemetry_event_id,
                     const std::string& heartbeat_event_id, const std::string& perception_event_id,
                     const std::string& task_id, const std::string& mapped_task_id,
                     const std::string& ui_action_event_id,
                     const std::string& safety_alert_id, const std::string& safety_ack_event_id,
                     const std::string& privacy_request_event_id,
                     const std::string& feedback_event_id,
                     const std::string& stale_action_event_id,
                     const PolicyEvidence& policy) {
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(options.database_path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    const std::string error = raw ? sqlite3_errmsg(raw) : "open failed";
    if (raw) {
      sqlite3_close(raw);
    }
    throw std::runtime_error("cannot inspect database: " + error);
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
  sqlite3_busy_timeout(database.get(), 5000);
  const auto check = [&](const std::string& label, const std::string& sql,
                         const std::vector<std::string>& parameters, std::int64_t minimum) {
    const std::int64_t value = scalar(database.get(), sql, parameters);
    std::cout << "CHECK " << label << " value=" << value << std::endl;
    if (value < minimum) {
      throw std::runtime_error("database check failed: " + label);
    }
  };
  check("command_redelivery_deduplicated",
        "SELECT delivery_count FROM mqtt_events WHERE event_id=?;", {command_event_id}, 2);
  check("command_succeeded",
        "SELECT COUNT(*) FROM commands WHERE command_id=? AND status='succeeded';", {command_id},
        1);
  check("two_ack_events",
        "SELECT COUNT(*) FROM acknowledgements WHERE command_id=?;", {command_id}, 2);
  check("duplicate_ack_recorded",
        "SELECT COUNT(*) FROM acknowledgements WHERE command_id=? AND duplicate=1;", {command_id},
        1);
  check("telemetry_recorded", "SELECT COUNT(*) FROM telemetry WHERE event_id=?;",
        {telemetry_event_id}, 1);
  check("out_of_range_telemetry_rejected",
        "SELECT COUNT(*) FROM mqtt_events WHERE event_id=? AND valid=0 AND "
        "validation_error='reading fan_level_percent is above maximum';",
        {invalid_telemetry_event_id}, 1);
  check("out_of_range_telemetry_not_normalized",
        "SELECT COUNT(*) FROM telemetry WHERE event_id=?;", {invalid_telemetry_event_id}, 0);
  check("telemetry_receive_clock_recorded",
        "SELECT COUNT(*) FROM mqtt_events WHERE event_id=? AND receiver_boot_id IS NOT NULL "
        "AND first_received_uptime_ms IS NOT NULL AND first_receiver_time_valid IN (0,1);",
        {telemetry_event_id}, 1);
  check("boot_session_recorded",
        "SELECT COUNT(*) FROM boot_sessions WHERE boot_id=(SELECT receiver_boot_id FROM "
        "mqtt_events WHERE event_id=?);",
        {telemetry_event_id}, 1);
  check("heartbeat_recorded", "SELECT COUNT(*) FROM heartbeats WHERE event_id=?;",
        {heartbeat_event_id}, 1);
  check("perception_recorded", "SELECT COUNT(*) FROM perceptions WHERE event_id=?;",
        {perception_event_id}, 1);
  check("world_state_updated",
        "SELECT COUNT(*) FROM world_state WHERE state_key='outdoor.FIGURE_PRESENCE' "
        "AND observation_event_id=?;",
        {perception_event_id}, 1);
  check("task_resumed", "SELECT COUNT(*) FROM tasks WHERE task_id=? AND state='ready' "
                         "AND revision=4;",
        {task_id}, 1);
  check("scenario_task_completed",
        "SELECT COUNT(*) FROM tasks WHERE task_id=? AND state='completed' AND revision=4;",
        {mapped_task_id}, 1);
  check("scenario_commands_succeeded",
        "SELECT COUNT(*) FROM commands WHERE task_id=? AND status='succeeded';",
        {mapped_task_id}, 2);
  check("task_transitions_recorded",
        "SELECT COUNT(*) FROM task_transitions WHERE task_id=?;", {task_id}, 3);
  check("user_feedback_recorded",
        "SELECT COUNT(*) FROM user_feedback WHERE event_id=? AND accepted=1;",
        {ui_action_event_id}, 1);
  check("target_device_command_recorded",
        "SELECT COUNT(*) FROM commands WHERE command_id=? AND target_device_id="
        "'FAN-01';", {command_id}, 1);
  check("target_device_ack_recorded",
        "SELECT COUNT(*) FROM acknowledgements WHERE command_id=? AND target_device_id="
        "'FAN-01';", {command_id}, 2);
  check("safety_alert_acknowledged",
        "SELECT COUNT(*) FROM safety_alerts WHERE alert_id=? AND status='acknowledged';",
        {safety_alert_id}, 1);
  check("safety_ack_recorded",
        "SELECT COUNT(*) FROM safety_acknowledgements WHERE event_id=? AND accepted=1;",
        {safety_ack_event_id}, 1);
  check("privacy_request_recorded",
        "SELECT COUNT(*) FROM privacy_events WHERE event_id=? AND event_kind='request';",
        {privacy_request_event_id}, 1);
  check("privacy_state_applied",
        "SELECT COUNT(*) FROM privacy_current WHERE singleton_id=1 AND mode='private' "
        "AND camera_allowed=0 AND microphone_allowed=0;", {}, 1);
  check("feedback_event_recorded",
        "SELECT COUNT(*) FROM feedback_events WHERE event_id=? AND rating='positive';",
        {feedback_event_id}, 1);
  check("system_health_recorded",
        "SELECT COUNT(*) FROM system_health_reports;", {}, 1);
  check("stale_revision_rejected",
        "SELECT COUNT(*) FROM user_feedback WHERE event_id=? AND accepted=0 "
        "AND error_text='revision_conflict';", {stale_action_event_id}, 1);
  check("invalid_json_preserved",
        "SELECT COUNT(*) FROM mqtt_events WHERE kind='invalid_json' AND payload_json=?;",
        {"{not-json"}, 1);

  const auto check_exact = [&](const std::string& label, const std::string& sql,
                               const std::vector<std::string>& parameters, std::int64_t expected) {
    const std::int64_t value = scalar(database.get(), sql, parameters);
    std::cout << "CHECK " << label << " value=" << value << std::endl;
    if (value != expected) {
      throw std::runtime_error("database check failed: " + label);
    }
  };
  check("voice_control_auto_approved",
        "SELECT COUNT(*) FROM task_transitions WHERE task_id=? AND to_state='ready' "
        "AND reason='policy_auto:direct_device_control';",
        {policy.voice_task_id}, 1);
  check("voice_control_command_to_registry_controller",
        "SELECT COUNT(*) FROM commands WHERE task_id=? AND target_node_id='CTRL-ESP8266-LED-01' "
        "AND target_device_id='LED-LIVING-01' AND status='succeeded';",
        {policy.voice_task_id}, 1);
  check_exact("entrance_door_waits_for_approval",
              "SELECT COUNT(*) FROM commands WHERE task_id=?;", {policy.door_task_id}, 0);
  check("llm_control_approved_by_ui",
        "SELECT COUNT(*) FROM task_transitions WHERE task_id=? AND to_state='ready' "
        "AND actor='homemind-ui-test-01';",
        {policy.llm_task_id}, 1);
  check("llm_control_approved_by_phone",
        "SELECT COUNT(*) FROM task_transitions WHERE task_id=? AND to_state='ready' "
        "AND actor='homemind-mobile-api';",
        {policy.phone_llm_task_id}, 1);
  check("phone_door_approval_refused",
        "SELECT COUNT(*) FROM user_feedback WHERE event_id=? AND accepted=0 "
        "AND error_text='phone_approval_not_allowed';",
        {policy.phone_door_approve_event_id}, 1);
  check("phone_door_rejection_accepted",
        "SELECT COUNT(*) FROM task_transitions WHERE task_id=? AND to_state='rejected' "
        "AND actor='homemind-mobile-api';",
        {policy.door_task_id}, 1);
  check("ui_control_completed",
        "SELECT COUNT(*) FROM tasks WHERE task_id=? AND state='completed';", {policy.ui_task_id}, 1);
  check_exact("rejected_tasks_send_no_command",
              "SELECT COUNT(*) FROM commands c JOIN tasks t ON t.task_id=c.task_id "
              "WHERE t.state='rejected';", {}, 0);
  check("rejection_codes_recorded",
        "SELECT COUNT(DISTINCT rejection_code) FROM tasks WHERE state='rejected' AND rejection_code IN "
        "('DEVICE_NOT_WIRED','CAPABILITY_NOT_ALLOWED','SOURCE_NOT_ALLOWED','POSITION_NOT_REGISTERED');",
        {}, 4);
  check("target_mismatch_invalid",
        "SELECT COUNT(*) FROM mqtt_events WHERE event_id=? AND valid=0 AND validation_error="
        "'target_device must equal parameters.target_device_id';",
        {policy.mismatch_event_id}, 1);
  check("foreign_perception_invalid",
        "SELECT COUNT(*) FROM mqtt_events WHERE event_id=? AND valid=0;",
        {policy.foreign_perception_event_id}, 1);
  // five ACTIVATED perceptions over two meetings (repeat, disputed, suppressed) -> one task per meeting
  check_exact("quiet_mode_context_task_once_per_meeting",
              "SELECT COUNT(*) FROM tasks WHERE intent='quiet_mode_on';", {}, 2);
  check_exact("feedback_quiet_off_once_no_second_on_release",
              "SELECT COUNT(*) FROM tasks WHERE intent='quiet_mode_off' "
              "AND json_extract(parameters_json,'$.rule')='quiet_context_wrong';", {}, 1);
  check_exact("quiet_mode_off_only_from_feedback",
              "SELECT COUNT(*) FROM tasks WHERE intent='quiet_mode_off';", {}, 1);
  check("context_feedback_stored_separately",
        "SELECT COUNT(*) FROM feedback_events WHERE feedback_type='context_feedback' AND rating='negative';", {}, 1);
  check("quiet_mode_commands_succeeded",
        "SELECT COUNT(*) FROM commands c JOIN tasks t ON t.task_id=c.task_id "
        "WHERE t.intent='quiet_mode_on' AND c.status='succeeded' AND c.action='set_color';",
        {}, 1);  // registry v4: quiet mode blinks the air conditioner indicator only
  check_exact("llm_topics_not_stored",
              "SELECT COUNT(*) FROM mqtt_events WHERE topic LIKE 'pfn/v1/ai/llm/%';", {}, 0);
  check_exact("transcript_not_stored_anywhere_in_events",
              "SELECT COUNT(*) FROM mqtt_events WHERE instr(payload_json, ?) > 0;",
              {policy.transcript_marker}, 0);
}

int run(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  require_mosquitto(mosquitto_lib_init(), "mosquitto_lib_init");

  SimulatorContext simulator_context;
  simulator_context.node_id = options.node_id;
  SenderContext sender_context;
  sender_context.node_id = options.node_id;

  std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)> simulator(
      mosquitto_new(("selftest-sim-" + uuid_v4()).c_str(), true, &simulator_context),
      mosquitto_destroy);
  std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)> sender(
      mosquitto_new(("selftest-send-" + uuid_v4()).c_str(), true, &sender_context),
      mosquitto_destroy);
  if (!simulator || !sender) {
    throw std::runtime_error("cannot create self-test MQTT clients");
  }

  mosquitto_connect_callback_set(simulator.get(), simulator_connect);
  mosquitto_subscribe_callback_set(simulator.get(), simulator_subscribed);
  mosquitto_message_callback_set(simulator.get(), simulator_message);
  mosquitto_connect_callback_set(sender.get(), sender_connect);
  mosquitto_subscribe_callback_set(sender.get(), sender_subscribed);
  mosquitto_message_callback_set(sender.get(), sender_message);
  require_mosquitto(mosquitto_connect_async(simulator.get(), options.broker_host.c_str(),
                                            options.broker_port, 30),
                    "connect simulator");
  require_mosquitto(mosquitto_connect_async(sender.get(), options.broker_host.c_str(),
                                            options.broker_port, 30),
                    "connect sender");
  require_mosquitto(mosquitto_loop_start(simulator.get()), "start simulator loop");
  require_mosquitto(mosquitto_loop_start(sender.get()), "start sender loop");
  wait_ready(simulator_context.ready, sender_context.ready);

  const std::string telemetry_event_id = uuid_v4();
  const std::string invalid_telemetry_event_id = uuid_v4();
  const std::string heartbeat_event_id = uuid_v4();
  const Json telemetry{{"schema_version", "pfn.v1"},
                       {"event_id", telemetry_event_id},
                       {"trace_id", uuid_v4()},
                       {"node_id", options.node_id},
                       {"seq", 1},
                       {"sent_at_ms", now_ms()},
                       {"uptime_ms", 1000},
                       {"sensor_ready", true},
                       {"simulated_value", 42},
                       {"led_on", false},
                       {"readings", {{"fan_level_percent", 42}}},
                       {"reading_sources", {{"fan_level_percent", "FAN-01"}}}};
  const Json invalid_telemetry{{"schema_version", "pfn.v1"},
                               {"event_id", invalid_telemetry_event_id},
                               {"trace_id", uuid_v4()},
                               {"node_id", options.node_id},
                               {"seq", 2},
                               {"sent_at_ms", now_ms()},
                               {"uptime_ms", 1100},
                               {"sensor_ready", true},
                               {"readings", {{"fan_level_percent", 150}}},
                               {"reading_sources", {{"fan_level_percent", "FAN-01"}}}};
  const Json heartbeat{{"schema_version", "pfn.v1"},
                       {"event_id", heartbeat_event_id},
                       {"trace_id", uuid_v4()},
                       {"node_id", options.node_id},
                       {"node_type", "esp32"},
                       {"status", "online"},
                       {"seq", 1},
                       {"sent_at_ms", now_ms()},
                       {"uptime_ms", 1000},
                       {"version", "selftest-0.1.0"}};
  publish_json(simulator.get(), "pfn/v1/device/" + options.node_id + "/telemetry", telemetry,
               0);
  publish_json(simulator.get(), "pfn/v1/device/" + options.node_id + "/telemetry",
               invalid_telemetry, 0);
  publish_json(simulator.get(), "pfn/v1/system/heartbeat", heartbeat, 0);

  const std::string scenario_trace_id = uuid_v4();
  const std::string perception_event_id = uuid_v4();
  const Json perception{{"schema_version", "pfn.v1"},
                        {"event_id", perception_event_id},
                        {"trace_id", scenario_trace_id},
                        {"source_node_id", "NODE-RPI5-02"},
                        {"source_device_id", "CAM-OUTDOOR-01"},
                        {"observation_type", "FIGURE_PRESENCE"},
                        {"zone", "outdoor"},
                        {"confidence", 0.96},
                        {"attributes",
                         {{"present", true}, {"figure_count", 1}, {"model_version", "selftest"}}},
                        {"observed_at_ms", now_ms()},
                        {"simulated", true}};
  publish_json(sender.get(), "pfn/v1/ai/perception", perception, 1);

  const std::string task_id = uuid_v4();
  const Json task_request{{"schema_version", "pfn.v1"},
                          {"event_id", uuid_v4()},
                          {"trace_id", scenario_trace_id},
                          {"task_id", task_id},
                          {"source_node_id", "homemind-pi5-ai-01"},
                          {"intent", "selftest_prepare_home"},
                          {"target_device", options.node_id},
                          {"priority", 50},
                          {"safety_level", "normal"},
                          {"interruptible", true},
                          {"resume_policy", "when_context_returns"},
                          {"confidence", 0.92},
                          {"parameters", {{"light_on", true}}},
                          {"requested_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/core/task/request", task_request, 1);
  const Json proposed_update = wait_task_update(sender_context, 1);
  if (proposed_update.at("task").at("task_id") != task_id ||
      proposed_update.at("task").at("state") != "proposed") {
    throw std::runtime_error("proposed task update is incorrect");
  }

  const std::string ui_action_event_id = uuid_v4();
  const Json ui_action{{"schema_version", "pfn.v1"},
                       {"event_id", ui_action_event_id},
                       {"trace_id", scenario_trace_id},
                       {"task_id", task_id},
                       {"client_id", "homemind-ui-test-01"},
                       {"action", "approve"},
                       {"expected_revision", proposed_update.at("task").at("revision")},
                       {"action_token", proposed_update.at("task").at("action_token")},
                       {"reason", "selftest approval"},
                       {"sent_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/ui/action", ui_action, 1);
  const Json ready_update = wait_task_update(sender_context, 2);
  if (ready_update.at("task").at("task_id") != task_id ||
      ready_update.at("task").at("state") != "ready" ||
      ready_update.at("task").at("revision") != 2) {
    throw std::runtime_error("approved task update is incorrect");
  }

  const std::string resume_token = uuid_v4();
  prepare_deferred_task(options.database_path, task_id, resume_token);
  const Json resume_action{{"schema_version", "pfn.v1"},
                           {"event_id", uuid_v4()},
                           {"trace_id", scenario_trace_id},
                           {"task_id", task_id},
                           {"client_id", "homemind-ui-test-01"},
                           {"action", "resume"},
                           {"expected_revision", 3},
                           {"action_token", resume_token},
                           {"reason", "selftest resume"},
                           {"sent_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/ui/action", resume_action, 1);
  const Json resumed_update = wait_task_update(sender_context, 3);
  if (resumed_update.at("task").at("state") != "ready" ||
      resumed_update.at("task").at("revision") != 4) {
    throw std::runtime_error("resumed task update is incorrect");
  }

  const std::string stale_action_event_id = uuid_v4();
  const Json stale_action{{"schema_version", "pfn.v1"},
                          {"event_id", stale_action_event_id},
                          {"trace_id", scenario_trace_id},
                          {"task_id", task_id},
                          {"client_id", "homemind-ui-test-01"},
                          {"action", "cancel"},
                          {"expected_revision", 2},
                          {"action_token", ready_update.at("task").at("action_token")},
                          {"reason", "stale selftest action"},
                          {"sent_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/ui/action", stale_action, 1);
  const Json unchanged_update = wait_task_update(sender_context, 4);
  if (unchanged_update.at("task").at("revision") != 4 ||
      unchanged_update.at("task").at("state") != "ready") {
    throw std::runtime_error("stale action changed the task");
  }

  const std::string mapped_trace_id = uuid_v4();
  const std::string mapped_task_id = uuid_v4();
  const Json mapped_task_request{{"schema_version", "pfn.v1"},
                                 {"event_id", uuid_v4()},
                                 {"trace_id", mapped_trace_id},
                                 {"task_id", mapped_task_id},
                                 {"source_node_id", "NODE-RPI5-01"},
                                 {"intent", "prepare_home"},
                                 {"target_device", "HOME-ENV-01"},
                                 {"priority", 40},
                                 {"safety_level", "normal"},
                                 {"interruptible", true},
                                 {"resume_policy", "automatic"},
                                 {"confidence", 0.95},
                                 {"parameters", Json::object()},
                                 {"requested_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/core/task/request", mapped_task_request, 1);
  const Json mapped_proposed = wait_task_update(sender_context, 5);
  if (mapped_proposed.at("task").at("task_id") != mapped_task_id ||
      mapped_proposed.at("task").at("state") != "proposed") {
    throw std::runtime_error("mapped scenario proposal is incorrect");
  }
  const Json mapped_approval{{"schema_version", "pfn.v1"},
                             {"event_id", uuid_v4()},
                             {"trace_id", mapped_trace_id},
                             {"task_id", mapped_task_id},
                             {"client_id", "homemind-ui-test-01"},
                             {"action", "approve"},
                             {"expected_revision", mapped_proposed.at("task").at("revision")},
                             {"action_token", mapped_proposed.at("task").at("action_token")},
                             {"reason", "scenario mapping selftest"},
                             {"sent_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/ui/action", mapped_approval, 1);
  const Json mapped_ready = wait_task_update(sender_context, 6);
  const Json mapped_running = wait_task_update(sender_context, 7);
  const Json mapped_completed = wait_task_update(sender_context, 8);
  if (mapped_ready.at("task").at("state") != "ready" ||
      mapped_running.at("task").at("state") != "running" ||
      mapped_completed.at("task").at("state") != "completed" ||
      mapped_completed.at("task").at("task_id") != mapped_task_id) {
    throw std::runtime_error("mapped scenario command flow is incorrect");
  }

  // P3 policy paths: contracts/pfn.v1/task-policy.json, device-registry.json, context-rules.json
  PolicyEvidence policy;
  std::size_t updates = 8;
  const auto send_request = [&](const Json& request) {
    publish_json(sender.get(), "pfn/v1/core/task/request", request, 1);
  };
  const auto next_update = [&]() { return wait_task_update(sender_context, ++updates); };

  policy.voice_task_id = uuid_v4();
  send_request(device_control_request(policy.voice_task_id, "NODE-RPI5-02", "voice_parser",
                                      "LED-LIVING-01", "set_switch", {{"on", true}}));
  expect_task(next_update(), policy.voice_task_id, "ready", "voice_light_auto");
  expect_task(next_update(), policy.voice_task_id, "running", "voice_light_running");
  expect_task(next_update(), policy.voice_task_id, "completed", "voice_light_ack");

  policy.door_task_id = uuid_v4();
  send_request(device_control_request(policy.door_task_id, "NODE-RPI5-02", "voice_parser",
                                      "SERVO-DOOR-01", "set_position", {{"position_degrees", 90}}));
  const Json door_proposed = next_update();
  expect_task(door_proposed, policy.door_task_id, "proposed", "entrance_door_open_needs_ui");

  // 0.6.3 phone approvals (pfn/v1/mobile/task/action, written only by homemind-mobile-api): the phone may reject the
  // entrance door but not open it (task-policy.json phone_may_approve false).
  const auto phone_action = [&](const Json& update, const std::string& action) {
    const Json document{{"schema_version", "pfn.v1"},
                        {"event_id", uuid_v4()},
                        {"trace_id", update.at("trace_id")},
                        {"task_id", update.at("task").at("task_id")},
                        {"client_id", "homemind-mobile-api"},
                        {"action", action},
                        {"expected_revision", update.at("task").at("revision")},
                        {"action_token", update.at("task").at("action_token")},
                        {"reason", "phone " + action + " in selftest"},
                        {"sent_at_ms", now_ms()}};
    publish_json(sender.get(), "pfn/v1/mobile/task/action", document, 1);
    return document.at("event_id").get<std::string>();
  };
  policy.phone_door_approve_event_id = phone_action(door_proposed, "approve");
  const Json door_after_phone = next_update();
  expect_task(door_after_phone, policy.door_task_id, "proposed", "phone_cannot_open_entrance_door");
  if (door_after_phone.at("task").at("revision") != door_proposed.at("task").at("revision")) {
    throw std::runtime_error("refused phone approval changed the door task");
  }
  phone_action(door_after_phone, "reject");
  expect_task(next_update(), policy.door_task_id, "rejected", "phone_rejects_entrance_door");

  policy.llm_task_id = uuid_v4();
  // registry v4: LED-AIRCON-01 (the TV indicator is not in the model any more)
  send_request(device_control_request(policy.llm_task_id, "NODE-RPI5-01", "llm", "LED-AIRCON-01",
                                      "set_color", {{"color", "red"}, {"pattern", "blink"}}));
  const Json llm_proposed = next_update();
  expect_task(llm_proposed, policy.llm_task_id, "proposed", "llm_needs_ui");
  publish_json(sender.get(), "pfn/v1/ui/action",
               Json{{"schema_version", "pfn.v1"},
                    {"event_id", uuid_v4()},
                    {"trace_id", llm_proposed.at("trace_id")},
                    {"task_id", policy.llm_task_id},
                    {"client_id", "homemind-ui-test-01"},
                    {"action", "approve"},
                    {"expected_revision", llm_proposed.at("task").at("revision")},
                    {"action_token", llm_proposed.at("task").at("action_token")},
                    {"reason", "llm proposal approved in selftest"},
                    {"sent_at_ms", now_ms()}},
               1);
  expect_task(next_update(), policy.llm_task_id, "ready", "llm_approved");
  expect_task(next_update(), policy.llm_task_id, "running", "llm_running");
  expect_task(next_update(), policy.llm_task_id, "completed", "llm_ack");

  policy.phone_llm_task_id = uuid_v4();
  send_request(device_control_request(policy.phone_llm_task_id, "NODE-RPI5-01", "llm", "LED-AIRCON-01",
                                      "set_color", {{"color", "red"}, {"pattern", "blink"}}));
  const Json phone_llm_proposed = next_update();
  expect_task(phone_llm_proposed, policy.phone_llm_task_id, "proposed", "phone_llm_needs_approval");
  phone_action(phone_llm_proposed, "approve");
  expect_task(next_update(), policy.phone_llm_task_id, "ready", "phone_llm_approved");
  expect_task(next_update(), policy.phone_llm_task_id, "running", "phone_llm_running");
  expect_task(next_update(), policy.phone_llm_task_id, "completed", "phone_llm_ack");

  policy.ui_task_id = uuid_v4();
  send_request(device_control_request(policy.ui_task_id, "NODE-RPI5-01", "ui", "SERVO-WINDOW-01",
                                      "set_position", {{"position_degrees", 0}}));
  expect_task(next_update(), policy.ui_task_id, "ready", "phone_window_close_auto");
  expect_task(next_update(), policy.ui_task_id, "running", "phone_window_running");
  expect_task(next_update(), policy.ui_task_id, "completed", "phone_window_ack");

  const std::string not_wired_id = uuid_v4();
  send_request(device_control_request(not_wired_id, "NODE-RPI5-02", "voice_parser", "LED-GUIDE-01",
                                      "set_switch", {{"on", true}}));
  expect_task(next_update(), not_wired_id, "rejected", "not_wired", "DEVICE_NOT_WIRED");
  const std::string capability_id = uuid_v4();
  send_request(device_control_request(capability_id, "NODE-RPI5-02", "voice_parser", "FAN-01",
                                      "set_level", {{"level_percent", 30}}));
  expect_task(next_update(), capability_id, "rejected", "fan_level", "CAPABILITY_NOT_ALLOWED");
  const std::string source_id = uuid_v4();
  send_request(device_control_request(source_id, "NODE-RPI5-01", "voice_parser", "LED-LIVING-01",
                                      "set_switch", {{"on", false}}));
  expect_task(next_update(), source_id, "rejected", "spoofed_origin", "SOURCE_NOT_ALLOWED");
  const std::string angle_id = uuid_v4();
  send_request(device_control_request(angle_id, "NODE-RPI5-02", "voice_parser", "SERVO-WINDOW-01",
                                      "set_position", {{"position_degrees", 45}}));
  expect_task(next_update(), angle_id, "rejected", "unregistered_angle", "POSITION_NOT_REGISTERED");

  Json mismatch = device_control_request(uuid_v4(), "NODE-RPI5-02", "voice_parser", "LED-LIVING-01",
                                         "set_switch", {{"on", true}});
  mismatch["target_device"] = "LED-AIRCON-01";
  policy.mismatch_event_id = mismatch.at("event_id").get<std::string>();
  send_request(mismatch);

  const auto quiet_perception = [&](const std::string& source_node_id) {
    return Json{{"schema_version", "pfn.v1"},
                {"event_id", uuid_v4()},
                {"trace_id", uuid_v4()},
                {"source_node_id", source_node_id},
                {"observation_type", "ACTIVITY_CONTEXT"},
                {"zone", "desk"},
                {"confidence", 0.84},
                {"attributes",
                 {{"activity", "MEETING"},
                  {"audio_context", "ZOOM_SPEAKER"},
                  {"quiet_required", true},
                  {"transition", "ACTIVATED"},
                  {"evidence", Json::array({"AUDIO_CONTEXT:ZOOM_SPEAKER"})},
                  {"valid_until_ms", now_ms() + 30000},
                  {"model_version", "selftest"}}},
                {"observed_at_ms", now_ms()},
                {"simulated", true}};
  };
  const Json foreign_perception = quiet_perception("NODE-RPI5-01");
  policy.foreign_perception_event_id = foreign_perception.at("event_id").get<std::string>();
  publish_json(sender.get(), "pfn/v1/ai/perception", foreign_perception, 1);
  publish_json(sender.get(), "pfn/v1/ai/perception", quiet_perception("NODE-RPI5-02"), 1);
  const Json quiet_ready = next_update();
  if (quiet_ready.at("task").at("intent") != "quiet_mode_on" ||
      quiet_ready.at("task").at("state") != "ready") {
    std::cerr << "TASK_DEBUG " << quiet_ready.dump() << std::endl;
    throw std::runtime_error("context rule did not start quiet_mode_on");
  }
  const std::string quiet_task_id = quiet_ready.at("task").at("task_id").get<std::string>();
  std::cout << "POLICY context_quiet_mode_auto state=ready" << std::endl;
  expect_task(next_update(), quiet_task_id, "running", "quiet_mode_running");
  expect_task(next_update(), quiet_task_id, "completed", "quiet_mode_ack");
  publish_json(sender.get(), "pfn/v1/ai/perception", quiet_perception("NODE-RPI5-02"), 1);

  // 0.6.2 feedback override: "상황이 달라요" while quiet mode is on turns it off at once, and the same context
  // cannot turn it on again until it ends (quiet_required_off) or 30 min pass.
  const Json disputed = quiet_perception("NODE-RPI5-02");
  publish_json(sender.get(), "pfn/v1/ai/perception", disputed, 1);
  publish_json(sender.get(), "pfn/v1/ui/feedback",
               Json{{"schema_version", "pfn.v1"},
                    {"event_id", uuid_v4()},
                    {"trace_id", uuid_v4()},
                    {"client_id", "homemind-ui-test-01"},
                    {"feedback_type", "context_feedback"},
                    {"observation_event_id", disputed.at("event_id")},
                    {"rating", "negative"},
                    {"sent_at_ms", now_ms()}},
               1);
  const Json undo_ready = next_update();
  if (undo_ready.at("task").at("intent") != "quiet_mode_off" ||
      undo_ready.at("task").at("parameters").value("rule", "") != "quiet_context_wrong" ||
      undo_ready.at("task").at("state") != "ready") {
    std::cerr << "TASK_DEBUG " << undo_ready.dump() << std::endl;
    throw std::runtime_error("negative context feedback did not turn quiet mode off");
  }
  const std::string undo_task_id = undo_ready.at("task").at("task_id").get<std::string>();
  expect_task(next_update(), undo_task_id, "running", "feedback_quiet_off_running");
  expect_task(next_update(), undo_task_id, "completed", "feedback_quiet_off_ack");
  publish_json(sender.get(), "pfn/v1/ai/perception", quiet_perception("NODE-RPI5-02"), 1);  // suppressed
  Json released = quiet_perception("NODE-RPI5-02");
  released["attributes"]["quiet_required"] = false;
  released["attributes"]["transition"] = "DEACTIVATED";
  publish_json(sender.get(), "pfn/v1/ai/perception", released, 1);  // ends it: no second quiet_mode_off
  const std::string sentinel_id = uuid_v4();
  send_request(device_control_request(sentinel_id, "NODE-RPI5-02", "voice_parser", "SERVO-WINDOW-01",
                                      "set_position", {{"position_degrees", 45}}));
  expect_task(next_update(), sentinel_id, "rejected", "suppressed_context_made_no_task", "POSITION_NOT_REGISTERED");
  std::cout << "POLICY context_feedback_override quiet_off_once suppressed_until_released" << std::endl;
  publish_json(sender.get(), "pfn/v1/ai/perception", quiet_perception("NODE-RPI5-02"), 1);
  const Json again_ready = next_update();
  if (again_ready.at("task").at("intent") != "quiet_mode_on") {
    std::cerr << "TASK_DEBUG " << again_ready.dump() << std::endl;
    throw std::runtime_error("a new meeting after the disputed one did not turn quiet mode on");
  }
  const std::string again_task_id = again_ready.at("task").at("task_id").get<std::string>();
  expect_task(next_update(), again_task_id, "running", "quiet_mode_again_running");
  expect_task(next_update(), again_task_id, "completed", "quiet_mode_again_ack");

  policy.transcript_marker = "SELFTEST-TRANSCRIPT-" + uuid_v4();
  publish_json(sender.get(), "pfn/v1/ai/llm/request",
               Json{{"schema_version", "pfn.v1"},
                    {"event_id", uuid_v4()},
                    {"trace_id", uuid_v4()},
                    {"request_id", uuid_v4()},
                    {"source_node_id", "NODE-RPI5-02"},
                    {"utterance_id", uuid_v4()},
                    {"transcript",
                     {{"text", policy.transcript_marker},
                      {"language", "ko"},
                      {"stt_model_version", "selftest"},
                      {"stt_confidence", 0.9}}},
                    {"parser", {{"result", "NO_MATCH"}, {"rule_version", "selftest"}}},
                    {"requested_at_ms", now_ms()},
                    {"expires_at_ms", now_ms() + 10000}},
               1);

  const std::string privacy_request_event_id = uuid_v4();
  const Json privacy_request{{"schema_version", "pfn.v1"},
                             {"event_id", privacy_request_event_id},
                             {"trace_id", scenario_trace_id},
                             {"client_id", "homemind-ui-test-01"},
                             {"mode", "private"},
                             {"sent_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/ui/privacy/set", privacy_request, 1);
  const Json privacy_state = wait_privacy_state(sender_context, 1);
  if (privacy_state.at("mode") != "private" ||
      privacy_state.at("camera_allowed").get<bool>() ||
      privacy_state.at("microphone_allowed").get<bool>()) {
    throw std::runtime_error("privacy state is incorrect");
  }

  const std::string safety_alert_id = uuid_v4();
  const Json safety_alert{{"schema_version", "pfn.v1"},
                          {"event_id", uuid_v4()},
                          {"trace_id", scenario_trace_id},
                          {"alert_id", safety_alert_id},
                          {"source_node_id", options.node_id},
                          {"alert_type", "manual_test"},
                          {"zone", "living_room"},
                          {"severity", "p0"},
                          {"summary", "selftest safety alert"},
                          {"evidence", {{"simulated", true}}},
                          {"raised_at_ms", now_ms()},
                          {"automatic_actions", Json::array()}};
  publish_json(sender.get(), "pfn/v1/system/safety/alert", safety_alert, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const std::string safety_ack_event_id = uuid_v4();
  const Json safety_ack{{"schema_version", "pfn.v1"},
                        {"event_id", safety_ack_event_id},
                        {"trace_id", scenario_trace_id},
                        {"alert_id", safety_alert_id},
                        {"client_id", "homemind-ui-test-01"},
                        {"action", "dismiss_test"},
                        {"sent_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/ui/safety/ack", safety_ack, 1);

  const std::string feedback_event_id = uuid_v4();
  const Json feedback{{"schema_version", "pfn.v1"},
                      {"event_id", feedback_event_id},
                      {"trace_id", scenario_trace_id},
                      {"client_id", "homemind-ui-test-01"},
                      {"feedback_type", "action_feedback"},
                      {"task_id", task_id},
                      {"observation_event_id", nullptr},
                      {"rating", "positive"},
                      {"note", "selftest"},
                      {"sent_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/ui/feedback", feedback, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const std::string state_request_id = uuid_v4();
  const Json state_request{{"schema_version", "pfn.v1"},
                           {"event_id", uuid_v4()},
                           {"trace_id", scenario_trace_id},
                           {"request_id", state_request_id},
                           {"client_id", "homemind-ui-test-01"},
                           {"sent_at_ms", now_ms()}};
  publish_json(sender.get(), "pfn/v1/core/state/request", state_request, 1);
  const Json snapshot = wait_state_response(sender_context, state_request_id);
  if (snapshot.at("request_id") != state_request_id || snapshot.at("tasks").empty() ||
      snapshot.at("world_state").empty() || snapshot.at("privacy").at("mode") != "private" ||
      !snapshot.contains("safety") || !snapshot.contains("system_health") ||
      !snapshot.contains("clock") || !snapshot.at("clock").contains("valid") ||
      !snapshot.at("system_health").at("available").get<bool>()) {
    std::cerr << "SNAPSHOT_DEBUG " << snapshot.dump() << std::endl;
    throw std::runtime_error("state snapshot is incomplete");
  }

  const std::string command_event_id = uuid_v4();
  const std::string command_id = uuid_v4();
  const std::string trace_id = uuid_v4();
  const std::int64_t issued_at_ms = now_ms();
  const Json command{{"schema_version", "pfn.v1"},
                     {"event_id", command_event_id},
                     {"task_id", nullptr},
                     {"command_id", command_id},
                     {"trace_id", trace_id},
                     {"target_node_id", options.node_id},
                     {"target_device_id", "FAN-01"},
                     {"action", "set_level"},
                     {"parameters", {{"level_percent", 30}}},
                     {"issued_at_ms", issued_at_ms},
                     {"expires_at_ms", issued_at_ms + 10000},
                     {"attempt", 1}};
  const std::string command_topic = "pfn/v1/device/" + options.node_id + "/command";
  publish_json(sender.get(), command_topic, command, 1);
  const Json first_ack = wait_ack(sender_context, command_id, 1);
  if (!first_ack.at("ok").get<bool>() || first_ack.at("duplicate").get<bool>()) {
    throw std::runtime_error("first ACK result is incorrect");
  }
  publish_json(sender.get(), command_topic, command, 1);
  const Json second_ack = wait_ack(sender_context, command_id, 2);
  if (!second_ack.at("ok").get<bool>() || !second_ack.at("duplicate").get<bool>()) {
    throw std::runtime_error("duplicate ACK result is incorrect");
  }

  const std::string invalid_payload = "{not-json";
  require_mosquitto(mosquitto_publish(sender.get(), nullptr,
                                      ("pfn/v1/device/" + options.node_id + "/telemetry").c_str(),
                                      static_cast<int>(invalid_payload.size()), invalid_payload.data(),
                                      0, false),
                    "publish invalid JSON test");
  std::this_thread::sleep_for(std::chrono::seconds(2));
  verify_database(options, command_event_id, command_id, telemetry_event_id,
                  invalid_telemetry_event_id, heartbeat_event_id, perception_event_id, task_id,
                  mapped_task_id, ui_action_event_id, safety_alert_id,
                  safety_ack_event_id, privacy_request_event_id, feedback_event_id,
                  stale_action_event_id, policy);

  mosquitto_disconnect(simulator.get());
  mosquitto_disconnect(sender.get());
  mosquitto_loop_stop(simulator.get(), true);
  mosquitto_loop_stop(sender.get(), true);
  simulator.reset();
  sender.reset();
  mosquitto_lib_cleanup();
  std::cout << "SELFTEST_PASS command_id=" << command_id << " trace_id=" << trace_id << std::endl;
  return 0;
}

}  // namespace homemind

int main(int argc, char** argv) {
  try {
    return homemind::run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "SELFTEST_FAIL " << error.what() << std::endl;
    return 1;
  }
}
