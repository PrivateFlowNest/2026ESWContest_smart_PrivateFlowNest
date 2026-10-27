// Unit tests for lib/hm_device (pio test -e native). PC only: no hardware is involved.
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <hm_node.h>

using namespace hm;

namespace {

struct HalEvent {
  char kind;  // 'S' servo, 'L' servo signal held low, 'D' digital
  uint8_t gpio;
  int value;
};

class FakeHal : public OutputHal {
 public:
  FakeHal() : count(0), fail_servo(false) {}
  bool writeServo(uint8_t gpio, int degrees) override {
    record('S', gpio, degrees);
    return !fail_servo;
  }
  void holdServoLow(uint8_t gpio) override { record('L', gpio, 0); }
  bool writeDigital(uint8_t gpio, bool high) override {
    record('D', gpio, high ? 1 : 0);
    return true;
  }
  void clear() { count = 0; }
  int countServo(uint8_t gpio) const {
    int total = 0;
    for (int index = 0; index < count; ++index) total += events[index].kind == 'S' && events[index].gpio == gpio;
    return total;
  }
  int countDigital(uint8_t gpio) const {
    int total = 0;
    for (int index = 0; index < count; ++index) total += events[index].kind == 'D' && events[index].gpio == gpio;
    return total;
  }
  bool touched(uint8_t gpio) const {
    for (int index = 0; index < count; ++index) if (events[index].gpio == gpio) return true;
    return false;
  }

  HalEvent events[256];
  int count;
  bool fail_servo;

 private:
  void record(char kind, uint8_t gpio, int value) {
    if (count < 256) events[count++] = {kind, gpio, value};
  }
};

uint32_t rng_state = 0x12345678u;
uint32_t testRandom() {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

const int64_t kIssued = 1788278400000LL;
const char kTrace[] = "22222222-2222-4222-8222-222222222222";

TimeNow at(uint64_t mono, bool wall_valid = false, int64_t wall = 0) {
  TimeNow now;
  now.mono_ms = mono;
  now.wall_valid = wall_valid;
  now.wall_ms = wall;
  return now;
}

void commandId(int number, char out[37]) {
  snprintf(out, 37, "44444444-4444-4444-8444-%012d", number);
}

struct Cmd {
  const char* node;
  const char* device;
  const char* action;
  const char* params;
  int64_t issued;
  int64_t expires;
  int attempt;
  const char* schema;
  int id;
};

Cmd cmd(const char* node, const char* device, const char* action, const char* params, int id) {
  Cmd value = {node, device, action, params, kIssued, kIssued + 10000, 1, "pfn.v1", id};
  return value;
}

size_t render(const Cmd& c, char* out, size_t capacity) {
  char id[37];
  commandId(c.id, id);
  return static_cast<size_t>(snprintf(
      out, capacity,
      "{\"schema_version\":\"%s\",\"event_id\":\"11111111-1111-4111-8111-111111111111\","
      "\"trace_id\":\"%s\",\"task_id\":null,\"command_id\":\"%s\",\"target_node_id\":\"%s\","
      "\"target_device_id\":\"%s\",\"action\":\"%s\",\"parameters\":%s,"
      "\"issued_at_ms\":%lld,\"expires_at_ms\":%lld,\"attempt\":%d}",
      c.schema, kTrace, id, c.node, c.device, c.action, c.params,
      static_cast<long long>(c.issued), static_cast<long long>(c.expires), c.attempt));
}

struct Run {
  CommandResult result;
  JsonDocument ack;
  size_t length;
};

FakeHal hal;
Node* actuator = nullptr;
Node* leds = nullptr;
const char ACT[] = "CTRL-ESP32-ACT-01";
const char LED[] = "CTRL-ESP8266-LED-01";

void send(Node& node, const Cmd& c, uint64_t received, const TimeNow& now, Run* run) {
  char payload[1024];
  const size_t length = render(c, payload, sizeof(payload));
  char ack[1024];
  run->length = 0;
  run->result = node.handleCommand(payload, length, received, now, ack, sizeof(ack), &run->length);
  run->ack.clear();
  if (run->length > 0) {
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(run->ack, ack, run->length).code());
  }
}

void sendAt(Node& node, const Cmd& c, uint64_t mono, Run* run) { send(node, c, mono, at(mono), run); }

void expectStatus(const Run& run, const char* status, const char* code) {
  TEST_ASSERT_TRUE(run.result.ackable);
  TEST_ASSERT_TRUE(run.length > 0);
  TEST_ASSERT_EQUAL_STRING(status, run.ack["status"].as<const char*>());
  TEST_ASSERT_EQUAL(strcmp(status, "succeeded") == 0, run.ack["ok"].as<bool>());
  if (code == nullptr) {
    TEST_ASSERT_TRUE(run.ack["error"].isNull());
  } else {
    TEST_ASSERT_EQUAL_STRING(code, run.ack["error"]["code"].as<const char*>());
    TEST_ASSERT_TRUE(run.ack["result"].isNull());
  }
}

}  // namespace

