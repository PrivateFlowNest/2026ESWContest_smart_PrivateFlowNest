#include "store.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <set>

namespace hm::mobile {

namespace {

const std::map<std::string, std::string> kStateByCommand = {
    {"OPEN", "OPEN"}, {"CLOSE", "CLOSED"}, {"ON", "ON"}, {"OFF", "OFF"}};
const std::set<std::string> kFinalStates = {"completed", "failed", "rejected", "cancelled"};

std::string format_utc(std::int64_t seconds, const char* pattern) {
  const std::time_t time = static_cast<std::time_t>(seconds);
  std::tm parts{};
  gmtime_r(&time, &parts);
  char text[64];
  std::strftime(text, sizeof text, pattern, &parts);
  return text;
}

}  // namespace

std::string utc_isoformat_now() {
  using namespace std::chrono;
  const auto micros = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
  const std::int64_t seconds = micros / 1000000;
  const int fraction = static_cast<int>(micros % 1000000);
  std::string out = format_utc(seconds, "%Y-%m-%dT%H:%M:%S");
  if (fraction) {
    char text[16];
    std::snprintf(text, sizeof text, ".%06d", fraction);
    out += text;
  }
  return out + "+00:00";
}

std::string utc_iso_seconds(std::int64_t unix_seconds) { return format_utc(unix_seconds, "%Y-%m-%dT%H:%M:%SZ"); }

HardwareStore::HardwareStore(const Registry& registry) : devices_(registry.devices()) {
  registry.nodes(nodes_, controllers_);
}

json HardwareStore::devices_locked() const {
  json out = json::array();
  for (const auto& device : devices_) out.push_back(device.to_json());
  return out;
}

json HardwareStore::snapshot(bool connected) const {
  std::lock_guard lock(mutex_);
  return {{"connected", connected}, {"devices", devices_locked()}, {"updated_at", utc_isoformat_now()}};
}

json HardwareStore::hardware_map() const {
  std::lock_guard lock(mutex_);
  json nodes = json::array(), controllers = json::array();
  for (const auto& node : nodes_) nodes.push_back(node.to_json());
  for (const auto& controller : controllers_) controllers.push_back(controller.to_json());
  return {{"nodes", nodes}, {"controllers", controllers}, {"devices", devices_locked()}};
}

json HardwareStore::devices() const {
  std::lock_guard lock(mutex_);
  return devices_locked();
}

std::optional<DeviceView> HardwareStore::device(const std::string& device_id) const {
  std::lock_guard lock(mutex_);
  for (const auto& device : devices_) {
    if (device.id == device_id) return device;
  }
  return std::nullopt;
}

DeviceView HardwareStore::mark_pending(const std::string& device_id, const std::string& command,
                                       const std::string& task_id) {
  std::lock_guard lock(mutex_);
  for (auto& device : devices_) {
    if (device.id != device_id) continue;
    device.pending_command = command;
    device.pending_task_id = task_id;
    device.last_task_state = "requested";
    device.last_failure_code.reset();
    return device;
  }
  throw std::out_of_range(device_id);
}

void HardwareStore::task_changed(const std::string& device_id, const std::string& task_id, const std::string& state,
                                 const std::optional<std::string>& failure_code) {
  std::lock_guard lock(mutex_);
  for (auto& device : devices_) {
    if (device.id != device_id) continue;
    if (device.pending_task_id != task_id) return;
    device.last_task_state = state;
    device.last_failure_code = failure_code;
    if (kFinalStates.count(state)) {
      device.pending_command.reset();
      device.pending_task_id.reset();
    }
    return;
  }
}

void HardwareStore::confirm_command(const std::string& device_id, const std::string& command) {
  std::lock_guard lock(mutex_);
  for (auto& device : devices_) {
    if (device.id != device_id) continue;
    device.state = kStateByCommand.at(command);
    device.online = true;
    return;
  }
}

void HardwareStore::set_node_status(const std::map<std::string, std::string>& statuses) {
  std::lock_guard lock(mutex_);
  for (auto* group : {&nodes_, &controllers_}) {
    for (auto& item : *group) {
      auto it = statuses.find(item.id);
      if (it != statuses.end()) item.online = it->second == "online";
    }
  }
  for (auto& device : devices_) {
    if (!device.controller_id.is_string()) continue;
    auto it = statuses.find(device.controller_id.get<std::string>());
    if (it != statuses.end()) device.online = it->second == "online";
  }
}

}  // namespace hm::mobile
