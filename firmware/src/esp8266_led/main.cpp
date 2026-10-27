// CTRL-ESP8266-LED-01 (registry v4, user 2026-10-25): LED-LIVING-01 (D0), LED-AIRCON-01 (D1),
// FAN-01 (D2). HIGH = on, all LOW at power-on, like the user's tested sketch.
// D3/D4/D8 are boot pins and are never driven.
// Build/simulation only so far: not yet run on the real board.
#include <Arduino.h>

#include <hm_node.h>
#include <hm_runtime.h>

#include "hm_secrets_guard.h"

namespace {

class Esp8266LedHal : public hm::OutputHal {
 public:
  bool writeServo(uint8_t, int) override { return false; }  // no servos on this board

  bool writeDigital(uint8_t gpio, bool high) override {
    for (uint8_t index = 0; index < hm::kLedForbiddenGpioCount; ++index) {
      if (hm::kLedForbiddenGpios[index] == gpio) return false;
    }
    if (gpio >= 32) return false;
    const uint32_t bit = 1u << gpio;
    if ((configured_ & bit) == 0) {
      digitalWrite(gpio, LOW);
      pinMode(gpio, OUTPUT);
      configured_ |= bit;
    }
    digitalWrite(gpio, high ? HIGH : LOW);
    return true;
  }

 private:
  uint32_t configured_ = 0;
};

Esp8266LedHal hal;
hm::Node node(hm::kLedNode, hal, hm::Runtime::random32);
const hm::RuntimeConfig config = {HM_SITE_WIFI_SSID, HM_SITE_WIFI_PASSWORD, HM_SITE_MQTT_HOST,
                                  static_cast<uint16_t>(HM_SITE_MQTT_PORT), HM_MQTT_PASSWORD_ESP8266_LED,
                                  HM_NTP_SERVER};
hm::Runtime runtime(node, config);

}  // namespace

void setup() {
  // FIRST: every LED output low (off).
  node.boot(hm::Runtime::now());
  Serial.begin(115200);
  runtime.begin();
}

void loop() {
  runtime.loop();
  delay(1);
}
