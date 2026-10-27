#include "hm_runtime.h"

#include <MQTT.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#if defined(ESP32)
#include <WiFi.h>
#include <esp_system.h>
#include <esp_timer.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#error "hm_runtime supports ESP32 and ESP8266 only"
#endif

namespace hm {

namespace {

const uint16_t kMqttKeepAliveSeconds = 10;  // broker fires the Last Will ~15 s after power loss
const uint16_t kMqttTimeoutMs = 2000;
const uint32_t kMqttRetryMs = 3000;
const uint32_t kWifiRestartMs = 20000;
const int kMqttBufferBytes = 2048;
const int64_t kMinimumValidEpochSeconds = 1704067200;  // 2024-01-01: anything earlier = not synced
const uint8_t kQueueSlots = 4;

struct QueuedCommand {
  bool used;
  uint64_t received_mono_ms;
  size_t length;
  char payload[kMaxCommandBytes];
};

WiFiClient network;
MQTTClient mqtt(kMqttBufferBytes);
QueuedCommand queue[kQueueSlots];
const char* subscribed_topic = nullptr;
uint32_t dropped_commands = 0;

char client_id[96];
char out_buffer[kMessageBufferBytes];
char will_buffer[kMessageBufferBytes];

bool configured = false;
bool wifi_was_connected = false;
bool mqtt_was_connected = false;
uint32_t last_mqtt_attempt_ms = 0;
uint32_t wifi_started_ms = 0;
uint32_t last_heartbeat_ms = 0;
uint32_t last_telemetry_ms = 0;
bool clock_logged = false;

void onMessage(MQTTClient*, char topic[], char bytes[], int length) {
  if (subscribed_topic == nullptr || strcmp(topic, subscribed_topic) != 0) return;
  if (length <= 0 || static_cast<size_t>(length) > kMaxCommandBytes) {
    ++dropped_commands;
    Serial.printf("[cmd] dropped: payload length %d outside 1..%u\n", length,
                  static_cast<unsigned>(kMaxCommandBytes));
    return;
  }
  for (uint8_t slot = 0; slot < kQueueSlots; ++slot) {
    if (queue[slot].used) continue;
    memcpy(queue[slot].payload, bytes, static_cast<size_t>(length));
    queue[slot].length = static_cast<size_t>(length);
    queue[slot].received_mono_ms = Runtime::now().mono_ms;
    queue[slot].used = true;
    return;
  }
  ++dropped_commands;
  Serial.println("[cmd] dropped: queue full (core will retry)");
}

bool publish(const char* topic, const char* payload, size_t length, int qos) {
  if (length == 0) {
    Serial.printf("[mqtt] message for %s did not fit the buffer\n", topic);
    return false;
  }
  const bool ok = mqtt.publish(topic, payload, static_cast<int>(length), kRetained, qos);
  if (!ok) Serial.printf("[mqtt] publish to %s failed (err %d)\n", topic, mqtt.lastError());
  return ok;
}

bool hasText(const char* value) { return value != nullptr && value[0] != '\0'; }

}  // namespace

TimeNow Runtime::now() {
  TimeNow now;
#if defined(ESP32)
  now.mono_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
#else
  now.mono_ms = micros64() / 1000;
#endif
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  now.wall_valid = static_cast<int64_t>(tv.tv_sec) >= kMinimumValidEpochSeconds;
  now.wall_ms = now.wall_valid
                    ? static_cast<int64_t>(tv.tv_sec) * 1000 + static_cast<int64_t>(tv.tv_usec / 1000)
                    : 0;
  return now;
}

uint32_t Runtime::random32() {
#if defined(ESP32)
  return esp_random();
#else
  return RANDOM_REG32;
#endif
}

Runtime::Runtime(Node& node, const RuntimeConfig& config) : node_(node), config_(config) {}

void Runtime::begin() {
  memset(queue, 0, sizeof(queue));
  snprintf(client_id, sizeof(client_id), "%s-runtime", node_.spec().node_id);
  subscribed_topic = node_.commandTopic();
  Serial.printf("\n[boot] %s %s, registry v%d, outputs in safe state\n", node_.spec().node_id,
                node_.spec().firmware_version, kRegistryVersion);

  configured = hasText(config_.wifi_ssid) && hasText(config_.mqtt_host) && config_.mqtt_port > 0 &&
               hasText(config_.mqtt_password);
  if (!configured) {
    // Never print the values themselves, only which one is missing.
    Serial.printf("[boot] network/secrets incomplete (ssid:%s host:%s port:%s password:%s); staying offline\n",
                  hasText(config_.wifi_ssid) ? "set" : "EMPTY",
                  hasText(config_.mqtt_host) ? "set" : "EMPTY",
                  config_.mqtt_port > 0 ? "set" : "EMPTY",
                  hasText(config_.mqtt_password) ? "set" : "EMPTY");
    return;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
#if defined(ESP32)
  WiFi.setSleep(false);
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
  WiFi.begin(config_.wifi_ssid, config_.wifi_password);
  wifi_started_ms = millis();

  if (hasText(config_.ntp_server)) {
    configTime(0, 0, config_.ntp_server);
    Serial.println("[time] SNTP configured (server from secrets.h)");
  } else {
    Serial.println("[time] no SNTP server: sent_at_ms/executed_at_ms = 0, expiry by hold time");
  }

  mqtt.begin(config_.mqtt_host, config_.mqtt_port, network);
  mqtt.setOptions(kMqttKeepAliveSeconds, true, kMqttTimeoutMs);
  mqtt.dropOverflow(true);
  mqtt.onMessageAdvanced(onMessage);
}

void Runtime::loop() {
  TimeNow now = Runtime::now();
  node_.tick(now);
  if (!configured) return;

  if (!clock_logged && now.wall_valid) {
    clock_logged = true;
    Serial.println("[time] wall clock synced");
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifi_was_connected) {
      Serial.println("[wifi] lost; outputs keep their state, reconnecting");
      wifi_was_connected = false;
      wifi_started_ms = millis();
    }
    mqtt_was_connected = false;
    if (millis() - wifi_started_ms >= kWifiRestartMs) {
      Serial.println("[wifi] still not connected, restarting association");
      WiFi.disconnect();
      WiFi.begin(config_.wifi_ssid, config_.wifi_password);
      wifi_started_ms = millis();
    }
    return;
  }
  if (!wifi_was_connected) {
    wifi_was_connected = true;
    Serial.printf("[wifi] connected, local ip %s, rssi %d\n", WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  }

  if (!mqtt.connected()) {
    if (mqtt_was_connected) {
      Serial.printf("[mqtt] disconnected (err %d); reconnecting\n", mqtt.lastError());
      mqtt_was_connected = false;
    }
    if (last_mqtt_attempt_ms != 0 && millis() - last_mqtt_attempt_ms < kMqttRetryMs) return;
    last_mqtt_attempt_ms = millis();
    if (last_mqtt_attempt_ms == 0) last_mqtt_attempt_ms = 1;

    const size_t will_length = node_.buildHeartbeat(now, false, will_buffer, sizeof(will_buffer));
    if (will_length > 0) mqtt.setWill(Node::heartbeatTopic(), will_buffer, kRetained, kQosTelemetryHeartbeat);
    if (!mqtt.connect(client_id, node_.spec().node_id, config_.mqtt_password)) {
      Serial.printf("[mqtt] connect failed (err %d, rc %d)\n", mqtt.lastError(), mqtt.returnCode());
      return;
    }
    if (!mqtt.subscribe(node_.commandTopic(), kQosCommandAck)) {
      Serial.printf("[mqtt] subscribe failed (err %d)\n", mqtt.lastError());
      mqtt.disconnect();
      return;
    }
    mqtt_was_connected = true;
    node_.onConnected();
    memset(queue, 0, sizeof(queue));
    Serial.printf("[mqtt] connected as %s, subscribed to %s\n", client_id, node_.commandTopic());
    now = Runtime::now();
    publish(Node::heartbeatTopic(), out_buffer,
            node_.buildHeartbeat(now, true, out_buffer, sizeof(out_buffer)), kQosTelemetryHeartbeat);
    last_heartbeat_ms = millis();
    for (uint8_t index = 0; index < node_.spec().device_count; ++index) {
      node_.takeTelemetryDirty(index);
      publish(node_.telemetryTopic(), out_buffer,
              node_.buildTelemetry(index, now, out_buffer, sizeof(out_buffer)),
              kQosTelemetryHeartbeat);
    }
    last_telemetry_ms = millis();
  }

  mqtt.loop();

  for (uint8_t slot = 0; slot < kQueueSlots; ++slot) {
    if (!queue[slot].used) continue;
    now = Runtime::now();
    size_t ack_length = 0;
    const CommandResult result =
        node_.handleCommand(queue[slot].payload, queue[slot].length, queue[slot].received_mono_ms,
                            now, out_buffer, sizeof(out_buffer), &ack_length);
    queue[slot].used = false;
    if (!result.ackable) {
      Serial.println("[cmd] dropped: not JSON or missing command_id/trace_id/target_device_id");
      continue;
    }
    Serial.printf("[cmd] %s%s%s%s\n", ackStatusText(result.status),
                  result.duplicate ? " (duplicate, not executed)" : "",
                  result.error_code ? " " : "", result.error_code ? result.error_code : "");
    publish(node_.ackTopic(), out_buffer, ack_length, kQosCommandAck);
  }

  now = Runtime::now();
  if (millis() - last_heartbeat_ms >= kHeartbeatIntervalMs) {
    last_heartbeat_ms = millis();
    publish(Node::heartbeatTopic(), out_buffer,
            node_.buildHeartbeat(now, true, out_buffer, sizeof(out_buffer)), kQosTelemetryHeartbeat);
  }
  const bool periodic = millis() - last_telemetry_ms >= kTelemetryIntervalMs;
  if (periodic) last_telemetry_ms = millis();
  for (uint8_t index = 0; index < node_.spec().device_count; ++index) {
    const bool dirty = node_.takeTelemetryDirty(index);
    if (!dirty && !periodic) continue;
    publish(node_.telemetryTopic(), out_buffer,
            node_.buildTelemetry(index, now, out_buffer, sizeof(out_buffer)), kQosTelemetryHeartbeat);
  }
}

}  // namespace hm