void setUp() {
  actuator = new Node(kActuatorNode, hal, testRandom);
  leds = new Node(kLedNode, hal, testRandom);
  hal.clear();
  hal.fail_servo = false;
  actuator->boot(at(0));
  leds->boot(at(0));
  hal.clear();
}

void tearDown() {
  delete actuator;
  delete leds;
  actuator = nullptr;
  leds = nullptr;
}

// ---------------------------------------------------------------- boot order

void test_boot_holds_servo_signals_low_first() {
  FakeHal local;
  Node node(kActuatorNode, local, testRandom);
  TEST_ASSERT_EQUAL(0, local.count);  // constructor touches no output
  node.boot(at(0));
  // Open/close motors (registry v4): signal held low, no pulses, nothing moves until a command.
  const uint8_t servo_gpios[] = {19, 21};
  for (int index = 0; index < 2; ++index) {
    TEST_ASSERT_EQUAL('L', local.events[index].kind);
    TEST_ASSERT_EQUAL(servo_gpios[index], local.events[index].gpio);
    TEST_ASSERT_EQUAL(0, node.servoPosition(index));  // assumed closed (registry positions.closed)
  }
  TEST_ASSERT_EQUAL(2, local.count);  // FAN-01 is on the ESP8266 since registry v4
  TEST_ASSERT_EQUAL(0, local.countServo(19) + local.countServo(21));
  for (uint8_t index = 0; index < kActuatorForbiddenGpioCount; ++index) {
    TEST_ASSERT_FALSE(local.touched(kActuatorForbiddenGpios[index]));  // includes GPIO27
  }
}

void test_boot_turns_every_led_off_and_skips_boot_pins() {
  FakeHal local;
  Node node(kLedNode, local, testRandom);
  node.boot(at(0));
  const uint8_t gpios[] = {4, 16, 5};  // fan D2 first (switches), then LEDs D0 D1
  TEST_ASSERT_EQUAL(3, local.count);
  for (int index = 0; index < 3; ++index) {
    TEST_ASSERT_EQUAL('D', local.events[index].kind);
    TEST_ASSERT_EQUAL(gpios[index], local.events[index].gpio);
    TEST_ASSERT_EQUAL(0, local.events[index].value);
  }
  TEST_ASSERT_FALSE(local.touched(0));   // D3
  TEST_ASSERT_FALSE(local.touched(2));   // D4
  TEST_ASSERT_FALSE(local.touched(15));  // D8
}

// ---------------------------------------------------------------- normal / duplicate

