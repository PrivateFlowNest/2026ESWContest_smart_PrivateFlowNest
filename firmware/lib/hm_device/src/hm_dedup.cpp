#include "hm_dedup.h"

#include <string.h>

namespace hm {

const char* ackStatusText(AckStatus status) {
  switch (status) {
    case AckStatus::Succeeded: return "succeeded";
    case AckStatus::Failed: return "failed";
    case AckStatus::RejectedExpired: return "rejected_expired";
    case AckStatus::RejectedInvalid: return "rejected_invalid";
  }
  return "failed";
}

void DedupRing::clear() {
  memset(entries_, 0, sizeof(entries_));
  next_ = 0;
}

const AckRecord* DedupRing::find(const char* command_id) const {
  if (command_id == nullptr) return nullptr;
  for (size_t index = 0; index < kCapacity; ++index) {
    if (entries_[index].used && strcmp(entries_[index].command_id, command_id) == 0) {
      return &entries_[index];
    }
  }
  return nullptr;
}

const AckRecord& DedupRing::remember(const AckRecord& record) {
  AckRecord& slot = entries_[next_];
  slot = record;
  slot.used = true;
  next_ = (next_ + 1) % kCapacity;
  return slot;
}

}  // namespace hm
