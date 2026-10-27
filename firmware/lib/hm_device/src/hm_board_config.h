// Board configuration for the two controllers.
// Hand-written from contracts/pfn.v1/device-registry.json (registry_version 4) and the confirmed
// harness plan. tools/check_messages.py dumps this table through the native CLI and fails if any
// ID, pin, capability, servo position, colour or wiring status differs from the registry.
#pragma once

#include <stdint.h>

namespace hm {

static const int kRegistryVersion = 4;

enum class DeviceKind : uint8_t {
  Servo,   // open/close motor, set_position with the registered open/closed codes only
  Switch,  // FAN-01 (ESP8266 D2), ON/OFF only (set_level not enabled)
  LedRed,  // RGB LED module, R channel only, HIGH = on
};

struct DeviceSpec {
  const char* device_id;
  DeviceKind kind;
  const char* pin_label;     // exactly as device-registry.json "pin"
  uint8_t gpio;              // GPIO number on the board
  const char* reading_name;  // telemetry reading name (sensor-rules.json)
  int16_t closed_degrees;    // Servo only (registry positions.closed), else -1
  int16_t open_degrees;      // Servo only (registry positions.open), else -1
  bool positions_calibrated; // registry positions.calibrated
};

struct NodeSpec {
  const char* node_id;
  const char* node_type;         // heartbeat node_type
  const char* firmware_version;  // heartbeat version (<= 64 chars)
  const DeviceSpec* devices;
  uint8_t device_count;
  // Official IDs assigned to this controller in the registry but not wired
  // (wiring_status not_wired / to_confirm). Commands for them are rejected_invalid.
  const char* const* unwired_device_ids;
  uint8_t unwired_count;
};

// Colours an LedRed device accepts (registry supported_colors).
extern const char* const kLedSupportedColors[];
extern const uint8_t kLedSupportedColorCount;

extern const NodeSpec kActuatorNode;  // CTRL-ESP32-ACT-01
extern const NodeSpec kLedNode;       // CTRL-ESP8266-LED-01

// GPIOs that must never be driven on each board (boot strapping / harness notes).
extern const uint8_t kActuatorForbiddenGpios[];
extern const uint8_t kActuatorForbiddenGpioCount;
extern const uint8_t kLedForbiddenGpios[];
extern const uint8_t kLedForbiddenGpioCount;

const NodeSpec* findNodeSpec(const char* node_id);

}  // namespace hm
