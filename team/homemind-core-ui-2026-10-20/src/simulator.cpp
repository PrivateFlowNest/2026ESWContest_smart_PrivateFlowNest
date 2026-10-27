#include "common.hpp"

#include <mosquitto.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace homemind {
using Json = nlohmann::json;

void require_mqtt(int result, const std::string& operation) {
  if (result != MOSQ_ERR_SUCCESS) {
    throw std::runtime_error(operation + ": " + mosquitto_strerror(result));
  }
}

struct Options {
  std::string broker_host{"10.10.10.2"};
  int broker_port{1883};
  std::string node_id{"CTRL-ESP32-ACT-01"};
  std::string profile{"normal"};
  int duration_seconds{30};
  int interval_ms{1000};
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
    } else if (argument == "--node-id") {
      options.node_id = value;
    } else if (argument == "--profile") {
      options.profile = value;
    } else if (argument == "--duration-seconds") {
      options.duration_seconds = std::stoi(value);
    } else if (argument == "--interval-ms") {
      options.interval_ms = std::stoi(value);
    } else {
      throw std::runtime_error("unknown option: " + argument);
    }
  }
  const bool profile_ok = options.profile == "normal" || options.profile == "out-of-range" ||
                          options.profile == "wrong-type" || options.profile == "unknown" ||
                          options.profile == "mixed";
  if (options.node_id.empty() || options.node_id.size() > 64 || options.broker_port < 1 ||
      options.broker_port > 65535 || !profile_ok || options.duration_seconds < 0 ||
      options.interval_ms < 100) {
    throw std::runtime_error("invalid simulator option");
  }
  return options;
}

std::string node_type(const std::string& node_id) {
  if (node_id.starts_with("CTRL-ESP8266")) {
    return "esp8266";
  }
  if (node_id.starts_with("CTRL-OCTOPUS")) {
    return "octopus";
  }
  return "esp32";
}

struct Context {
  explicit Context(std::string value) : node_id(std::move(value)) {}
  std::string node_id;
  std::atomic<bool> connected{false};
  std::atomic<bool> subscribed{false};
  std::mutex mutex;
  std::set<std::string> completed_commands;
};

void publish_json(mosquitto* client, const std::string& topic, const Json& value, int qos) {
  const std::string payload = value.dump();
  require_mqtt(mosquitto_publish(client, nullptr, topic.c_str(), static_cast<int>(payload.size()),
                                 payload.data(), qos, false),
               "publish " + topic);
}

void on_connect(mosquitto* client, void* userdata, int result) {
  auto& context = *static_cast<Context*>(userdata);
  if (result != 0) {
    return;
  }
  context.connected = true;
  const std::string topic = "pfn/v1/device/" + context.node_id + "/command";
  if (mosquitto_subscribe(client, nullptr, topic.c_str(), 1) != MOSQ_ERR_SUCCESS) {
    context.connected = false;
  }
}

void on_subscribe(mosquitto*, void* userdata, int, int, const int*) {
  static_cast<Context*>(userdata)->subscribed = true;
}

void on_disconnect(mosquitto*, void* userdata, int) {
  static_cast<Context*>(userdata)->connected = false;
}

void on_message(mosquitto* client, void* userdata, const mosquitto_message* message) {
  auto& context = *static_cast<Context*>(userdata);
  try {
    if (!message || !message->payload || message->payloadlen <= 0) {
      return;
    }
    const auto* begin = static_cast<const char*>(message->payload);
    const Json command = Json::parse(begin, begin + message->payloadlen);
    if (command.at("target_node_id").get<std::string>() != context.node_id) {
      return;
    }
    const std::string command_id = command.at("command_id").get<std::string>();
    bool duplicate = false;
    {
      std::lock_guard<std::mutex> lock(context.mutex);
      duplicate = !context.completed_commands.insert(command_id).second;
    }
    const Json acknowledgement{{"schema_version", "pfn.v1"},
                               {"event_id", uuid_v4()},
                               {"trace_id", command.at("trace_id")},
                               {"command_id", command_id},
                               {"node_id", context.node_id},
                               {"target_device_id", command.at("target_device_id")},
                               {"ok", true},
                               {"duplicate", duplicate},
                               {"status", "succeeded"},
                               {"executed_at_ms", now_ms()},
                               {"result", {{"simulated", true}, {"action", command.at("action")}}},
                               {"error", nullptr}};
    publish_json(client, "pfn/v1/device/" + context.node_id + "/ack", acknowledgement, 1);
    std::cout << "COMMAND_ACK command_id=" << command_id
              << " duplicate=" << (duplicate ? "true" : "false") << std::endl;
  } catch (const std::exception& error) {
    std::cerr << "COMMAND_ERROR " << error.what() << std::endl;
  }
}

