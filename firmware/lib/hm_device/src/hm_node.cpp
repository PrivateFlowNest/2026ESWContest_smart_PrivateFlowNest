#include "hm_node.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace hm {

namespace {

struct Rejection {
  const char* code;
  const char* message;
};

const Rejection kNone = {nullptr, nullptr};

bool isRejection(const Rejection& value) { return value.code != nullptr; }

// Parameters after validation.
struct Parsed {
  int degrees;
  bool on;
  uint32_t auto_off_seconds;
  bool color_off;
  bool blink;
  const char* color;
};

bool deviceSupports(DeviceKind kind, const char* action) {
  switch (kind) {
    case DeviceKind::Servo: return strcmp(action, "set_position") == 0;
    case DeviceKind::Switch: return strcmp(action, "set_switch") == 0;
    case DeviceKind::LedRed:
      return strcmp(action, "set_switch") == 0 || strcmp(action, "set_color") == 0;
  }
  return false;
}

bool isKnownAction(const char* action) {
  static const char* const actions[] = {"set_switch", "set_color", "set_level", "set_position",
                                        "move_axis",  "home_axis", "set_led",   "set_number"};
  for (size_t index = 0; index < sizeof(actions) / sizeof(actions[0]); ++index) {
    if (strcmp(action, actions[index]) == 0) return true;
  }
  return false;
}

bool inList(const char* value, const char* const* list, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    if (strcmp(value, list[index]) == 0) return true;
  }
  return false;
}

Rejection validateParameters(const DeviceSpec& device, const char* action, JsonObjectConst params,
                             Parsed* parsed) {
  memset(parsed, 0, sizeof(*parsed));
  if (strcmp(action, "set_position") == 0) {
    JsonVariantConst value = params["position_degrees"];
    if (!value.is<double>() || value.is<bool>()) {
      return {"PARAMETERS_INVALID", "set_position needs position_degrees 0..180"};
    }
    const double degrees = value.as<double>();
    if (!(degrees >= 0.0 && degrees <= 180.0)) {
      return {"PARAMETERS_INVALID", "set_position needs position_degrees 0..180"};
    }
    if (degrees != static_cast<double>(device.closed_degrees) &&
        degrees != static_cast<double>(device.open_degrees)) {
      return {"POSITION_NOT_REGISTERED", "angle is not a registered open/closed position"};
    }
    parsed->degrees = static_cast<int>(degrees);
    return kNone;
  }
  if (strcmp(action, "set_switch") == 0) {
    JsonVariantConst on = params["on"];
    if (!on.is<bool>()) return {"PARAMETERS_INVALID", "set_switch needs boolean on"};
    parsed->on = on.as<bool>();
    JsonVariantConst auto_off = params["auto_off_after_seconds"];
    if (!auto_off.isNull()) {
      if (device.kind != DeviceKind::LedRed) {
        return {"PARAMETERS_INVALID", "auto_off_after_seconds is only supported on LEDs"};
      }
      if (!auto_off.is<double>() || auto_off.is<bool>()) {
        return {"PARAMETERS_INVALID", "auto_off_after_seconds must be a number"};
      }
      const double seconds = auto_off.as<double>();
      if (!(seconds > 0.0 && seconds <= static_cast<double>(kMaxAutoOffSeconds)) ||
          seconds != static_cast<double>(static_cast<uint32_t>(seconds))) {
        return {"PARAMETERS_INVALID", "auto_off_after_seconds must be a whole number 1..86400"};
      }
      // auto-off only means something when switching on; with on=false it is ignored.
      parsed->auto_off_seconds = parsed->on ? static_cast<uint32_t>(seconds) : 0;
    }
    return kNone;
  }
  if (strcmp(action, "set_color") == 0) {
    static const char* const schema_colors[] = {"off", "green", "red", "amber"};
    JsonVariantConst color = params["color"];
    if (!color.is<const char*>()) return {"PARAMETERS_INVALID", "set_color needs color"};
    const char* color_text = color.as<const char*>();
    if (!inList(color_text, schema_colors, 4)) {
      return {"PARAMETERS_INVALID", "unknown colour"};
    }
    JsonVariantConst pattern = params["pattern"];
    bool blink = false;
    if (!pattern.isNull()) {
      if (!pattern.is<const char*>()) {
        return {"PARAMETERS_INVALID", "pattern must be steady or blink"};
      }
      const char* pattern_text = pattern.as<const char*>();
      if (strcmp(pattern_text, "blink") == 0) {
        blink = true;
      } else if (strcmp(pattern_text, "steady") != 0) {
        return {"PARAMETERS_INVALID", "pattern must be steady or blink"};
      }
    }
    if (!inList(color_text, kLedSupportedColors, kLedSupportedColorCount)) {
      return {"COLOR_NOT_SUPPORTED", "colour is not in supported_colors (off, red)"};
    }
    parsed->color_off = strcmp(color_text, "off") == 0;
    parsed->blink = blink;
    parsed->color = color_text;
    return kNone;
  }
  return {"CAPABILITY_NOT_ALLOWED", "action is not a capability of this device"};
}

