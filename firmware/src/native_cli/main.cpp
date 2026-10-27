// PC-only driver for lib/hm_device (pio run -e native). Used by tools/check_messages.py:
// reads one JSON operation per stdin line and prints one JSON object per stdout line with every
// MQTT message the controller logic would publish and every output (HAL) write.
// This is a simulation of the decision logic, not a hardware test.
//
// Operations:
//   {"op":"boot","node":"CTRL-ESP32-ACT-01","wall_ms":1788278400000|null}
//   {"op":"clock","advance_ms":1000}                 (wall clock, if valid, advances too)
//   {"op":"command","payload":{...},"hold_ms":0}    (processed hold_ms after receipt)
//   {"op":"tick"} {"op":"heartbeat"} {"op":"will"} {"op":"telemetry"} {"op":"reconnect"}
//   {"op":"board_config"}
// After each operation the CLI prints {"kind":"end"}.
#include <ArduinoJson.h>

#include <iostream>
#include <random>
#include <string>

#include <hm_node.h>

namespace {

std::mt19937 generator(20261021u);
uint32_t random32() { return static_cast<uint32_t>(generator()); }

void emit(const JsonDocument& document) {
  std::string line;
  serializeJson(document, line);
  std::cout << line << "\n";
}

class PrintingHal : public hm::OutputHal {
 public:
  bool writeServo(uint8_t gpio, int degrees) override { return record("servo", gpio, degrees); }
  void holdServoLow(uint8_t gpio) override { record("servo_low", gpio, 0); }
  bool writeDigital(uint8_t gpio, bool high) override { return record("digital", gpio, high ? 1 : 0); }

 private:
  bool record(const char* op, uint8_t gpio, int value) {
    JsonDocument event;
    event["kind"] = "hal";
    event["op"] = op;
    event["gpio"] = gpio;
    event["value"] = value;
    emit(event);
    return true;
  }
};

void emitMessage(const char* kind, const char* topic, int qos, const char* payload, size_t length) {
  JsonDocument message;
  message["kind"] = kind;
  message["topic"] = topic;
  message["qos"] = qos;
  message["retained"] = hm::kRetained;
  message["bytes"] = length;
  if (length == 0 || deserializeJson(message["payload"].to<JsonVariant>(), payload, length)) {
    message["payload"] = nullptr;
  }
  emit(message);
}

void emitBoardConfig() {
  JsonDocument config;
  config["kind"] = "board_config";
  config["registry_version"] = hm::kRegistryVersion;
  JsonObject protocol = config["protocol"].to<JsonObject>();
  protocol["heartbeat_interval_ms"] = hm::kHeartbeatIntervalMs;
  protocol["offline_after_ms"] = hm::kOfflineAfterMs;
  protocol["command_ttl_ms"] = hm::kCommandTtlMs;
  protocol["ack_timeout_ms"] = hm::kAckTimeoutMs;
  protocol["maximum_payload_bytes"] = hm::kMaximumPayloadBytes;
  protocol["commands_and_user_actions_qos"] = hm::kQosCommandAck;
  protocol["telemetry_heartbeat_health_qos"] = hm::kQosTelemetryHeartbeat;
  protocol["retained"] = hm::kRetained;
  JsonArray nodes = config["nodes"].to<JsonArray>();
  const hm::NodeSpec* specs[] = {&hm::kActuatorNode, &hm::kLedNode};
  for (const hm::NodeSpec* spec : specs) {
    JsonObject node = nodes.add<JsonObject>();
    node["node_id"] = spec->node_id;
    node["node_type"] = spec->node_type;
    node["firmware_version"] = spec->firmware_version;
    JsonArray devices = node["devices"].to<JsonArray>();
    for (uint8_t index = 0; index < spec->device_count; ++index) {
      const hm::DeviceSpec& device = spec->devices[index];
      JsonObject item = devices.add<JsonObject>();
      item["device_id"] = device.device_id;
      item["pin"] = device.pin_label;
      item["gpio"] = device.gpio;
      item["reading_name"] = device.reading_name;
      JsonArray capabilities = item["capabilities"].to<JsonArray>();
      switch (device.kind) {
        case hm::DeviceKind::Servo: {
          capabilities.add("set_position");
          JsonObject positions = item["positions"].to<JsonObject>();
          positions["open"] = device.open_degrees;
          positions["closed"] = device.closed_degrees;
          positions["calibrated"] = device.positions_calibrated;
          break;
        }
        case hm::DeviceKind::Switch:
          capabilities.add("set_switch");
          break;
        case hm::DeviceKind::LedRed: {
          capabilities.add("set_switch");
          capabilities.add("set_color");
          JsonArray colors = item["supported_colors"].to<JsonArray>();
          for (uint8_t color = 0; color < hm::kLedSupportedColorCount; ++color) {
            colors.add(hm::kLedSupportedColors[color]);
          }
          break;
        }
      }
    }
    JsonArray unwired = node["unwired_device_ids"].to<JsonArray>();
    for (uint8_t index = 0; index < spec->unwired_count; ++index) {
      unwired.add(spec->unwired_device_ids[index]);
    }
    JsonArray forbidden = node["forbidden_gpios"].to<JsonArray>();
    const bool actuator = spec == &hm::kActuatorNode;
    const uint8_t* gpios = actuator ? hm::kActuatorForbiddenGpios : hm::kLedForbiddenGpios;
    const uint8_t count = actuator ? hm::kActuatorForbiddenGpioCount : hm::kLedForbiddenGpioCount;
    for (uint8_t index = 0; index < count; ++index) forbidden.add(gpios[index]);
  }
  emit(config);
}

void emitError(const std::string& text) {
  JsonDocument error;
  error["kind"] = "cli_error";
  error["message"] = text;
  emit(error);
}

}  // namespace