Json normal_readings(const std::string& node_id, std::uint64_t sequence) {
  if (node_id.starts_with("CTRL-ESP8266")) {
    return Json{{"readings", {{"light_on", sequence % 2 == 0},
                               {"display_value", static_cast<int>(sequence % 100)}}},
                {"reading_sources", {{"light_on", "LED-LIVING-01"},
                                      {"display_value", "FND-WASHER-01"}}}};
  }
  if (node_id.starts_with("CTRL-OCTOPUS")) {
    return Json{{"readings", {{"x_home_active", sequence % 10 == 0},
                               {"y_home_active", sequence % 12 == 0}}},
                {"reading_sources", {{"x_home_active", "SENSOR-HOME-X-01"},
                                      {"y_home_active", "SENSOR-HOME-Y-01"}}}};
  }
  return Json{{"readings", {{"fan_level_percent", static_cast<int>(sequence % 101)}}},
              {"reading_sources", {{"fan_level_percent", "FAN-01"}}}};
}

Json profiled_readings(const Options& options, std::uint64_t sequence) {
  std::string profile = options.profile;
  if (profile == "mixed") {
    static constexpr const char* profiles[] = {"normal", "out-of-range", "wrong-type", "unknown"};
    profile = profiles[sequence % 4];
  }
  if (profile == "normal") {
    return normal_readings(options.node_id, sequence);
  }
  if (profile == "out-of-range") {
    return Json{{"readings", {{"fan_level_percent", 150}}},
                {"reading_sources", {{"fan_level_percent", "FAN-01"}}}};
  }
  if (profile == "wrong-type") {
    return Json{{"readings", {{"fan_level_percent", "invalid"}}},
                {"reading_sources", {{"fan_level_percent", "FAN-01"}}}};
  }
  return Json{{"readings", {{"unknown_sensor_value", 1}}},
              {"reading_sources", {{"unknown_sensor_value", "FAN-01"}}}};
}

std::atomic<bool> running{true};
void stop_handler(int) { running = false; }

int run(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  require_mqtt(mosquitto_lib_init(), "mosquitto_lib_init");
  Context context{options.node_id};
  std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)> client(
      mosquitto_new((options.node_id + "-simulator-" + uuid_v4()).c_str(), true, &context),
      mosquitto_destroy);
  if (!client) {
    throw std::runtime_error("cannot create MQTT simulator client");
  }
  mosquitto_connect_callback_set(client.get(), on_connect);
  mosquitto_subscribe_callback_set(client.get(), on_subscribe);
  mosquitto_disconnect_callback_set(client.get(), on_disconnect);
  mosquitto_message_callback_set(client.get(), on_message);
  require_mqtt(mosquitto_connect_async(client.get(), options.broker_host.c_str(),
                                       options.broker_port, 30),
               "connect simulator");
  require_mqtt(mosquitto_loop_start(client.get()), "start simulator loop");
  for (int count = 0; count < 100 && (!context.connected || !context.subscribed); ++count) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!context.connected || !context.subscribed) {
    throw std::runtime_error("simulator did not connect and subscribe");
  }
  std::signal(SIGINT, stop_handler);
  std::signal(SIGTERM, stop_handler);

  const auto started = std::chrono::steady_clock::now();
  auto next_telemetry = started;
  auto next_heartbeat = started;
  std::uint64_t sequence = 0;
  std::uint64_t heartbeat_sequence = 0;
  const std::string trace_id = uuid_v4();
  std::cout << "SIMULATOR_STARTED node_id=" << options.node_id
            << " profile=" << options.profile << std::endl;
  while (running) {
    const auto current = std::chrono::steady_clock::now();
    if (options.duration_seconds > 0 &&
        current - started >= std::chrono::seconds(options.duration_seconds)) {
      break;
    }
    if (current >= next_telemetry) {
      const Json profile = profiled_readings(options, sequence);
      const Json telemetry{{"schema_version", "pfn.v1"},
                           {"event_id", uuid_v4()},
                           {"trace_id", trace_id},
                           {"node_id", options.node_id},
                           {"seq", sequence},
                           {"sent_at_ms", now_ms()},
                           {"uptime_ms", uptime_ms()},
                           {"sensor_ready", true},
                           {"readings", profile.at("readings")},
                           {"reading_sources", profile.at("reading_sources")}};
      publish_json(client.get(), "pfn/v1/device/" + options.node_id + "/telemetry", telemetry, 0);
      ++sequence;
      next_telemetry += std::chrono::milliseconds(options.interval_ms);
    }
    if (current >= next_heartbeat) {
      const Json heartbeat{{"schema_version", "pfn.v1"},
                           {"event_id", uuid_v4()},
                           {"trace_id", trace_id},
                           {"node_id", options.node_id},
                           {"node_type", node_type(options.node_id)},
                           {"status", "online"},
                           {"seq", heartbeat_sequence++},
                           {"sent_at_ms", now_ms()},
                           {"uptime_ms", uptime_ms()},
                           {"version", "fake-device-0.5.0"}};
      publish_json(client.get(), "pfn/v1/system/heartbeat", heartbeat, 0);
      next_heartbeat += std::chrono::seconds(5);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cout << "SIMULATOR_STOPPED telemetry_sent=" << sequence
            << " heartbeat_sent=" << heartbeat_sequence << std::endl;
  mosquitto_disconnect(client.get());
  mosquitto_loop_stop(client.get(), true);
  client.reset();
  mosquitto_lib_cleanup();
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