size_t finish(const JsonDocument& document, char* out, size_t capacity) {
  const size_t length = measureJson(document);
  if (out == nullptr || length + 1 > capacity) return 0;
  serializeJson(document, out, capacity);
  return length;
}

void copyText(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) return;
  strncpy(destination, source ? source : "", capacity - 1);
  destination[capacity - 1] = '\0';
}

const char* expiryMessage(ExpiryReason reason) {
  switch (reason) {
    case ExpiryReason::WallClock: return "expires_at_ms has passed (wall clock)";
    case ExpiryReason::HeldTooLong: return "held longer than the command TTL";
    case ExpiryReason::CoreClockFloor: return "expires_at_ms is older than a newer core command";
    default: return "expired";
  }
}

}  // namespace

Node::Node(const NodeSpec& spec, OutputHal& hal, Random32 random32)
    : spec_(spec), hal_(hal), random32_(random32), heartbeat_seq_(0), telemetry_seq_(0) {
  for (uint8_t index = 0; index < kMaxDevices; ++index) {
    DeviceState& state = devices_[index];
    state.position = 0;
    state.on = false;
    state.led = LedState();
    state.level = false;
    state.level_written = false;
    state.dirty = false;
  }
  boot_trace_id_[0] = '\0';
  snprintf(command_topic_, sizeof(command_topic_), "pfn/v1/device/%s/command", spec.node_id);
  snprintf(ack_topic_, sizeof(ack_topic_), "pfn/v1/device/%s/ack", spec.node_id);
  snprintf(telemetry_topic_, sizeof(telemetry_topic_), "pfn/v1/device/%s/telemetry", spec.node_id);
}

void Node::boot(const TimeNow& now) {
  const uint8_t count = spec_.device_count < kMaxDevices ? spec_.device_count : kMaxDevices;
  // 1. Servos first: signal held low, no pulses (registry v4: open/close motors are driven only by a command,
  //    like the user's tested sketch). The model is assumed closed at power-on (registry positions.closed).
  for (uint8_t index = 0; index < count; ++index) {
    const DeviceSpec& device = spec_.devices[index];
    if (device.kind != DeviceKind::Servo) continue;
    hal_.holdServoLow(device.gpio);
    devices_[index].position = device.closed_degrees;
    devices_[index].dirty = true;
  }
  // 2. Switches off. 3. LEDs off.
  for (int pass = 0; pass < 2; ++pass) {
    const DeviceKind kind = pass == 0 ? DeviceKind::Switch : DeviceKind::LedRed;
    for (uint8_t index = 0; index < count; ++index) {
      const DeviceSpec& device = spec_.devices[index];
      if (device.kind != kind) continue;
      hal_.writeDigital(device.gpio, false);
      devices_[index].on = false;
      devices_[index].level = false;
      devices_[index].level_written = true;
      devices_[index].dirty = true;
    }
  }
  static_cast<void>(now);
  makeUuidV4(random32_, boot_trace_id_);
  dedup_.clear();
  clock_.reset();
}

void Node::onConnected() { clock_.reset(); }

