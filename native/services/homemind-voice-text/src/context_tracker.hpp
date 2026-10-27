// Latest ACTIVITY_CONTEXT perception as the sanitized activity-context the guard/EXAONE expect.
// Port of services/runtime cli.py ContextTracker.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "hm/common.hpp"

namespace hm {

class ContextTracker {
 public:
  // Bus handler for pfn/v1/ai/perception. Throws (and the bus logs) on documents Python would reject.
  void update(const std::string& topic, const json& document);
  // null when nothing is known or the last context expired (valid_until_ms < now).
  json current() const;

 private:
  mutable std::mutex mutex_;
  json context_ = nullptr;
  std::int64_t valid_until_ms_ = 0;
};

}  // namespace hm
