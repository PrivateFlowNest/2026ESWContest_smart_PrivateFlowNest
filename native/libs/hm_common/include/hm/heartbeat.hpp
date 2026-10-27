// Node heartbeat (pfn/v1/system/heartbeat, heartbeat.schema.json) for the RPI5 services. Core marks a node and the
// devices it controls offline after protocol offline_after_ms (15 s) without one, so before 2026-10-25 the 7-inch UI
// showed MIC-INDOOR-01 and both cameras (NODE-RPI5-02) offline although they worked.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "hm/common.hpp"

namespace hm {

inline constexpr const char* kHeartbeatTopic = "pfn/v1/system/heartbeat";
inline constexpr int kHeartbeatIntervalMs = 5000;  // protocol.json heartbeat_interval_ms

json heartbeat_document(const std::string& node_id, const std::string& node_type, const std::string& status,
                        std::int64_t seq, std::int64_t sent_at_ms, std::int64_t uptime_ms, const std::string& trace_id);

// Sends "online" every interval while connected() and one "offline" from stop() (or the destructor) when connected.
// send() and connected() wrap the service's hm::Bus: [&](const json& d) { bus.publish(kHeartbeatTopic, d, 0); }.
class HeartbeatLoop {
 public:
  HeartbeatLoop(std::function<void(const json&)> send, std::function<bool()> connected, std::string node_id,
                std::string node_type, int interval_ms = kHeartbeatIntervalMs);
  ~HeartbeatLoop();
  HeartbeatLoop(const HeartbeatLoop&) = delete;
  HeartbeatLoop& operator=(const HeartbeatLoop&) = delete;
  void stop();

 private:
  void run();
  void beat(const char* status);

  std::function<void(const json&)> send_;
  std::function<bool()> connected_;
  std::string node_id_;
  std::string node_type_;
  int interval_ms_;
  std::string trace_id_;  // one per process, like the ESP boot trace
  std::chrono::steady_clock::time_point started_;
  std::int64_t seq_ = 0;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace hm
