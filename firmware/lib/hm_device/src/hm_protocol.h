// HomeMind pfn.v1 constants used by the controller firmware.
// Source of truth: contracts/pfn.v1/protocol.json. tools/check_messages.py compares these
// values with protocol.json so a contract change cannot silently diverge from the firmware.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hm {

static const char kSchemaVersion[] = "pfn.v1";

static const uint32_t kHeartbeatIntervalMs = 5000;   // protocol.json heartbeat_interval_ms
static const uint32_t kOfflineAfterMs = 15000;       // protocol.json offline_after_ms (core side)
static const uint32_t kCommandTtlMs = 10000;         // protocol.json command_ttl_ms
static const uint32_t kAckTimeoutMs = 3000;          // protocol.json ack_timeout_ms (core side)
static const uint32_t kMaximumPayloadBytes = 262144; // protocol.json maximum_payload_bytes

// Firmware choices (not contract values).
static const uint32_t kTelemetryIntervalMs = 10000;  // periodic telemetry, plus one on every change
static const uint32_t kLedBlinkHalfPeriodMs = 1000;  // quiet overlay: 1 s on / 1 s off
static const uint32_t kMaxAutoOffSeconds = 86400;    // upper bound accepted for auto_off_after_seconds

// MQTT QoS / retained policy (protocol.json transport_policy).
static const int kQosCommandAck = 1;
static const int kQosTelemetryHeartbeat = 0;
static const bool kRetained = false;

// Buffer sizes on the controllers. Commands from core are < 1 KiB; larger payloads are
// dropped before parsing (still far below maximum_payload_bytes).
static const size_t kMaxCommandBytes = 1536;
static const size_t kMessageBufferBytes = 1024;

static const size_t kUuidLength = 36;
static const size_t kIdMaxLength = 64;

}  // namespace hm
