#include "hm/heartbeat.hpp"

#include <utility>

namespace hm {

json heartbeat_document(const std::string& node_id, const std::string& node_type, const std::string& status,
                        std::int64_t seq, std::int64_t sent_at_ms, std::int64_t uptime_ms, const std::string& trace_id) {
  return {{"schema_version", "pfn.v1"}, {"event_id", new_id()},     {"trace_id", trace_id},
          {"node_id", node_id},         {"node_type", node_type},   {"status", status},
          {"seq", seq},                 {"sent_at_ms", sent_at_ms}, {"uptime_ms", uptime_ms}};
}

HeartbeatLoop::HeartbeatLoop(std::function<void(const json&)> send, std::function<bool()> connected,
                             std::string node_id, std::string node_type, int interval_ms)
    : send_(std::move(send)),
      connected_(std::move(connected)),
      node_id_(std::move(node_id)),
      node_type_(std::move(node_type)),
      interval_ms_(interval_ms),
      trace_id_(new_id()),
      started_(std::chrono::steady_clock::now()),
      thread_([this] { run(); }) {}

HeartbeatLoop::~HeartbeatLoop() { stop(); }

void HeartbeatLoop::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (connected_()) beat("offline");
}

void HeartbeatLoop::run() {
  std::unique_lock lock(mutex_);
  while (!stopping_) {
    if (connected_()) {
      lock.unlock();
      beat("online");
      lock.lock();
    }
    wake_.wait_for(lock, std::chrono::milliseconds(interval_ms_), [this] { return stopping_; });
  }
}

void HeartbeatLoop::beat(const char* status) {
  const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_);
  try {
    send_(heartbeat_document(node_id_, node_type_, status, seq_++, now_ms(), uptime.count(), trace_id_));
  } catch (const std::exception& error) {  // a failed publish must not stop the service
    log(LogLevel::warning, "heartbeat", std::string("heartbeat not sent: ") + error.what());
  }
}

}  // namespace hm
