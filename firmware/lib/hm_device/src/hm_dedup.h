// Fixed-size ring of the most recent command outcomes, keyed by command_id.
// A command whose command_id is still in the ring is never executed again: the stored outcome is
// re-sent with duplicate:true. Every outcome is recorded (succeeded, failed, rejected_*), and it is
// recorded BEFORE the ACK is published. Capacity 16 covers core's 3 attempts within the 10 s TTL
// with a wide margin. Not persisted across reboot: all actions are absolute (servo angle, on/off,
// colour), so re-applying one after a reboot does not compound.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hm_protocol.h"

namespace hm {

enum class AckStatus : uint8_t { Succeeded, Failed, RejectedExpired, RejectedInvalid };

const char* ackStatusText(AckStatus status);

struct AckRecord {
  bool used;
  char command_id[kUuidLength + 1];
  char device_id[kIdMaxLength + 1];
  AckStatus status;
  char result_json[96];     // serialized "result" object, empty = null
  const char* error_code;   // static string or nullptr
  const char* error_message;
};

class DedupRing {
 public:
  static const size_t kCapacity = 16;

  DedupRing() : next_(0) { clear(); }
  void clear();
  const AckRecord* find(const char* command_id) const;
  // Stores a copy (overwrites the oldest entry when full) and returns the stored record.
  const AckRecord& remember(const AckRecord& record);

 private:
  AckRecord entries_[kCapacity];
  size_t next_;
};

}  // namespace hm
