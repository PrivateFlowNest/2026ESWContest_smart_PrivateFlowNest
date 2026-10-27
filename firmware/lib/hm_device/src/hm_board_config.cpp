#include "hm_board_config.h"

#include <string.h>

namespace hm {

const char* const kLedSupportedColors[] = {"off", "red"};
const uint8_t kLedSupportedColorCount = 2;

// ---------------------------------------------------------------------------------------------
// CTRL-ESP32-ACT-01 (ESP32 DevKit). Registry v4 (user 2026-10-25): 19 door1, 21 wind1 (open/close
// motors) only; the bathroom door (was GPIO25) is removed and FAN-01 moved to the ESP8266. Order of this
// table is the boot order: servo signals are held low first, nothing moves until a command.
// ---------------------------------------------------------------------------------------------
static const DeviceSpec kActuatorDevices[] = {
    {"SERVO-DOOR-01", DeviceKind::Servo, "GPIO19", 19, "door_position_degrees", 0, 90, true},
    {"SERVO-WINDOW-01", DeviceKind::Servo, "GPIO21", 21, "window_position_degrees", 0, 90, true},
};

static const char* const kActuatorUnwired[] = {"SERVO-DOOR-02", "SERVO-BED-CURTAIN-01"};

const NodeSpec kActuatorNode = {
    "CTRL-ESP32-ACT-01", "esp32", "esp32-act-1.0.0",
    kActuatorDevices, sizeof(kActuatorDevices) / sizeof(kActuatorDevices[0]),
    kActuatorUnwired, sizeof(kActuatorUnwired) / sizeof(kActuatorUnwired[0]),
};

// 0/2/12/15 boot strapping, 14 unused, 27 spare-servo slot left empty, 34-39 input only.
const uint8_t kActuatorForbiddenGpios[] = {0, 2, 12, 14, 15, 27, 34, 35, 36, 37, 38, 39};
// GPIO32 (FAN-01 until registry v4) is no longer driven: no device of this board uses it.
const uint8_t kActuatorForbiddenGpioCount = sizeof(kActuatorForbiddenGpios);

// ---------------------------------------------------------------------------------------------
// CTRL-ESP8266-LED-01 (NodeMCU v2). Registry v4 (user 2026-10-25, the user's tested sketch): D0 living
// light, D1 air conditioner indicator, D2 ventilation fan; HIGH = on, all LOW at power-on.
// NodeMCU labels: D0=GPIO16, D1=GPIO5, D2=GPIO4.
// D3 (GPIO0), D4 (GPIO2), D8 (GPIO15) are boot pins and are never driven.
// ---------------------------------------------------------------------------------------------
static const DeviceSpec kLedDevices[] = {
    {"LED-LIVING-01", DeviceKind::LedRed, "D0", 16, "light_on", -1, -1, false},
    {"LED-AIRCON-01", DeviceKind::LedRed, "D1", 5, "light_on", -1, -1, false},
    {"FAN-01", DeviceKind::Switch, "D2", 4, "fan_level_percent", -1, -1, false},
};

// Not in the model (registry v4): official IDs kept, commands rejected as not wired.
static const char* const kLedUnwired[] = {"LED-WASHER-01", "LED-TV-01", "LED-LAPTOP-01", "LED-GUIDE-01",
                                          "FND-WASHER-01", "FND-AIRCON-01", "FND-TV-01"};

const NodeSpec kLedNode = {
    "CTRL-ESP8266-LED-01", "esp8266", "esp8266-led-1.0.0",
    kLedDevices, sizeof(kLedDevices) / sizeof(kLedDevices[0]),
    kLedUnwired, sizeof(kLedUnwired) / sizeof(kLedUnwired[0]),
};

const uint8_t kLedForbiddenGpios[] = {0, 2, 15};
const uint8_t kLedForbiddenGpioCount = sizeof(kLedForbiddenGpios);

const NodeSpec* findNodeSpec(const char* node_id) {
  if (node_id == nullptr) return nullptr;
  if (strcmp(node_id, kActuatorNode.node_id) == 0) return &kActuatorNode;
  if (strcmp(node_id, kLedNode.node_id) == 0) return &kLedNode;
  return nullptr;
}

}  // namespace hm
