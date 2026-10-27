// Phone approvals and live events (Mobile API, NODE-RPI4-01).
// ApprovalBook follows every task core announces (pfn/v1/core/task/update and the tasks of a state snapshot): tasks
// in state "proposed" wait for approval and are listed for the phone; the phone answers with pfn/v1/ui/action, the
// same document the 7-inch UI sends on pfn/v1/ui/action, on its own topic pfn/v1/mobile/task/action (only this
// service may write it, so core knows the phone sent it). Decision (task-policy.json policy_version 2): the phone may
// approve or reject everything waiting, except that opening the entrance door (SERVO-DOOR-01) is approved only on the
// 7-inch screen in the house; the phone may still reject it. Core refuses such an approval too; this service answers
// 403 before sending. EventHub keeps the last events for GET /api/v1/events (server-sent events).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hm/common.hpp"
#include "registry.hpp"

namespace hm::mobile {

struct PendingApproval {
  std::string task_id;
  std::string trace_id;
  std::string intent;
  std::string target_device;
  std::string origin;
  std::string action_token;
  std::int64_t revision = 0;
  json parameters = json::object();
  std::int64_t seen_ms = 0;
};

// The one approval the phone may not give: opening the entrance door.
bool phone_may_approve(const PendingApproval& approval);

// Short Korean title for the phone ("에어컨 표시등 켜기", "취침 준비").
std::string approval_title(const PendingApproval& approval, const Registry& registry);

// pfn/v1/mobile/task/action document for approve / reject (contracts/pfn.v1/ui-action.schema.json).
json ui_action_document(const PendingApproval& approval, const std::string& action, const std::string& client_id,
                        std::int64_t now_ms);

class ApprovalBook {
 public:
  // One task document from core. Returns true when the set of waiting approvals changed.
  bool observe(const json& task, std::int64_t now_ms);
  // The tasks of a state snapshot (core lists every active task first): also forgets approvals core no longer has
  // waiting, e.g. answered on the 7-inch UI while MQTT was down. Returns true when the waiting set changed.
  bool sync(const json& tasks, std::int64_t now_ms);
  std::vector<PendingApproval> pending() const;
  std::optional<PendingApproval> find(const std::string& task_id) const;
  // Last state core reported for any task seen recently (approved tasks keep reporting here).
  std::optional<std::string> state_of(const std::string& task_id) const;

 private:
  struct Seen {
    std::string state;
    std::int64_t revision = 0;
    std::int64_t seen_ms = 0;
  };
  mutable std::mutex mutex_;
  std::map<std::string, PendingApproval> pending_;
  std::map<std::string, Seen> states_;  // last 256 tasks
};

class EventHub {
 public:
  explicit EventHub(std::size_t keep = 200) : keep_(keep) {}
  void publish(json event);  // adds "seq" and "at_ms"
  // Events with seq > after, waiting up to `timeout` for the first one. Empty on timeout or after close().
  std::vector<json> wait_after(std::uint64_t after, std::chrono::milliseconds timeout);
  std::uint64_t latest() const;
  void close();
  bool closed() const;

 private:
  std::size_t keep_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<json> events_;
  std::uint64_t seq_ = 0;
  bool closed_ = false;
};

}  // namespace hm::mobile