void test_servo_open_succeeds_with_ack_fields() {
  Run run;
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90}", 1), 100, &run);
  expectStatus(run, "succeeded", nullptr);
  char id[37];
  commandId(1, id);
  TEST_ASSERT_EQUAL_STRING(id, run.ack["command_id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(kTrace, run.ack["trace_id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(ACT, run.ack["node_id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("SERVO-DOOR-01", run.ack["target_device_id"].as<const char*>());
  TEST_ASSERT_FALSE(run.ack["duplicate"].as<bool>());
  TEST_ASSERT_EQUAL(90, run.ack["result"]["position_degrees"].as<int>());
  TEST_ASSERT_TRUE(isUuid(run.ack["event_id"].as<const char*>()));
  TEST_ASSERT_TRUE(run.ack["executed_at_ms"].as<long long>() == 0);  // no wall clock
  TEST_ASSERT_EQUAL(1, hal.countServo(19));
  TEST_ASSERT_EQUAL(90, actuator->servoPosition(0));
}

void test_duplicate_command_is_not_executed_twice() {
  Run first;
  Run second;
  Cmd c = cmd(ACT, "SERVO-WINDOW-01", "set_position", "{\"position_degrees\":90}", 2);
  sendAt(*actuator, c, 100, &first);
  expectStatus(first, "succeeded", nullptr);
  c.attempt = 2;  // core retry: same command_id, attempt incremented
  sendAt(*actuator, c, 3200, &second);
  expectStatus(second, "succeeded", nullptr);
  TEST_ASSERT_TRUE(second.result.duplicate);
  TEST_ASSERT_FALSE(second.result.executed);
  TEST_ASSERT_TRUE(second.ack["duplicate"].as<bool>());
  TEST_ASSERT_EQUAL(90, second.ack["result"]["position_degrees"].as<int>());
  TEST_ASSERT_EQUAL(1, hal.countServo(21));
  TEST_ASSERT_TRUE(strcmp(first.ack["event_id"].as<const char*>(),
                          second.ack["event_id"].as<const char*>()) != 0);
}

void test_duplicate_after_expiry_still_returns_stored_success() {
  Run first;
  Run late;
  Cmd c = cmd(LED, "FAN-01", "set_switch", "{\"on\":true}", 3);
  sendAt(*leds, c, 100, &first);
  expectStatus(first, "succeeded", nullptr);
  c.attempt = 3;
  send(*leds, c, 20000, at(20000, true, kIssued + 20000), &late);
  expectStatus(late, "succeeded", nullptr);
  TEST_ASSERT_TRUE(late.ack["duplicate"].as<bool>());
  TEST_ASSERT_EQUAL(1, hal.countDigital(4));
}

void test_duplicate_of_rejected_command_keeps_rejection() {
  Run first;
  Run second;
  Cmd c = cmd(ACT, "SERVO-WINDOW-01", "set_position", "{\"position_degrees\":45}", 4);
  sendAt(*actuator, c, 100, &first);
  expectStatus(first, "rejected_invalid", "POSITION_NOT_REGISTERED");
  sendAt(*actuator, c, 200, &second);
  expectStatus(second, "rejected_invalid", "POSITION_NOT_REGISTERED");
  TEST_ASSERT_TRUE(second.ack["duplicate"].as<bool>());
  TEST_ASSERT_EQUAL(0, hal.count);
}

void test_dedup_ring_keeps_last_16() {
  Run run;
  for (int number = 0; number < 17; ++number) {
    sendAt(*leds, cmd(LED, "FAN-01", "set_switch", number % 2 ? "{\"on\":true}" : "{\"on\":false}", 100 + number),
           100 + number, &run);
    expectStatus(run, "succeeded", nullptr);
  }
  // Number 116 is still remembered; 100 was evicted (capacity 16).
  sendAt(*leds, cmd(LED, "FAN-01", "set_switch", "{\"on\":false}", 116), 200, &run);
  TEST_ASSERT_TRUE(run.result.duplicate);
  sendAt(*leds, cmd(LED, "FAN-01", "set_switch", "{\"on\":false}", 100), 201, &run);
  TEST_ASSERT_FALSE(run.result.duplicate);
}

// ---------------------------------------------------------------- expiry

void test_expired_by_wall_clock() {
  Run run;
  send(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90}", 10), 100,
       at(100, true, kIssued + 10000), &run);
  expectStatus(run, "rejected_expired", "COMMAND_EXPIRED");
  TEST_ASSERT_EQUAL(0, hal.count);
  TEST_ASSERT_TRUE(run.ack["executed_at_ms"].as<long long>() == kIssued + 10000);
}

void test_fresh_by_wall_clock() {
  Run run;
  send(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":0}", 11), 100,
       at(100, true, kIssued + 9999), &run);
  expectStatus(run, "succeeded", nullptr);
}

void test_expired_by_hold_time_without_wall_clock() {
  Run run;
  send(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90}", 12), 1000,
       at(1000 + 10000), &run);
  expectStatus(run, "rejected_expired", "COMMAND_EXPIRED");
  TEST_ASSERT_EQUAL(0, hal.count);
}

void test_ttl_is_capped_at_protocol_command_ttl() {
  Run run;
  Cmd c = cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90}", 13);
  c.expires = kIssued + 600000;  // claims 10 minutes
  send(*actuator, c, 1000, at(1000 + kCommandTtlMs), &run);
  expectStatus(run, "rejected_expired", "COMMAND_EXPIRED");
  Cmd d = cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90}", 14);
  d.expires = kIssued + 600000;
  send(*actuator, d, 1000, at(1000, true, kIssued + kCommandTtlMs), &run);
  expectStatus(run, "rejected_expired", "COMMAND_EXPIRED");
}

void test_stale_command_after_newer_one_is_expired_by_core_clock_floor() {
  Run run;
  sendAt(*actuator, cmd(ACT, "SERVO-WINDOW-01", "set_position", "{\"position_degrees\":0}", 15), 1000, &run);
  expectStatus(run, "succeeded", nullptr);
  Cmd stale = cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90}", 16);
  stale.issued = kIssued - 20000;
  stale.expires = kIssued - 10000;
  sendAt(*actuator, stale, 1500, &run);
  expectStatus(run, "rejected_expired", "COMMAND_EXPIRED");
  TEST_ASSERT_EQUAL(0, hal.countServo(19));
}

void test_core_clock_floor_resets_on_reconnect() {
  Run run;
  sendAt(*actuator, cmd(ACT, "SERVO-WINDOW-01", "set_position", "{\"position_degrees\":0}", 17), 1000, &run);
  actuator->onConnected();  // e.g. Pi4 rebooted with an earlier clock
  Cmd older = cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90}", 18);
  older.issued = kIssued - 3600000;
  older.expires = older.issued + 10000;
  sendAt(*actuator, older, 2000, &run);
  expectStatus(run, "succeeded", nullptr);
}

