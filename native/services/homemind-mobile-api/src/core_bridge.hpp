// Mobile API <-> NODE-RPI4-01 core over pfn.v1 MQTT (system-architecture 3-5). Port of core_bridge.py.
// Phone -> POST command -> pfn/v1/core/task/request (intent device_control, parameters.origin "ui") -> core checks
// registry + task-policy -> device command -> ESP ACK -> pfn/v1/core/task/update. Device state changes only when
// core reports the task "completed". No device topic is ever used. MQTT account homemind-mobile-api
// (core package mosquitto.acl); the password only from a file path (hm::BrokerConfig).
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "hm/common.hpp"
#include "registry.hpp"

namespace hm::mobile {

inline constexpr const char* kTaskRequest = "pfn/v1/core/task/request";
inline constexpr const char* kTaskUpdate = "pfn/v1/core/task/update";
inline constexpr const char* kStateRequest = "pfn/v1/core/state/request";
inline constexpr const char* kStateResponsePrefix = "pfn/v1/core/state/response/";
inline constexpr const char* kMobileTaskAction = "pfn/v1/mobile/task/action";  // phone approve / reject

// task-policy.json origins.ui.allowed_source_nodes (same list as the Python reference).
bool allowed_source_node(const std::string& node_id);

// The MQTT side as the bridge sees it (hm::Bus in the service, a fake in the unit tests).
class Publisher {
 public:
  virtual ~Publisher() = default;
  virtual bool connected() const = 0;
  virtual bool publish(const std::string& topic, const json& document) = 0;
};

struct BridgeConfig {
  bool enabled = false;
  std::string client_id = "homemind-mobile-api";
  std::string source_node_id = "NODE-RPI4-01";
};

struct TrackedTask {
  std::string task_id;
  std::string device_id;
  std::string command;
  std::string state = "requested";
  std::int64_t revision = 0;
  json failure = nullptr;
  std::int64_t requested_at_ms = 0;
  std::int64_t updated_at_ms = 0;

  std::optional<std::string> failure_field(const char* key) const;
};

// pfn.v1 task request for one app command (same document as core_bridge.device_control_request).
json device_control_request(const ControlRequest& control, const std::string& source_node_id);

class CoreBridge {
 public:
  using TaskCallback = std::function<void(const TrackedTask&)>;
  using NodesCallback = std::function<void(const std::map<std::string, std::string>&)>;

  CoreBridge(BridgeConfig config, Publisher* publisher, TaskCallback on_task, NodesCallback on_nodes);

  bool enabled() const { return config_.enabled; }
  bool connected() const { return config_.enabled && publisher_ && publisher_->connected(); }
  const BridgeConfig& config() const { return config_; }
  std::string state_response_topic() const { return kStateResponsePrefix + config_.client_id; }

  // nullopt when MQTT is disabled/disconnected or the publish was refused (API answers 503).
  std::optional<TrackedTask> request(const ControlRequest& control, const std::string& command);
  std::optional<TrackedTask> task(const std::string& task_id) const;
  // pfn/v1/mobile/task/action (phone approve / reject). False when MQTT is down or the publish was refused.
  bool send_task_action(const json& document);
  void request_snapshot();
  void handle_task_update(const json& document);
  void handle_state_response(const json& document);
  void on_message(const std::string& topic, const json& document);

 private:
  void prune_locked();

  BridgeConfig config_;
  Publisher* publisher_;
  TaskCallback on_task_;
  NodesCallback on_nodes_;
  mutable std::mutex mutex_;
  std::map<std::string, TrackedTask> tasks_;
};

}  // namespace hm::mobile