int Node::findDevice(const char* device_id) const {
  for (uint8_t index = 0; index < spec_.device_count && index < kMaxDevices; ++index) {
    if (strcmp(spec_.devices[index].device_id, device_id) == 0) return index;
  }
  return -1;
}

bool Node::isUnwired(const char* device_id) const {
  return inList(device_id, spec_.unwired_device_ids, spec_.unwired_count);
}

bool Node::applyLedOutput(uint8_t index, uint64_t now_ms) {
  DeviceState& state = devices_[index];
  const bool level = state.led.outputLevel(now_ms);
  if (state.level_written && level == state.level) return true;
  const bool ok = hal_.writeDigital(spec_.devices[index].gpio, level);
  if (ok) {
    state.level = level;
    state.level_written = true;
  }
  return ok;
}

CommandResult Node::handleCommand(const char* payload, size_t length, uint64_t received_mono_ms,
                                  const TimeNow& now, char* ack_out, size_t ack_capacity,
                                  size_t* ack_length) {
  CommandResult result = {false, false, false, AckStatus::RejectedInvalid, nullptr, -1};
  if (ack_length) *ack_length = 0;
  if (payload == nullptr || length == 0 || length > kMaxCommandBytes) return result;

  JsonDocument document;
  if (deserializeJson(document, payload, length) != DeserializationError::Ok ||
      !document.is<JsonObjectConst>()) {
    return result;
  }
  JsonObjectConst command = document.as<JsonObjectConst>();
  const char* command_id = command["command_id"].is<const char*>()
                               ? command["command_id"].as<const char*>() : nullptr;
  const char* trace_id = command["trace_id"].is<const char*>()
                             ? command["trace_id"].as<const char*>() : nullptr;
  const char* device_id = command["target_device_id"].is<const char*>()
                              ? command["target_device_id"].as<const char*>() : nullptr;
  if (!isUuid(command_id) || !isUuid(trace_id) || device_id == nullptr ||
      strlen(device_id) == 0 || strlen(device_id) > kIdMaxLength) {
    return result;
  }
  result.ackable = true;

  // 1. De-dup.
  const AckRecord* previous = dedup_.find(command_id);
  if (previous != nullptr) {
    result.duplicate = true;
    result.status = previous->status;
    result.error_code = previous->error_code;
    result.device_index = findDevice(previous->device_id);
    const size_t written = writeAck(*previous, trace_id, true, now, ack_out, ack_capacity);
    if (ack_length) *ack_length = written;
    return result;
  }

  AckRecord record;
  memset(&record, 0, sizeof(record));
  copyText(record.command_id, sizeof(record.command_id), command_id);
  copyText(record.device_id, sizeof(record.device_id), device_id);
  record.status = AckStatus::RejectedInvalid;

  Rejection rejection = kNone;
  JsonVariantConst issued = command["issued_at_ms"];
  JsonVariantConst expires = command["expires_at_ms"];
  JsonVariantConst attempt = command["attempt"];
  const char* schema = command["schema_version"].is<const char*>()
                           ? command["schema_version"].as<const char*>() : "";
  const char* node_id = command["target_node_id"].is<const char*>()
                            ? command["target_node_id"].as<const char*>() : "";
  const char* action = command["action"].is<const char*>() ? command["action"].as<const char*>() : "";

  // 2. Envelope.
  if (strcmp(schema, kSchemaVersion) != 0) {
    rejection = {"SCHEMA_VERSION_INVALID", "schema_version must be pfn.v1"};
  } else if (strcmp(node_id, spec_.node_id) != 0) {
    rejection = {"NODE_MISMATCH", "target_node_id is not this controller"};
  } else if (!issued.is<int64_t>() || !expires.is<int64_t>() || issued.as<int64_t>() < 0 ||
             expires.as<int64_t>() <= issued.as<int64_t>() || !attempt.is<int>() ||
             attempt.as<int>() < 1 || attempt.as<int>() > 3) {
    rejection = {"COMMAND_INVALID", "issued_at_ms/expires_at_ms/attempt are invalid"};
  }

  if (!isRejection(rejection)) {
    // 3. Expiry.
    const int64_t issued_ms = issued.as<int64_t>();
    const int64_t expires_ms = expires.as<int64_t>();
    clock_.observe(issued_ms, received_mono_ms);
    const ExpiryReason expiry = clock_.check(issued_ms, expires_ms, received_mono_ms, now);
    if (expiry != ExpiryReason::NotExpired) {
      record.status = AckStatus::RejectedExpired;
      record.error_code = "COMMAND_EXPIRED";
      record.error_message = expiryMessage(expiry);
    } else {
      // 4. Device / action / parameters.
      const int index = findDevice(device_id);
      result.device_index = index;
      Parsed parsed;
      if (index < 0) {
        rejection = isUnwired(device_id)
                        ? Rejection{"DEVICE_NOT_WIRED", "device is registered but not wired"}
                        : Rejection{"DEVICE_UNKNOWN", "device is not on this controller"};
      } else if (!isKnownAction(action) || !deviceSupports(spec_.devices[index].kind, action)) {
        rejection = {"CAPABILITY_NOT_ALLOWED", "action is not a capability of this device"};
      } else if (!command["parameters"].is<JsonObjectConst>()) {
        rejection = {"PARAMETERS_INVALID", "parameters must be an object"};
      } else {
        rejection = validateParameters(spec_.devices[index], action,
                                       command["parameters"].as<JsonObjectConst>(), &parsed);
      }

      if (!isRejection(rejection)) {
        // 5. Execute.
        const DeviceSpec& device = spec_.devices[index];
        DeviceState& state = devices_[index];
        JsonDocument result_document;
        bool ok = false;
        result.executed = true;
        if (device.kind == DeviceKind::Servo) {
          ok = hal_.writeServo(device.gpio, parsed.degrees);
          if (ok) state.position = static_cast<int16_t>(parsed.degrees);
          result_document["position_degrees"] = parsed.degrees;
        } else if (device.kind == DeviceKind::Switch) {
          ok = hal_.writeDigital(device.gpio, parsed.on);
          if (ok) state.on = parsed.on;
          result_document["on"] = parsed.on;
          result_document["fan_level_percent"] = parsed.on ? 100 : 0;
        } else {
          if (strcmp(action, "set_switch") == 0) {
            state.led.setSwitch(parsed.on, parsed.auto_off_seconds, now.mono_ms);
          } else {
            state.led.setColor(parsed.color_off, parsed.blink, now.mono_ms);
          }
          ok = applyLedOutput(static_cast<uint8_t>(index), now.mono_ms);
          result_document["light_on"] = state.led.power();
          result_document["light_pattern"] = state.led.pattern();
          if (state.led.autoOffArmed()) {
            result_document["auto_off_after_seconds"] = parsed.auto_off_seconds;
          }
        }
        if (ok) {
          record.status = AckStatus::Succeeded;
          finish(result_document, record.result_json, sizeof(record.result_json));
        } else {
          record.status = AckStatus::Failed;
          record.error_code = "ACTUATOR_FAILED";
          record.error_message = "output could not be driven";
        }
        state.dirty = true;
      }
    }
  }
  if (isRejection(rejection)) {
    record.status = AckStatus::RejectedInvalid;
    record.error_code = rejection.code;
    record.error_message = rejection.message;
  }

  // 6. Remember before the ACK goes out, then build it.
  const AckRecord& stored = dedup_.remember(record);
  result.status = stored.status;
  result.error_code = stored.error_code;
  const size_t written = writeAck(stored, trace_id, false, now, ack_out, ack_capacity);
  if (ack_length) *ack_length = written;
  return result;
}

