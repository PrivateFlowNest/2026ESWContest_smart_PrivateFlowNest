#include "pairing_relay.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "relay_checks.hpp"
#include "store.hpp"

namespace hm::mobile {

namespace {

constexpr std::int64_t kMaxRequestLifetimeMs = 60000;
constexpr std::size_t kSeenRequests = 64;

}  // namespace

PairingRelay::PairingRelay(const PairingService& pairing, const AuthConfig& auth, std::string node_id, bool simulated,
                           Publish publish, std::int64_t min_interval_ms)
    : pairing_(pairing),
      auth_(auth),
      node_id_(std::move(node_id)),
      simulated_(simulated),
      publish_(std::move(publish)),
      min_interval_ms_(min_interval_ms) {}

std::string PairingRelay::check(const json& request, std::int64_t now_ms) const {
  using namespace relay;
  if (!request.is_object()) return "not an object";
  static const char* const kAllowed[] = {"schema_version", "event_id", "request_id", "source_node_id",
                                         "target_node_id", "client_id", "sent_at_ms", "expires_at_ms", "simulated"};
  for (const auto& [key, value] : request.items()) {
    if (std::find(std::begin(kAllowed), std::end(kAllowed), key) == std::end(kAllowed)) return "unknown field " + key;
  }
  if (request.value("schema_version", "") != "pfn.v1") return "schema_version";
  if (!is_uuid_text(request.value("event_id", json())) || !is_uuid_text(request.value("request_id", json()))) {
    return "event_id/request_id";
  }
  if (request.value("source_node_id", "") != kPairingUiNode) return "source_node_id";
  if (request.value("target_node_id", "") != node_id_) return "target_node_id";
  if (!is_string(request, "client_id", 64)) return "client_id";
  if (!is_time(request, "sent_at_ms") || !is_time(request, "expires_at_ms")) return "sent_at_ms/expires_at_ms";
  if (request.contains("simulated") && !request["simulated"].is_boolean()) return "simulated";
  const auto sent = request["sent_at_ms"].get<std::int64_t>();
  const auto expires = request["expires_at_ms"].get<std::int64_t>();
  if (expires <= sent || expires - sent > kMaxRequestLifetimeMs) return "lifetime";
  if (expires < now_ms) return "expired";
  const auto& id = request["request_id"].get_ref<const std::string&>();
  if (std::find(seen_.begin(), seen_.end(), id) != seen_.end()) return "replayed";
  return "";
}

json PairingRelay::answer(const json& request, std::int64_t now_ms) {
  json offer = {{"schema_version", "pfn.v1"},
                {"event_id", hm::new_id()},
                {"request_id", request["request_id"]},
                {"source_node_id", node_id_},
                {"target_node_id", kPairingUiNode},
                {"created_at_ms", now_ms}};
  if (simulated_) offer["simulated"] = true;
  auto unavailable = [&offer](const std::string& reason) {
    offer["status"] = "unavailable";
    offer["reason"] = reason;
    return offer;
  };
  if (auth_.public_base_url.empty()) return unavailable("public_base_url is not set on the Mobile API");
  if (last_offer_ms_ && now_ms - *last_offer_ms_ < min_interval_ms_) return unavailable("too many requests, try again");
  try {
    const PairingOffer made = pairing_.make_offer(now_ms / 1000, auth_.public_base_url, auth_.code_ttl_s);
    last_offer_ms_ = now_ms;
    offer["status"] = "offered";
    offer["expires_at_ms"] = made.expires_at_s * 1000;
    offer["qr_payload"] = hm::dump_compact(made.qr);
    offer["pairing_code"] = made.code;
    offer["device_name"] = made.qr["device_name"];
    offer["base_url"] = made.qr["base_url"];
    return offer;
  } catch (const std::exception& error) {
    hm::log(LogLevel::warning, "pairing", std::string("offer for the UI failed: ") + error.what());
    return unavailable("pairing offer could not be created");
  }
}

std::optional<json> PairingRelay::on_request(const json& request, std::int64_t now_ms) {
  const std::string problem = check(request, now_ms);
  if (!problem.empty()) {
    hm::log(LogLevel::info, "pairing", "UI pairing request ignored (" + problem + ")");
    return std::nullopt;
  }
  const std::string id = request["request_id"].get<std::string>();
  seen_.push_back(id);
  if (seen_.size() > kSeenRequests) seen_.pop_front();
  json offer = answer(request, now_ms);
  const bool offered = offer["status"] == "offered";
  if (!publish_(kPairingOffer, offer)) {
    hm::log(LogLevel::warning, "pairing", "could not publish the pairing offer (request " + id.substr(0, 8) + ")");
  } else if (offered) {
    hm::log(LogLevel::info, "pairing",
            "pairing QR offered to the 7-inch UI (request " + id.substr(0, 8) + ", valid until " +
                utc_iso_seconds(offer["expires_at_ms"].get<std::int64_t>() / 1000) + " UTC)");
  } else {
    hm::log(LogLevel::info, "pairing",
            "UI pairing request " + id.substr(0, 8) + " answered unavailable: " + offer["reason"].get<std::string>());
  }
  return offer;
}

}  // namespace hm::mobile
