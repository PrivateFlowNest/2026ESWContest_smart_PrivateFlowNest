// App view of the registry devices (port of hardware_store.py). Device state changes only through
// confirm_command, i.e. after core reported the task completed (ESP ACK). Thread-safe.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "registry.hpp"

namespace hm::mobile {

class HardwareStore {
 public:
  explicit HardwareStore(const Registry& registry);

  json snapshot(bool connected) const;  // {"connected", "devices", "updated_at"}
  json hardware_map() const;            // {"nodes", "controllers", "devices"}
  json devices() const;                 // [device, ...] in registry order
  std::optional<DeviceView> device(const std::string& device_id) const;
  DeviceView mark_pending(const std::string& device_id, const std::string& command, const std::string& task_id);
  void task_changed(const std::string& device_id, const std::string& task_id, const std::string& state,
                    const std::optional<std::string>& failure_code);
  void confirm_command(const std::string& device_id, const std::string& command);
  void set_node_status(const std::map<std::string, std::string>& statuses);

 private:
  json devices_locked() const;

  mutable std::mutex mutex_;
  std::vector<NodeView> nodes_;
  std::vector<NodeView> controllers_;
  std::vector<DeviceView> devices_;
};

// datetime.now(timezone.utc).isoformat(): "2026-10-22T03:04:05.123456+00:00" (microseconds omitted when 0).
std::string utc_isoformat_now();
// "2026-10-22T03:04:05Z" for a Unix time in seconds.
std::string utc_iso_seconds(std::int64_t unix_seconds);

}  // namespace hm::mobile
