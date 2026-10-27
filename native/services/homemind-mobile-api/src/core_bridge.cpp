#include "core_bridge.hpp"

#include <set>
#include <vector>

#include "hm/pfn.hpp"

namespace hm::mobile {

namespace {

const std::set<std::string> kFinalStates = {"completed", "failed", "rejected", "cancelled"};
constexpr std::int64_t kKeepFinishedMs = 10 * 60 * 1000;

// str(value) for the JSON values core sends (strings stay as they are).
std::string as_text(const json& value) { return value.is_string() ? value.get<std::string>() : value.dump(); }

}  // namespace

bool allowed_source_node(const std::string& node_id) {
  return node_id == "NODE-RPI4-01" || node_id == "NODE-RPI5-01";
}

std::optional<std::string> TrackedTask::failure_field(const char* key) const {
  if (!failure.is_object()) return std::nullopt;
  auto it = failure.find(key);
  if (it == failure.end() || it->is_null()) return std::nullopt;
  return as_text(*it);
}

json device_control_request(const ControlRequest& control, const std::string& source_node_id) {
  pfn::TaskRequestArgs args;
  args.source_node_id = source_node_id;
  args.intent = "device_control";
  args.target_device = control.device_id;
  args.confidence = 1.0;
  // Opening the entrance door waits for approval on the 7-inch UI (task-policy rule).
  args.safety_level =
      control.position_ref == std::optional<std::string>("open") && control.device_id == "SERVO-DOOR-01" ? "caution"
                                                                                                          : "normal";
  args.parameters = {
      {"target_device_id", control.device_id},
      {"action", control.action},
      {"parameters", control.parameters},
      {"origin", "ui"},
      {"ai_device", nullptr},
      {"ai_action", nullptr},
  };
  return pfn::task_request(args);
}

CoreBridge::CoreBridge(BridgeConfig config, Publisher* publisher, TaskCallback on_task, NodesCallback on_nodes)
    : config_(std::move(config)), publisher_(publisher), on_task_(std::move(on_task)), on_nodes_(std::move(on_nodes)) {}

std::optional<TrackedTask> CoreBridge::request(const ControlRequest& control, const std::string& command) {
  if (!connected()) return std::nullopt;
  const json document = device_control_request(control, config_.source_node_id);
  TrackedTask task;
  task.task_id = document.at("task_id").get<std::string>();
  task.device_id = control.device_id;
  task.command = command;
  task.requested_at_ms = task.updated_at_ms = now_ms();
  {
    std::lock_guard lock(mutex_);
    tasks_[task.task_id] = task;
    prune_locked();
  }
  if (!publisher_->publish(kTaskRequest, document)) {
    std::lock_guard lock(mutex_);
    tasks_.erase(task.task_id);
    return std::nullopt;
  }
  return task;
}

std::optional<TrackedTask> CoreBridge::task(const std::string& task_id) const {
  std::lock_guard lock(mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return std::nullopt;
  return it->second;
}

bool CoreBridge::send_task_action(const json& document) {
  return connected() && publisher_->publish(kMobileTaskAction, document);
}

void CoreBridge::request_snapshot() {
  if (!connected()) return;
  publisher_->publish(kStateRequest, {{"schema_version", "pfn.v1"},
                                      {"event_id", new_id()},
                                      {"trace_id", new_id()},
                                      {"request_id", new_id()},
                                      {"client_id", config_.client_id},
                                      {"sections", {"devices", "tasks"}},
                                      {"sent_at_ms", now_ms()}});
}

void CoreBridge::prune_locked() {
  const std::int64_t cutoff = now_ms() - kKeepFinishedMs;
  for (auto it = tasks_.begin(); it != tasks_.end();) {
    if (it->second.updated_at_ms < cutoff && kFinalStates.count(it->second.state)) {
      it = tasks_.erase(it);
    } else {
      ++it;
    }
  }
}

void CoreBridge::handle_task_update(const json& document) {
  if (!document.is_object()) return;
  const json task = document.contains("task") && document["task"].is_object() ? document["task"] : json::object();
  TrackedTask copy;
  {
    std::lock_guard lock(mutex_);
    auto it = tasks_.find(task.contains("task_id") ? as_text(task["task_id"]) : "None");
    if (it == tasks_.end()) return;
    TrackedTask& tracked = it->second;
    const std::int64_t revision = task.contains("revision") ? task["revision"].get<std::int64_t>() : 0;
    if (revision < tracked.revision) return;
    if (task.contains("state")) tracked.state = as_text(task["state"]);
    if (task.contains("revision")) tracked.revision = revision;
    tracked.failure = task.value("failure", json(nullptr));
    tracked.updated_at_ms = now_ms();
    copy = tracked;
  }
  if (on_task_) on_task_(copy);
}

void CoreBridge::handle_state_response(const json& document) {
  std::map<std::string, std::string> statuses;
  if (document.is_object() && document.contains("devices") && document["devices"].is_array()) {
    for (const auto& item : document["devices"]) {
      if (!item.is_object()) continue;
      statuses[as_text(item.value("node_id", json(nullptr)))] = as_text(item.value("status", json(nullptr)));
    }
  }
  if (on_nodes_) on_nodes_(statuses);
}

void CoreBridge::on_message(const std::string& topic, const json& document) {
  if (topic == kTaskUpdate) {
    handle_task_update(document);
  } else if (topic.rfind(kStateResponsePrefix, 0) == 0) {
    handle_state_response(document);
  }
}

}  // namespace hm::mobile