int main() {
  PrintingHal hal;
  hm::Node* node = nullptr;
  hm::TimeNow now = {0, false, 0};
  char buffer[hm::kMessageBufferBytes];
  std::string line;

  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    struct EndMarker {
      ~EndMarker() { std::cout << "{\"kind\":\"end\"}" << std::endl; }  // one per operation line
    } end_marker;
    JsonDocument op;
    if (deserializeJson(op, line)) {
      emitError("cannot parse operation");
      continue;
    }
    const std::string name = op["op"] | "";
    if (name == "board_config") {
      emitBoardConfig();
      continue;
    }
    if (name == "boot") {
      const hm::NodeSpec* spec = hm::findNodeSpec(op["node"] | "");
      if (spec == nullptr) {
        emitError("unknown node");
        continue;
      }
      delete node;
      node = new hm::Node(*spec, hal, random32);
      now.mono_ms = op["mono_ms"] | 0;
      now.wall_valid = op["wall_ms"].is<long long>();
      now.wall_ms = now.wall_valid ? op["wall_ms"].as<long long>() : 0;
      node->boot(now);
      continue;
    }
    if (node == nullptr) {
      emitError("boot first");
      continue;
    }
    if (name == "clock") {
      const uint64_t advance = op["advance_ms"] | 0;
      now.mono_ms += advance;
      if (op["wall_ms"].is<long long>()) {
        now.wall_valid = true;
        now.wall_ms = op["wall_ms"].as<long long>();
      } else if (now.wall_valid) {
        now.wall_ms += static_cast<int64_t>(advance);
      }
      node->tick(now);
    } else if (name == "tick") {
      node->tick(now);
    } else if (name == "reconnect") {
      node->onConnected();
    } else if (name == "heartbeat" || name == "will") {
      const bool online = name == "heartbeat";
      emitMessage(online ? "heartbeat" : "will", hm::Node::heartbeatTopic(),
                  hm::kQosTelemetryHeartbeat, buffer,
                  node->buildHeartbeat(now, online, buffer, sizeof(buffer)));
    } else if (name == "telemetry") {
      for (uint8_t index = 0; index < node->spec().device_count; ++index) {
        emitMessage("telemetry", node->telemetryTopic(), hm::kQosTelemetryHeartbeat, buffer,
                    node->buildTelemetry(index, now, buffer, sizeof(buffer)));
      }
    } else if (name == "command") {
      std::string payload;
      if (op["payload"].is<const char*>()) {
        payload = op["payload"].as<const char*>();  // raw text (malformed-input cases)
      } else {
        serializeJson(op["payload"], payload);
      }
      const uint64_t received = now.mono_ms;
      const uint64_t hold = op["hold_ms"] | 0;
      now.mono_ms += hold;
      if (now.wall_valid) now.wall_ms += static_cast<int64_t>(hold);
      size_t ack_length = 0;
      const hm::CommandResult result = node->handleCommand(
          payload.data(), payload.size(), received, now, buffer, sizeof(buffer), &ack_length);
      JsonDocument summary;
      summary["kind"] = "result";
      summary["ackable"] = result.ackable;
      summary["duplicate"] = result.duplicate;
      summary["executed"] = result.executed;
      summary["status"] = hm::ackStatusText(result.status);
      summary["error_code"] = result.error_code;
      emit(summary);
      if (ack_length > 0) {
        emitMessage("ack", node->ackTopic(), hm::kQosCommandAck, buffer, ack_length);
      }
    } else {
      emitError("unknown op " + name);
    }
  }
  delete node;
  return 0;
}
