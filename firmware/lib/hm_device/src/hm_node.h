// Hardware-independent controller logic: command validation, de-dup, expiry, execution through an
// OutputHal, ACK / heartbeat / telemetry JSON. Used unchanged by both boards and by the native
// tests and CLI.
//
// Command handling order (team ESP32_HANDOFF_KO.md + system-architecture 6-1):
//   0. parse JSON; without a UUID command_id/trace_id and a target_device_id no ACK can be built,
//      so the message is dropped (logged by the board)
//   1. command_id already in the de-dup ring  -> re-send stored outcome with duplicate:true,
//                                                 nothing is executed
//   2. schema_version / target_node_id / times / attempt invalid -> rejected_invalid
//   3. expired (see hm_time.h)                 -> rejected_expired
//   4. device / action / parameters invalid    -> rejected_invalid (core's failure codes)
//   5. execute through the HAL                 -> succeeded, or failed if the HAL reports an error
//   6. remember the outcome, then build the ACK
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hm_board_config.h"
#include "hm_dedup.h"
#include "hm_led.h"
#include "hm_protocol.h"
#include "hm_time.h"
#include "hm_uuid.h"

namespace hm {

class OutputHal {
 public:
  virtual ~OutputHal() {}
  // Drive a servo to a registered position (registry positions.open / positions.closed). Returns false if the
  // output could not be driven.
  virtual bool writeServo(uint8_t gpio, int degrees) = 0;
  // Hold a servo signal low without pulses: nothing moves until the first command. Boards without servos ignore it.
  virtual void holdServoLow(uint8_t gpio) { static_cast<void>(gpio); }
  // Drive a digital output. Returns false if the output could not be driven.
  virtual bool writeDigital(uint8_t gpio, bool high) = 0;
};

struct CommandResult {
  bool ackable;           // false: message dropped, no ACK
  bool duplicate;         // true: stored outcome re-sent, nothing executed
  bool executed;          // true: the HAL was driven for this command
  AckStatus status;
  const char* error_code; // nullptr on success
  int device_index;       // index in NodeSpec::devices, -1 if not a local device
};

class Node {
 public:
  static const uint8_t kMaxDevices = 8;

  Node(const NodeSpec& spec, OutputHal& hal, Random32 random32);

  // Safe initial outputs. Servos FIRST, in table order, to their closed angle (power-on twitch
  // mitigation), then switches off, then LEDs off. Call as the very first thing in setup().
  void boot(const TimeNow& now);

  // A new MQTT session started: reset the core clock floor (hm_time.h).
  void onConnected();

  CommandResult handleCommand(const char* payload, size_t length, uint64_t received_mono_ms,
                              const TimeNow& now, char* ack_out, size_t ack_capacity,
                              size_t* ack_length);

  // Blink edges and auto-off. Call often (every loop).
  void tick(const TimeNow& now);

  // online=true: periodic heartbeat (seq increments). online=false: Last Will payload
  // (status offline, same seq, not incremented).
  size_t buildHeartbeat(const TimeNow& now, bool online, char* out, size_t capacity);
  // One telemetry message per device (readings is a flat name->value map, so two devices that
  // share a reading name cannot be reported in one message).
  size_t buildTelemetry(uint8_t device_index, const TimeNow& now, char* out, size_t capacity);
  // True once after the device state changed (command or auto-off).
  bool takeTelemetryDirty(uint8_t device_index);

  const NodeSpec& spec() const { return spec_; }
  const char* bootTraceId() const { return boot_trace_id_; }
  const char* commandTopic() const { return command_topic_; }
  const char* ackTopic() const { return ack_topic_; }
  const char* telemetryTopic() const { return telemetry_topic_; }
  static const char* heartbeatTopic() { return "pfn/v1/system/heartbeat"; }

  int16_t servoPosition(uint8_t index) const { return devices_[index].position; }
  bool switchOn(uint8_t index) const { return devices_[index].on; }
  const LedState& led(uint8_t index) const { return devices_[index].led; }
  bool outputLevel(uint8_t index) const { return devices_[index].level; }

 private:
  struct DeviceState {
    int16_t position;
    bool on;
    LedState led;
    bool level;
    bool level_written;
    bool dirty;
  };

  int findDevice(const char* device_id) const;
  bool isUnwired(const char* device_id) const;
  bool applyLedOutput(uint8_t index, uint64_t now_ms);
  size_t writeAck(const AckRecord& record, const char* trace_id, bool duplicate, const TimeNow& now,
                  char* out, size_t capacity);

  const NodeSpec& spec_;
  OutputHal& hal_;
  Random32 random32_;
  DeviceState devices_[kMaxDevices];
  DedupRing dedup_;
  CommandClock clock_;
  uint32_t heartbeat_seq_;
  uint32_t telemetry_seq_;
  char boot_trace_id_[kUuidLength + 1];
  char command_topic_[96];
  char ack_topic_[96];
  char telemetry_topic_[96];
};

}  // namespace hm