void test_retry_of_same_command_within_ttl_not_expired_by_floor() {
  Run run;
  Cmd first = cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":0}", 19);
  sendAt(*actuator, first, 1000, &run);
  // A different command issued at the same instant, received 3 s later (e.g. delayed).
  Cmd second = cmd(ACT, "SERVO-WINDOW-01", "set_position", "{\"position_degrees\":90}", 20);
  sendAt(*actuator, second, 4000, &run);
  expectStatus(run, "succeeded", nullptr);
}

// ---------------------------------------------------------------- invalid

void test_unknown_device_rejected() {
  Run run;
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-09", "set_position", "{\"position_degrees\":90}", 30), 100, &run);
  expectStatus(run, "rejected_invalid", "DEVICE_UNKNOWN");
  sendAt(*actuator, cmd(ACT, "LED-TV-01", "set_switch", "{\"on\":true}", 31), 100, &run);  // other board
  expectStatus(run, "rejected_invalid", "DEVICE_UNKNOWN");
  TEST_ASSERT_EQUAL(0, hal.count);
}

void test_not_wired_devices_rejected() {
  Run run;
  sendAt(*actuator, cmd(ACT, "SERVO-BED-CURTAIN-01", "set_position", "{\"position_degrees\":0}", 32), 100, &run);
  expectStatus(run, "rejected_invalid", "DEVICE_NOT_WIRED");
  // Bathroom door removed from the model (registry v4, 2026-10-25): ID kept, not wired.
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-02", "set_position", "{\"position_degrees\":90}", 38), 100, &run);
  expectStatus(run, "rejected_invalid", "DEVICE_NOT_WIRED");
  sendAt(*leds, cmd(LED, "LED-GUIDE-01", "set_switch", "{\"on\":true,\"auto_off_after_seconds\":15}", 33), 100, &run);
  expectStatus(run, "rejected_invalid", "DEVICE_NOT_WIRED");
  sendAt(*leds, cmd(LED, "FND-TV-01", "set_number", "{\"value\":43}", 34), 100, &run);
  expectStatus(run, "rejected_invalid", "DEVICE_NOT_WIRED");
  TEST_ASSERT_EQUAL(0, hal.count);
}

void test_unsupported_action_rejected() {
  Run run;
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_color", "{\"color\":\"red\"}", 35), 100, &run);
  expectStatus(run, "rejected_invalid", "CAPABILITY_NOT_ALLOWED");
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_position", "{\"position_degrees\":90}", 36), 100, &run);
  expectStatus(run, "rejected_invalid", "CAPABILITY_NOT_ALLOWED");
  sendAt(*leds, cmd(LED, "FAN-01", "explode", "{}", 37), 100, &run);
  expectStatus(run, "rejected_invalid", "CAPABILITY_NOT_ALLOWED");
  TEST_ASSERT_EQUAL(0, hal.count);
}

void test_fan_set_level_rejected() {
  Run run;
  sendAt(*leds, cmd(LED, "FAN-01", "set_level", "{\"level_percent\":30}", 38), 100, &run);
  expectStatus(run, "rejected_invalid", "CAPABILITY_NOT_ALLOWED");
  TEST_ASSERT_EQUAL(0, hal.count);
  TEST_ASSERT_FALSE(leds->switchOn(2));
}

void test_unregistered_and_out_of_range_angles_rejected() {
  Run run;
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":45}", 39), 100, &run);
  expectStatus(run, "rejected_invalid", "POSITION_NOT_REGISTERED");
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":180}", 40), 100, &run);
  expectStatus(run, "rejected_invalid", "POSITION_NOT_REGISTERED");
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":200}", 41), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":\"90\"}", 42), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90.0}", 43), 100, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_EQUAL(1, hal.countServo(19));
}

void test_unsupported_colours_rejected() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"green\"}", 44), 100, &run);
  expectStatus(run, "rejected_invalid", "COLOR_NOT_SUPPORTED");
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"amber\",\"pattern\":\"blink\"}", 45), 100, &run);
  expectStatus(run, "rejected_invalid", "COLOR_NOT_SUPPORTED");
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"purple\"}", 46), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"strobe\"}", 47), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");
  TEST_ASSERT_EQUAL(0, hal.count);
  TEST_ASSERT_FALSE(leds->led(1).overlay());  // LED-AIRCON-01
}

