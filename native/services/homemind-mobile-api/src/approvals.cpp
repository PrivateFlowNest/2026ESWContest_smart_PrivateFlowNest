#include "approvals.hpp"

#include <set>
#include <utility>

namespace hm::mobile {

namespace {

constexpr std::size_t kKeepStates = 256;
constexpr const char* kEntranceDoor = "SERVO-DOOR-01";

std::string text(const json& value, const char* key) {
  return value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : std::string();
}

std::string action_label(const json& parameters) {
  const std::string action = text(parameters, "action");
  const json values = parameters.value("parameters", json::object());
  if (action == "set_switch") return values.value("on", false) ? "켜기" : "끄기";
  if (action == "set_position") {
    if (values.contains("position_ref") && values["position_ref"].is_string()) {
      return values["position_ref"] == "open" ? "열기" : "닫기";
    }
    return values.value("position_degrees", 0) > 0 ? "열기" : "닫기";
  }
  if (action == "set_color") return "표시 바꾸기";
  if (action == "set_level") return "세기 바꾸기";
  return "동작";
}

}  // namespace

bool phone_may_approve(const PendingApproval& approval) { return approval.target_device != kEntranceDoor; }

std::string approval_title(const PendingApproval& approval, const Registry& registry) {
  if (approval.intent == "device_control") {
    const std::string device_id = text(approval.parameters, "target_device_id").empty()
                                      ? approval.target_device
                                      : text(approval.parameters, "target_device_id");
    std::string name = device_id;
    for (const auto& view : registry.devices()) {
      if (view.id == device_id) name = view.name;
    }
    return name + " " + action_label(approval.parameters);
  }
  static const std::map<std::string, std::string> kScenarios = {
      {"prepare_home", "귀가 준비"}, {"prepare_sleep", "취침 준비"}, {"night_bed_exit", "야간 이동 조명"},
      {"quiet_mode_on", "조용 모드 켜기"}, {"quiet_mode_off", "조용 모드 해제"}};
  const auto it = kScenarios.find(approval.intent);
  return it == kScenarios.end() ? approval.intent : it->second;
}

json ui_action_document(const PendingApproval& approval, const std::string& action, const std::string& client_id,
                        std::int64_t now_ms) {
  return {{"schema_version", "pfn.v1"},
          {"event_id", new_id()},
          {"trace_id", approval.trace_id.empty() ? new_id() : approval.trace_id},
          {"task_id", approval.task_id},
          {"client_id", client_id},
          {"action", action},
          {"expected_revision", approval.revision},
          {"action_token", approval.action_token},
          {"reason", action == "approve" ? "phone approval" : "phone rejection"},
          {"sent_at_ms", now_ms}};
}

bool ApprovalBook::observe(const json& task, std::int64_t now_ms) {
  if (!task.is_object()) return false;
  const std::string task_id = text(task, "task_id");
  const std::string state = text(task, "state");
  if (task_id.empty() || state.empty()) return false;
  const std::int64_t revision =
      task.contains("revision") && task["revision"].is_number_integer() ? task["revision"].get<std::int64_t>() : 0;
  std::lock_guard lock(mutex_);
  const auto seen = states_.find(task_id);
  if (seen != states_.end() && revision < seen->second.revision) return false;  // stale (QoS 1 redelivery, snapshot)
  states_[task_id] = {state, revision, now_ms};
  if (states_.size() > kKeepStates) {
    auto oldest = states_.begin();
    for (auto it = states_.begin(); it != states_.end(); ++it) {
      if (it->second.seen_ms < oldest->second.seen_ms) oldest = it;
    }
    states_.erase(oldest);
  }
  auto existing = pending_.find(task_id);
  if (state == "proposed" && !text(task, "action_token").empty()) {
    PendingApproval approval;
    approval.task_id = task_id;
    approval.trace_id = text(task, "trace_id");
    approval.intent = text(task, "intent");
    approval.target_device = text(task, "target_device");
    approval.parameters = task.value("parameters", json::object());
    approval.origin = text(approval.parameters, "origin");
    approval.action_token = text(task, "action_token");
    approval.revision = revision;
    approval.seen_ms = existing == pending_.end() ? now_ms : existing->second.seen_ms;
    const bool added = existing == pending_.end();
    pending_[task_id] = std::move(approval);
    return added;
  }
  if (existing == pending_.end()) return false;
  pending_.erase(existing);
  return true;
}

bool ApprovalBook::sync(const json& tasks, std::int64_t now_ms) {
  if (!tasks.is_array()) return false;
  bool changed = false;
  std::set<std::string> waiting;
  for (const auto& task : tasks) {
    changed = observe(task, now_ms) || changed;
    if (task.is_object() && text(task, "state") == "proposed") waiting.insert(text(task, "task_id"));
  }
  std::lock_guard lock(mutex_);
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (waiting.count(it->first)) {
      ++it;
    } else {
      it = pending_.erase(it);
      changed = true;
    }
  }
  return changed;
}

std::vector<PendingApproval> ApprovalBook::pending() const {
  std::lock_guard lock(mutex_);
  std::vector<PendingApproval> out;
  for (const auto& [id, approval] : pending_) out.push_back(approval);
  return out;
}

std::optional<PendingApproval> ApprovalBook::find(const std::string& task_id) const {
  std::lock_guard lock(mutex_);
  const auto it = pending_.find(task_id);
  if (it == pending_.end()) return std::nullopt;
  return it->second;
}

std::optional<std::string> ApprovalBook::state_of(const std::string& task_id) const {
  std::lock_guard lock(mutex_);
  const auto it = states_.find(task_id);
  if (it == states_.end()) return std::nullopt;
  return it->second.state;
}

void EventHub::publish(json event) {
  {
    std::lock_guard lock(mutex_);
    event["seq"] = ++seq_;
    event["at_ms"] = now_ms();
    events_.push_back(std::move(event));
    while (events_.size() > keep_) events_.pop_front();
  }
  changed_.notify_all();
}

std::vector<json> EventHub::wait_after(std::uint64_t after, std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, timeout, [&] { return closed_ || seq_ > after; });
  std::vector<json> out;
  if (closed_) return out;
  for (const auto& event : events_) {
    if (event["seq"].get<std::uint64_t>() > after) out.push_back(event);
  }
  return out;
}

std::uint64_t EventHub::latest() const {
  std::lock_guard lock(mutex_);
  return seq_;
}

void EventHub::close() {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  changed_.notify_all();
}

bool EventHub::closed() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

}  // namespace hm::mobile