size_t Node::writeAck(const AckRecord& record, const char* trace_id, bool duplicate,
                      const TimeNow& now, char* out, size_t capacity) {
  char event_id[kUuidLength + 1];
  makeUuidV4(random32_, event_id);
  JsonDocument ack;
  ack["schema_version"] = kSchemaVersion;
  ack["event_id"] = event_id;
  ack["trace_id"] = trace_id;
  ack["command_id"] = record.command_id;
  ack["node_id"] = spec_.node_id;
  ack["target_device_id"] = record.device_id;
  ack["ok"] = record.status == AckStatus::Succeeded;
  ack["duplicate"] = duplicate;
  ack["status"] = ackStatusText(record.status);
  ack["executed_at_ms"] = publishedTimeMs(now);
  if (record.result_json[0] != '\0') {
    ack["result"] = serialized(static_cast<const char*>(record.result_json));
  } else {
    ack["result"] = nullptr;
  }
  if (record.error_code != nullptr) {
    JsonObject error = ack["error"].to<JsonObject>();
    error["code"] = record.error_code;
    error["message"] = record.error_message ? record.error_message : "";
  } else {
    ack["error"] = nullptr;
  }
  return finish(ack, out, capacity);
}

void Node::tick(const TimeNow& now) {
  for (uint8_t index = 0; index < spec_.device_count && index < kMaxDevices; ++index) {
    if (spec_.devices[index].kind != DeviceKind::LedRed) continue;
    if (devices_[index].led.tick(now.mono_ms)) devices_[index].dirty = true;
    applyLedOutput(index, now.mono_ms);
  }
}