void test_invalid_switch_parameters_rejected() {
  Run run;
  sendAt(*leds, cmd(LED, "FAN-01", "set_switch", "{\"on\":1}", 48), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");
  sendAt(*leds, cmd(LED, "FAN-01", "set_switch", "{\"on\":true,\"auto_off_after_seconds\":15}", 49), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");  // auto-off is LED only
  sendAt(*leds, cmd(LED, "LED-LIVING-01", "set_switch", "{\"on\":true,\"auto_off_after_seconds\":-1}", 50), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");
  sendAt(*leds, cmd(LED, "LED-LIVING-01", "set_switch", "{\"on\":true,\"auto_off_after_seconds\":\"15\"}", 51), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");
  sendAt(*leds, cmd(LED, "LED-LIVING-01", "set_switch", "[]", 52), 100, &run);
  expectStatus(run, "rejected_invalid", "PARAMETERS_INVALID");
  TEST_ASSERT_EQUAL(0, hal.count);
}

void test_envelope_errors_rejected() {
  Run run;
  sendAt(*actuator, cmd(LED, "FAN-01", "set_switch", "{\"on\":true}", 53), 100, &run);
  expectStatus(run, "rejected_invalid", "NODE_MISMATCH");
  Cmd schema = cmd(ACT, "SERVO-WINDOW-01", "set_position", "{\"position_degrees\":0}", 54);
  schema.schema = "pfn.v0";
  sendAt(*actuator, schema, 100, &run);
  expectStatus(run, "rejected_invalid", "SCHEMA_VERSION_INVALID");
  Cmd times = cmd(ACT, "SERVO-WINDOW-01", "set_position", "{\"position_degrees\":0}", 55);
  times.expires = times.issued;
  sendAt(*actuator, times, 100, &run);
  expectStatus(run, "rejected_invalid", "COMMAND_INVALID");
  Cmd attempt = cmd(ACT, "SERVO-WINDOW-01", "set_position", "{\"position_degrees\":0}", 56);
  attempt.attempt = 4;
  sendAt(*actuator, attempt, 100, &run);
  expectStatus(run, "rejected_invalid", "COMMAND_INVALID");
  TEST_ASSERT_EQUAL(0, hal.count);
}

void test_unackable_messages_are_dropped() {
  char ack[512];
  size_t length = 99;
  const char* bad[] = {
      "not json",
      "[1,2]",
      "{\"command_id\":\"x\",\"trace_id\":\"22222222-2222-4222-8222-222222222222\",\"target_device_id\":\"FAN-01\"}",
      "{\"command_id\":\"44444444-4444-4444-8444-444444444444\",\"target_device_id\":\"FAN-01\"}",
      "{\"command_id\":\"44444444-4444-4444-8444-444444444444\",\"trace_id\":\"22222222-2222-4222-8222-222222222222\"}",
  };
  for (size_t index = 0; index < sizeof(bad) / sizeof(bad[0]); ++index) {
    const CommandResult result =
        actuator->handleCommand(bad[index], strlen(bad[index]), 0, at(0), ack, sizeof(ack), &length);
    TEST_ASSERT_FALSE(result.ackable);
    TEST_ASSERT_EQUAL(0, length);
  }
  TEST_ASSERT_EQUAL(0, hal.count);
}

void test_servo_hal_failure_reports_failed() {
  Run run;
  hal.fail_servo = true;
  sendAt(*actuator, cmd(ACT, "SERVO-DOOR-01", "set_position", "{\"position_degrees\":90}", 57), 100, &run);
  expectStatus(run, "failed", "ACTUATOR_FAILED");
  TEST_ASSERT_EQUAL(0, actuator->servoPosition(0));
}

// ---------------------------------------------------------------- fan

void test_fan_switch_on_off_and_telemetry() {
  Run run;
  sendAt(*leds, cmd(LED, "FAN-01", "set_switch", "{\"on\":true}", 60), 100, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_EQUAL(1, hal.events[hal.count - 1].value);
  TEST_ASSERT_EQUAL(100, run.ack["result"]["fan_level_percent"].as<int>());
  char buffer[1024];
  JsonDocument telemetry;
  TEST_ASSERT_TRUE(leds->buildTelemetry(2, at(200), buffer, sizeof(buffer)) > 0);
  deserializeJson(telemetry, buffer);
  TEST_ASSERT_EQUAL(100, telemetry["readings"]["fan_level_percent"].as<int>());
  TEST_ASSERT_EQUAL_STRING("FAN-01", telemetry["reading_sources"]["fan_level_percent"].as<const char*>());
  sendAt(*leds, cmd(LED, "FAN-01", "set_switch", "{\"on\":false}", 61), 300, &run);
  TEST_ASSERT_EQUAL(0, hal.events[hal.count - 1].value);
  leds->buildTelemetry(2, at(400), buffer, sizeof(buffer));
  deserializeJson(telemetry, buffer);
  TEST_ASSERT_EQUAL(0, telemetry["readings"]["fan_level_percent"].as<int>());
}

// ---------------------------------------------------------------- LED rules

const uint8_t AIRCON = 1;    // LED-AIRCON-01 index, D1 = GPIO5
const uint8_t LIVING = 0;    // LED-LIVING-01 index, D0 = GPIO16

void test_led_switch_on_is_steady() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_switch", "{\"on\":true}", 70), 100, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_TRUE(leds->led(AIRCON).power());
  TEST_ASSERT_EQUAL_STRING("steady", leds->led(AIRCON).pattern());
  TEST_ASSERT_TRUE(leds->outputLevel(AIRCON));
  for (uint64_t t = 100; t < 5000; t += 250) {
    leds->tick(at(t));
    TEST_ASSERT_TRUE(leds->outputLevel(AIRCON));
  }
}

void test_led_blink_overlay_while_power_off() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"blink\"}", 71), 1000, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_EQUAL_STRING("blink", run.ack["result"]["light_pattern"].as<const char*>());
  TEST_ASSERT_FALSE(run.ack["result"]["light_on"].as<bool>());
  leds->tick(at(1000));
  TEST_ASSERT_TRUE(leds->outputLevel(AIRCON));
  leds->tick(at(1999));
  TEST_ASSERT_TRUE(leds->outputLevel(AIRCON));
  leds->tick(at(2000));
  TEST_ASSERT_FALSE(leds->outputLevel(AIRCON));
  leds->tick(at(3000));
  TEST_ASSERT_TRUE(leds->outputLevel(AIRCON));
  TEST_ASSERT_EQUAL(3, hal.countDigital(5));  // on, off, on: writes only on edges
}

void test_led_blink_overlay_while_power_on_then_steady_restores_power() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_switch", "{\"on\":true}", 72), 0, &run);
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"blink\"}", 73), 10, &run);
  leds->tick(at(1010));
  TEST_ASSERT_FALSE(leds->outputLevel(AIRCON));
  TEST_ASSERT_EQUAL_STRING("blink", leds->led(AIRCON).pattern());
  // set_switch while blinking changes power only; overlay keeps blinking.
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_switch", "{\"on\":false}", 74), 1100, &run);
  TEST_ASSERT_TRUE(leds->led(AIRCON).overlay());
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_switch", "{\"on\":true}", 75), 1200, &run);
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"steady\"}", 76), 1300, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_FALSE(leds->led(AIRCON).overlay());
  TEST_ASSERT_EQUAL_STRING("steady", leds->led(AIRCON).pattern());
  leds->tick(at(2300));
  TEST_ASSERT_TRUE(leds->outputLevel(AIRCON));
}

