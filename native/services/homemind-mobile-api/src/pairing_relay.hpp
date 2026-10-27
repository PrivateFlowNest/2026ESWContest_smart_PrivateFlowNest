// Phone pairing QR on the 7-inch UI (contracts/pfn.v1 mobile-pairing-request / mobile-pairing-offer, 2026-10-22).
// NODE-RPI5-01 publishes pfn/v1/mobile/pairing/request; this answers on pfn/v1/mobile/pairing/offer with a fresh
// one-time offer from the same PairingService as --print-pairing-qr (code + nonce, valid code_ttl_s). The broker ACL
// lets only NODE-RPI5-01 read offers. The code never goes to a log: logs carry the request id prefix and expiry only.
// Ignored (nothing published): wrong shape, other target node, expired or replayed requests. Answered "unavailable"
// (no secret): no public_base_url, another request within min_interval_ms, offer creation failed.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>

#include "auth.hpp"
#include "hm/common.hpp"

namespace hm::mobile {

inline constexpr const char* kPairingRequest = "pfn/v1/mobile/pairing/request";
inline constexpr const char* kPairingOffer = "pfn/v1/mobile/pairing/offer";
inline constexpr const char* kPairingUiNode = "NODE-RPI5-01";

class PairingRelay {
 public:
  using Publish = std::function<bool(const std::string& topic, const json& document)>;

  PairingRelay(const PairingService& pairing, const AuthConfig& auth, std::string node_id, bool simulated,
               Publish publish, std::int64_t min_interval_ms = 2000);

  // One request from the broker. Returns the offer it published (tests), or nothing when it was ignored.
  std::optional<json> on_request(const json& request, std::int64_t now_ms);

  // Why a request is ignored, or "" when it is acceptable (shape, target, freshness, replay).
  std::string check(const json& request, std::int64_t now_ms) const;

 private:
  json answer(const json& request, std::int64_t now_ms);

  const PairingService& pairing_;
  const AuthConfig& auth_;
  std::string node_id_;
  bool simulated_;
  Publish publish_;
  std::int64_t min_interval_ms_;
  std::optional<std::int64_t> last_offer_ms_;
  std::deque<std::string> seen_;  // recent request ids (replays after a reconnect are ignored)
};

}  // namespace hm::mobile