size_t Node::buildHeartbeat(const TimeNow& now, bool online, char* out, size_t capacity) {
  char event_id[kUuidLength + 1];
  makeUuidV4(random32_, event_id);
  JsonDocument heartbeat;
  heartbeat["schema_version"] = kSchemaVersion;
  heartbeat["event_id"] = event_id;
  heartbeat["trace_id"] = boot_trace_id_;
  heartbeat["node_id"] = spec_.node_id;
  heartbeat["node_type"] = spec_.node_type;
  heartbeat["status"] = online ? "online" : "offline";
  heartbeat["seq"] = heartbeat_seq_;
  heartbeat["sent_at_ms"] = publishedTimeMs(now);
  heartbeat["uptime_ms"] = now.mono_ms;
  heartbeat["version"] = spec_.firmware_version;
  const size_t written = finish(heartbeat, out, capacity);
  if (online && written > 0) ++heartbeat_seq_;
  return written;
}

size_t Node::buildTelemetry(uint8_t device_index, const TimeNow& now, char* out,
                            size_t capacity) {
  if (device_index >= spec_.device_count || device_index >= kMaxDevices) return 0;
  const DeviceSpec& device = spec_.devices[device_index];
  const DeviceState& state = devices_[device_index];
  char event_id[kUuidLength + 1];
  makeUuidV4(random32_, event_id);
  JsonDocument telemetry;
  telemetry["schema_version"] = kSchemaVersion;
  telemetry["event_id"] = event_id;
  telemetry["trace_id"] = boot_trace_id_;
  telemetry["node_id"] = spec_.node_id;
  telemetry["seq"] = telemetry_seq_;
  telemetry["sent_at_ms"] = publishedTimeMs(now);
  telemetry["uptime_ms"] = now.mono_ms;
  // Values are the commanded output state (no position/current feedback is wired).
  telemetry["sensor_ready"] = true;
  JsonObject readings = telemetry["readings"].to<JsonObject>();
  JsonObject sources = telemetry["reading_sources"].to<JsonObject>();
  switch (device.kind) {
    case DeviceKind::Servo:
      readings[device.reading_name] = state.position;
      sources[device.reading_name] = device.device_id;
      break;
    case DeviceKind::Switch:
      readings[device.reading_name] = state.on ? 100 : 0;
      sources[device.reading_name] = device.device_id;
      break;
    case DeviceKind::LedRed:
      readings["light_on"] = state.led.power();
      readings["light_pattern"] = state.led.pattern();
      sources["light_on"] = device.device_id;
      sources["light_pattern"] = device.device_id;
      break;
  }
  const size_t written = finish(telemetry, out, capacity);
  if (written > 0) ++telemetry_seq_;
  return written;
}

bool Node::takeTelemetryDirty(uint8_t device_index) {
  if (device_index >= kMaxDevices) return false;
  const bool dirty = devices_[device_index].dirty;
  devices_[device_index].dirty = false;
  return dirty;
}

}  // namespace hm
