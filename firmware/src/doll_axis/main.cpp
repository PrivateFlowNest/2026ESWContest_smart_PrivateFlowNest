// Doll axis controller: one STEP/DIR stepper (front/back) + one optical home sensor, driven by a
// host over USB serial with the line protocol of lib/hm_axis (hm_axis_protocol.h).
// Boards: Teensy 4.1 (env teensy41_axis) or ESP32 DevKit (env esp32_axis). Pins and mechanics:
// include/doll_axis_config.h (placeholders until the hardware is decided).
// Standalone: no Wi-Fi, no MQTT, no HomeMind contract (integration is undecided).
// Build only so far: never flashed or run on a board.
#include <Arduino.h>

#include <hm_axis.h>
#include <hm_axis_protocol.h>

#include "doll_axis_config.h"

namespace {

using namespace doll_axis;

class BoardAxisHal : public hm::axis::AxisHal {
 public:
  // Safe pin state before anything else: driver disabled, STEP and DIR low, sensor with pull-up.
  void begin(bool enable_active_low) {
    const uint8_t disabled = enable_active_low ? HIGH : LOW;
    digitalWrite(kPinEnable, disabled);  // ESP32: latch the level before the pin becomes an output
    pinMode(kPinEnable, OUTPUT);
    digitalWrite(kPinEnable, disabled);
    digitalWrite(kPinStep, LOW);
    pinMode(kPinStep, OUTPUT);
    digitalWrite(kPinStep, LOW);
    digitalWrite(kPinDir, LOW);
    pinMode(kPinDir, OUTPUT);
    digitalWrite(kPinDir, LOW);
    pinMode(kPinEndstop, INPUT_PULLUP);
  }

  void writeDir(bool high) override { digitalWrite(kPinDir, high ? HIGH : LOW); }

  void pulseStep(uint16_t width_us) override {
    digitalWrite(kPinStep, HIGH);
    delayMicroseconds(width_us);
    digitalWrite(kPinStep, LOW);
  }

  void writeEnable(bool high) override { digitalWrite(kPinEnable, high ? HIGH : LOW); }

  bool readEndstop() override { return digitalRead(kPinEndstop) == HIGH; }

  uint32_t nowUs() override { return static_cast<uint32_t>(micros()); }
};

class SerialSink : public hm::axis::LineSink {
 public:
  void writeLine(const char* line) override {
#if defined(HM_BOARD_TEENSY41_AXIS)
    // USB serial: with no host port open, drop the line instead of waiting for a reader (a
    // blocked write would stall the step timing).
    if (!Serial) return;
#endif
    Serial.write(line, strlen(line));
    Serial.write('\n');
  }
};

BoardAxisHal hal;
SerialSink sink;
hm::axis::Axis axis(hal);
hm::axis::Controller controller(axis, sink, kFirmwareName);

// Received bytes handled per loop pass: keeps the step timing tight while a line arrives.
const int kBytesPerPass = 8;

}  // namespace

void setup() {
  hal.begin(kAxisConfig.enable_active_low);  // FIRST: driver disabled
#if defined(HM_BOARD_ESP32_AXIS)
  Serial.setTxBufferSize(1024);  // replies never wait for the UART FIFO
#endif
  Serial.begin(115200);
  axis.begin(kAxisConfig);
  controller.begin(kNamedPositions, kNamedPositionCount);
}

void loop() {
  axis.tick();
  for (int count = 0; count < kBytesPerPass && Serial.available() > 0; ++count) {
    controller.feed(static_cast<char>(Serial.read()));
  }
  controller.poll();
}
