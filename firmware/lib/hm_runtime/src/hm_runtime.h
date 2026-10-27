// Arduino runtime shared by both controllers: Wi-Fi, MQTT (256dpi/arduino-mqtt), SNTP, timers.
// All decisions live in hm::Node (lib/hm_device); this file only moves bytes and keeps time.
//
// MQTT: username = node ID (mosquitto.acl), client ID = node ID + "-runtime", clean session,
// keepalive 10 s, command subscribe QoS 1, ACK QoS 1, telemetry/heartbeat QoS 0, never retained.
// Last Will = heartbeat with status "offline" (built at connect time, QoS 0, not retained).
// Received commands are copied into a small queue in the MQTT callback and processed from loop()
// (arduino-mqtt does not allow publishing from inside its callback).
#pragma once

#if !defined(ARDUINO)
#error "hm_runtime is Arduino-only; the native env uses lib/hm_device directly"
#endif

#include <Arduino.h>
#include <hm_node.h>

namespace hm {

struct RuntimeConfig {
  const char* wifi_ssid;
  const char* wifi_password;
  const char* mqtt_host;
  uint16_t mqtt_port;
  const char* mqtt_password;
  const char* ntp_server;  // "" = no SNTP
};

class Runtime {
 public:
  Runtime(Node& node, const RuntimeConfig& config);
  void begin();
  void loop();

  static TimeNow now();
  static uint32_t random32();

 private:
  Node& node_;
  RuntimeConfig config_;
};

}  // namespace hm
