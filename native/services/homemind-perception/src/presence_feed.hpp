// Live camera presence for context-fusion: homemind-vision writes rpi5-internal PRESENCE observations
// (contracts/rpi5-internal/context-feature-observation.schema.json, one compact JSON object per line) to a local
// FIFO (vision.presence_fifo); homemind-perception reads the same path (presence_fifo) and hands every event to the
// context engine (hm::AudioContextPipeline::submit_presence). Nothing leaves the node; the FIFO lives under the
// runtime directory (device: /run/homemind/presence).
//
// The FIFO is created when missing (mode 0660; an existing non-FIFO path is refused) and opened read/write so
// writers coming and going never produce EOF. Lines that are not JSON objects, longer than kMaxLine, or rejected by
// the sink (invalid event) are counted and logged without their content.
#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "hm/common.hpp"

namespace hm::perception {

class PresenceFeed {
 public:
  static constexpr std::size_t kMaxLine = 8192;
  using Sink = std::function<void(const json& event)>;  // throws for an event it rejects

  struct Counts {
    std::size_t accepted = 0, not_json = 0, rejected = 0, oversized = 0;
  };

  // Line handling only (tests); start() adds the FIFO reader thread.
  explicit PresenceFeed(Sink sink);
  ~PresenceFeed();
  PresenceFeed(const PresenceFeed&) = delete;
  PresenceFeed& operator=(const PresenceFeed&) = delete;

  void start(const std::filesystem::path& fifo);  // throws std::runtime_error when the FIFO cannot be used
  void stop();
  void feed(const char* data, std::size_t size);  // bytes as read from the FIFO
  Counts counts() const;

 private:
  void line(const std::string& text);
  void warn(const std::string& what);

  Sink sink_;
  std::string partial_;
  bool skipping_ = false;  // inside an over-long line
  mutable std::mutex mutex_;
  Counts counts_;
  std::size_t warnings_ = 0;
  int fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace hm::perception