void test_led_steady_with_power_off_goes_dark() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"blink\"}", 77), 0, &run);
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"steady\"}", 78), 500, &run);
  TEST_ASSERT_FALSE(leds->outputLevel(AIRCON));
  TEST_ASSERT_EQUAL_STRING("off", leds->led(AIRCON).pattern());
}

void test_led_colour_without_pattern_clears_overlay() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_switch", "{\"on\":true}", 79), 0, &run);
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"blink\"}", 80), 10, &run);
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\"}", 81), 1500, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_FALSE(leds->led(AIRCON).overlay());
  TEST_ASSERT_TRUE(leds->outputLevel(AIRCON));
}

void test_led_colour_off_clears_overlay_and_powers_off() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_switch", "{\"on\":true}", 82), 0, &run);
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"blink\"}", 83), 10, &run);
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"off\",\"pattern\":\"blink\"}", 84), 20, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_FALSE(leds->led(AIRCON).overlay());
  TEST_ASSERT_FALSE(leds->led(AIRCON).power());
  TEST_ASSERT_FALSE(leds->outputLevel(AIRCON));
  TEST_ASSERT_EQUAL_STRING("off", run.ack["result"]["light_pattern"].as<const char*>());
}

void test_quiet_mode_scenario_on_the_aircon_led() {
  // registry v4: the quiet mode scenario blinks the air conditioner indicator only (TV, washer not in the model)
  Run run;
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"blink\"}", 85), 0, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_TRUE(leds->led(AIRCON).overlay());
  TEST_ASSERT_FALSE(leds->led(LIVING).overlay());  // living light untouched
  sendAt(*leds, cmd(LED, "LED-TV-01", "set_color", "{\"color\":\"red\",\"pattern\":\"blink\"}", 86), 0, &run);
  expectStatus(run, "rejected_invalid", "DEVICE_NOT_WIRED");
  sendAt(*leds, cmd(LED, "LED-AIRCON-01", "set_color", "{\"color\":\"red\",\"pattern\":\"steady\"}", 88), 100, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_FALSE(leds->led(AIRCON).overlay());
}

