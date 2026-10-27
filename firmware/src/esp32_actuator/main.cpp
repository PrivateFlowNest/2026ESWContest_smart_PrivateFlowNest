// CTRL-ESP32-ACT-01: SERVO-DOOR-01 (GPIO19), SERVO-WINDOW-01 (GPIO21) open/close motors.
// FAN-01 moved to CTRL-ESP8266-LED-01 (registry v4). GPIO27 is never driven.
// Build/simulation only so far: not yet run on the real board.
#include <Arduino.h>

#include <hm_node.h>
#include <hm_runtime.h>

#include "hm_secrets_guard.h"

namespace {

// Open/close motors without angle control (registry v4, user 2026-10-25). Same pulses as the user's tested
// sketch: ESP32Servo setPeriodHertz(50), attach(pin, 1000, 2000), write(HOME_ANGLE + offset); no PWM before
// the first command. The registry codes are positions.open 90 and positions.closed 0.
const int kHomeAngle = 90;
const uint32_t kServoMinPulseUs = 1000;   // write(0)
const uint32_t kServoMaxPulseUs = 2000;   // write(180)
const uint32_t kServoFrequencyHz = 50;
const uint8_t kServoResolutionBits = 16;
const uint32_t kServoPeriodUs = 1000000 / kServoFrequencyHz;

struct MotorOffsets {
  uint8_t gpio;
  int open_offset;
  int closed_offset;
};
// Sketch commands: "1,-90" door open, "1,0" door close, "2,-90" window open, "2,90" window close.
const MotorOffsets kMotors[] = {{19, -90, 0}, {21, -90, 90}};

class Esp32ActuatorHal : public hm::OutputHal {
 public:
  bool writeServo(uint8_t gpio, int degrees) override {
    const int channel = channelFor(gpio);
    const MotorOffsets* motor = motorFor(gpio);
    if (channel < 0 || motor == nullptr || (degrees != 90 && degrees != 0)) return false;
    const int angle = kHomeAngle + (degrees == 90 ? motor->open_offset : motor->closed_offset);
    const uint32_t pulse_us =
        kServoMinPulseUs + (kServoMaxPulseUs - kServoMinPulseUs) * static_cast<uint32_t>(angle) / 180u;
    const uint32_t duty =
        static_cast<uint32_t>((static_cast<uint64_t>(pulse_us) * ((1u << kServoResolutionBits) - 1u)) /
                              kServoPeriodUs);
    const int slot = channel / 2;
    if (!attached_[slot]) {
      // Set the duty BEFORE the pin is routed to the PWM channel, so the very first pulse the
      // servo sees is already the requested (closed) angle.
      if (ledcSetup(channel, kServoFrequencyHz, kServoResolutionBits) == 0) return false;
      ledcWrite(channel, duty);
      ledcAttachPin(gpio, channel);
      attached_[slot] = true;
    } else {
      ledcWrite(channel, duty);
    }
    return true;
  }

  // The sketch's setup(): digitalWrite LOW, pinMode OUTPUT, no PWM until the first command.
  void holdServoLow(uint8_t gpio) override {
    if (channelFor(gpio) < 0) return;
    digitalWrite(gpio, LOW);
    pinMode(gpio, OUTPUT);
  }

  // No digital outputs on this board since registry v4 (FAN-01 is on the ESP8266).
  bool writeDigital(uint8_t, bool) override { return false; }

 private:
  // One LEDC channel per servo; even channels so no two servos share a channel pair.
  static int channelFor(uint8_t gpio) {
    switch (gpio) {
      case 19: return 0;  // SERVO-DOOR-01
      case 21: return 2;  // SERVO-WINDOW-01
      default: return -1;
    }
  }

  static const MotorOffsets* motorFor(uint8_t gpio) {
    for (const MotorOffsets& motor : kMotors) {
      if (motor.gpio == gpio) return &motor;
    }
    return nullptr;
  }

  bool attached_[2] = {false, false};
};

Esp32ActuatorHal hal;
hm::Node node(hm::kActuatorNode, hal, hm::Runtime::random32);
const hm::RuntimeConfig config = {HM_SITE_WIFI_SSID, HM_SITE_WIFI_PASSWORD, HM_SITE_MQTT_HOST,
                                  static_cast<uint16_t>(HM_SITE_MQTT_PORT), HM_MQTT_PASSWORD_ESP32_ACT,
                                  HM_NTP_SERVER};
hm::Runtime runtime(node, config);

}  // namespace

void setup() {
  // FIRST: servos to their closed angle (power-on twitch mitigation), then fan off.
  node.boot(hm::Runtime::now());
  Serial.begin(115200);
  runtime.begin();
}

void loop() {
  runtime.loop();
  delay(1);
}
