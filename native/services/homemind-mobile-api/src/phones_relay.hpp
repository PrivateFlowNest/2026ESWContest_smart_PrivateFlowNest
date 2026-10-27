// Paired phones on the 7-inch UI (contracts/pfn.v1 mobile-phones-request / mobile-phones-state, 2026-10-25).
// NODE-RPI5-01 publishes pfn/v1/mobile/phones/request (action list or revoke); this answers on
// pfn/v1/mobile/phones/state with every phone that holds a valid token: the token id (never the token), the name the
// phone sent when it paired, the pairing time and its last accepted request since start. A revoke removes the token
// from tokens.json at once: the phone's next request gets 401 and it shows the pairing QR gate again, and its open
// event stream ends within one keep-alive (15 s). notify_paired() sends the list right after a pairing so an open
// screen shows the new phone. The broker ACL lets only NODE-RPI5-01 read the list.
// Ignored (nothing published): wrong shape, other target node, expired or replayed requests.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>

#include "auth.hpp"
#include "hm/common.hpp"

namespace hm::mobile {

inline constexpr const char* kPhonesRequest = "pfn/v1/mobile/phones/request";
inline constexpr const char* kPhonesState = "pfn/v1/mobile/phones/state";

class PhonesRelay {
 public:
  using Publish = std::function<bool(const std::string& topic, const json& document)>;

  PhonesRelay(TokenStore& tokens, std::string node_id, bool simulated, Publish publish);

  // One request from the broker. Returns the state it published (tests), or nothing when it was ignored.
  std::optional<json> on_request(const json& request, std::int64_t now_ms);
  // Why a request is ignored, or "" when it is acceptable (shape, target, freshness, replay).
  std::string check(const json& request, std::int64_t now_ms) const;
  // After a phone paired: the list without a request (request_id null, action "paired"). Returns what it published.
  json notify_paired(std::int64_t now_ms);

 private:
  json state(const json& request_id, const std::string& action, std::int64_t now_ms);

  TokenStore& tokens_;
  std::string node_id_;
  bool simulated_;
  Publish publish_;
  std::deque<std::string> seen_;  // recent request ids (replays after a reconnect are ignored)
};

}  // namespace hm::mobile