void test_led_auto_off_after_15_seconds() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-LIVING-01", "set_switch", "{\"on\":true,\"auto_off_after_seconds\":15}", 91), 1000, &run);
  expectStatus(run, "succeeded", nullptr);
  TEST_ASSERT_EQUAL(15, run.ack["result"]["auto_off_after_seconds"].as<int>());
  TEST_ASSERT_TRUE(leds->takeTelemetryDirty(LIVING));
  leds->tick(at(15999));
  TEST_ASSERT_TRUE(leds->outputLevel(LIVING));
  TEST_ASSERT_FALSE(leds->takeTelemetryDirty(LIVING));
  leds->tick(at(16000));
  TEST_ASSERT_FALSE(leds->outputLevel(LIVING));
  TEST_ASSERT_FALSE(leds->led(LIVING).power());
  TEST_ASSERT_TRUE(leds->takeTelemetryDirty(LIVING));
  char buffer[1024];
  JsonDocument telemetry;
  leds->buildTelemetry(LIVING, at(16001), buffer, sizeof(buffer));
  deserializeJson(telemetry, buffer);
  TEST_ASSERT_FALSE(telemetry["readings"]["light_on"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("off", telemetry["readings"]["light_pattern"].as<const char*>());
}

void test_led_auto_off_cancelled_by_later_switch() {
  Run run;
  sendAt(*leds, cmd(LED, "LED-LIVING-01", "set_switch", "{\"on\":true,\"auto_off_after_seconds\":15}", 92), 0, &run);
  sendAt(*leds, cmd(LED, "LED-LIVING-01", "set_switch", "{\"on\":true}", 93), 5000, &run);
  leds->tick(at(30000));
  TEST_ASSERT_TRUE(leds->outputLevel(LIVING));
  TEST_ASSERT_FALSE(leds->led(LIVING).autoOffArmed());
}

// ---------------------------------------------------------------- messages

void test_heartbeat_and_last_will() {
  char buffer[512];
  JsonDocument heartbeat;
  TEST_ASSERT_TRUE(actuator->buildHeartbeat(at(5210), true, buffer, sizeof(buffer)) > 0);
  deserializeJson(heartbeat, buffer);
  TEST_ASSERT_EQUAL_STRING("pfn.v1", heartbeat["schema_version"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("esp32", heartbeat["node_type"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("online", heartbeat["status"].as<const char*>());
  TEST_ASSERT_EQUAL(0, heartbeat["seq"].as<int>());
  TEST_ASSERT_TRUE(heartbeat["uptime_ms"].as<long long>() == 5210);
  TEST_ASSERT_EQUAL_STRING(actuator->bootTraceId(), heartbeat["trace_id"].as<const char*>());
  actuator->buildHeartbeat(at(10210, true, kIssued), true, buffer, sizeof(buffer));
  deserializeJson(heartbeat, buffer);
  TEST_ASSERT_EQUAL(1, heartbeat["seq"].as<int>());
  TEST_ASSERT_TRUE(heartbeat["sent_at_ms"].as<long long>() == kIssued);
  actuator->buildHeartbeat(at(10300), false, buffer, sizeof(buffer));
  deserializeJson(heartbeat, buffer);
  TEST_ASSERT_EQUAL_STRING("offline", heartbeat["status"].as<const char*>());
  TEST_ASSERT_EQUAL(2, heartbeat["seq"].as<int>());
  TEST_ASSERT_EQUAL_STRING("pfn/v1/system/heartbeat", Node::heartbeatTopic());
  TEST_ASSERT_EQUAL_STRING("pfn/v1/device/CTRL-ESP32-ACT-01/command", actuator->commandTopic());
  TEST_ASSERT_EQUAL_STRING("pfn/v1/device/CTRL-ESP8266-LED-01/ack", leds->ackTopic());
}

void test_telemetry_one_device_per_message() {
  char buffer[1024];
  JsonDocument telemetry;
  actuator->buildTelemetry(0, at(100), buffer, sizeof(buffer));
  deserializeJson(telemetry, buffer);
  TEST_ASSERT_EQUAL(1, telemetry["readings"].size());
  TEST_ASSERT_EQUAL(0, telemetry["readings"]["door_position_degrees"].as<int>());
  TEST_ASSERT_EQUAL_STRING("SERVO-DOOR-01", telemetry["reading_sources"]["door_position_degrees"].as<const char*>());
  TEST_ASSERT_TRUE(telemetry["sensor_ready"].as<bool>());
  actuator->buildTelemetry(1, at(100), buffer, sizeof(buffer));
  deserializeJson(telemetry, buffer);
  TEST_ASSERT_EQUAL_STRING("SERVO-WINDOW-01", telemetry["reading_sources"]["window_position_degrees"].as<const char*>());
  TEST_ASSERT_EQUAL(1, telemetry["seq"].as<int>());
  TEST_ASSERT_EQUAL(0, actuator->buildTelemetry(9, at(100), buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL(0, actuator->buildTelemetry(0, at(100), buffer, 20));  // too small: nothing
}

void test_uuid_v4_format() {
  char id[37];
  for (int index = 0; index < 50; ++index) {
    makeUuidV4(testRandom, id);
    TEST_ASSERT_TRUE(isUuid(id));
    TEST_ASSERT_EQUAL('4', id[14]);
    TEST_ASSERT_TRUE(id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
  }
  TEST_ASSERT_FALSE(isUuid("44444444444444444444444444444444"));
  TEST_ASSERT_FALSE(isUuid("4444444-44444-4444-8444-444444444444"));
  TEST_ASSERT_FALSE(isUuid(nullptr));
}

void test_board_config_matches_harness() {
  TEST_ASSERT_EQUAL_STRING("CTRL-ESP32-ACT-01", kActuatorNode.node_id);
  TEST_ASSERT_EQUAL(2, kActuatorNode.device_count);
  TEST_ASSERT_EQUAL(19, kActuatorNode.devices[0].gpio);  // SERVO-DOOR-01
  TEST_ASSERT_EQUAL(21, kActuatorNode.devices[1].gpio);  // SERVO-WINDOW-01
  TEST_ASSERT_EQUAL(3, kLedNode.device_count);
  TEST_ASSERT_EQUAL(16, kLedNode.devices[0].gpio);  // LED-LIVING-01 D0
  TEST_ASSERT_EQUAL(5, kLedNode.devices[1].gpio);   // LED-AIRCON-01 D1
  TEST_ASSERT_EQUAL(4, kLedNode.devices[2].gpio);   // FAN-01 D2
  TEST_ASSERT_TRUE(findNodeSpec("CTRL-ESP8266-LED-01") == &kLedNode);
  TEST_ASSERT_TRUE(findNodeSpec("CTRL-OCTOPUS-01") == nullptr);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_holds_servo_signals_low_first);
  RUN_TEST(test_boot_turns_every_led_off_and_skips_boot_pins);
  RUN_TEST(test_servo_open_succeeds_with_ack_fields);
  RUN_TEST(test_duplicate_command_is_not_executed_twice);
  RUN_TEST(test_duplicate_after_expiry_still_returns_stored_success);
  RUN_TEST(test_duplicate_of_rejected_command_keeps_rejection);
  RUN_TEST(test_dedup_ring_keeps_last_16);
  RUN_TEST(test_expired_by_wall_clock);
  RUN_TEST(test_fresh_by_wall_clock);
  RUN_TEST(test_expired_by_hold_time_without_wall_clock);
  RUN_TEST(test_ttl_is_capped_at_protocol_command_ttl);
  RUN_TEST(test_stale_command_after_newer_one_is_expired_by_core_clock_floor);
  RUN_TEST(test_core_clock_floor_resets_on_reconnect);
  RUN_TEST(test_retry_of_same_command_within_ttl_not_expired_by_floor);
  RUN_TEST(test_unknown_device_rejected);
  RUN_TEST(test_not_wired_devices_rejected);
  RUN_TEST(test_unsupported_action_rejected);
  RUN_TEST(test_fan_set_level_rejected);
  RUN_TEST(test_unregistered_and_out_of_range_angles_rejected);
  RUN_TEST(test_unsupported_colours_rejected);
  RUN_TEST(test_invalid_switch_parameters_rejected);
  RUN_TEST(test_envelope_errors_rejected);
  RUN_TEST(test_unackable_messages_are_dropped);
  RUN_TEST(test_servo_hal_failure_reports_failed);
  RUN_TEST(test_fan_switch_on_off_and_telemetry);
  RUN_TEST(test_led_switch_on_is_steady);
  RUN_TEST(test_led_blink_overlay_while_power_off);
  RUN_TEST(test_led_blink_overlay_while_power_on_then_steady_restores_power);
  RUN_TEST(test_led_steady_with_power_off_goes_dark);
  RUN_TEST(test_led_colour_without_pattern_clears_overlay);
  RUN_TEST(test_led_colour_off_clears_overlay_and_powers_off);
  RUN_TEST(test_quiet_mode_scenario_on_the_aircon_led);
  RUN_TEST(test_led_auto_off_after_15_seconds);
  RUN_TEST(test_led_auto_off_cancelled_by_later_switch);
  RUN_TEST(test_heartbeat_and_last_will);
  RUN_TEST(test_telemetry_one_device_per_message);
  RUN_TEST(test_uuid_v4_format);
  RUN_TEST(test_board_config_matches_harness);
  return UNITY_END();
}
