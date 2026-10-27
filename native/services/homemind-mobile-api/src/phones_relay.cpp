#include "phones_relay.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

#include "relay_checks.hpp"

namespace hm::mobile {

namespace {

constexpr const char* kPhonesUiNode = "NODE-RPI5-01";
constexpr std::int64_t kMaxRequestLifetimeMs = 60000;
constexpr std::size_t kSeenRequests = 64;
constexpr std::size_t kMaxPhones = 50;  // mobile-phones-state.schema.json phones maxItems

bool is_phone_id(const json& value) {
  if (!value.is_string()) return false;
  const auto& text = value.get_ref<const std::string&>();
  return text.size() == 8 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

}  // namespace

PhonesRelay::PhonesRelay(TokenStore& tokens, std::string node_id, bool simulated, Publish publish)
    : tokens_(tokens), node_id_(std::move(node_id)), simulated_(simulated), publish_(std::move(publish)) {}

std::string PhonesRelay::check(const json& request, std::int64_t now_ms) const {
  using namespace relay;
  if (!request.is_object()) return "not an object";
  static const char* const kAllowed[] = {"schema_version", "event_id",   "request_id", "source_node_id",
                                         "target_node_id", "client_id",  "action",     "phone_id",
                                         "sent_at_ms",     "expires_at_ms", "simulated"};
  for (const auto& [key, value] : request.items()) {
    if (std::find(std::begin(kAllowed), std::end(kAllowed), key) == std::end(kAllowed)) return "unknown field " + key;
  }
  if (request.value("schema_version", "") != "pfn.v1") return "schema_version";
  if (!is_uuid_text(request.value("event_id", json())) || !is_uuid_text(request.value("request_id", json()))) {
    return "event_id/request_id";
  }
  if (request.value("source_node_id", "") != kPhonesUiNode) return "source_node_id";
  if (request.value("target_node_id", "") != node_id_) return "target_node_id";
  if (!is_string(request, "client_id", 64)) return "client_id";
  const std::string action = request.value("action", "");
  if (action != "list" && action != "revoke") return "action";
  if (action == "revoke" ? !is_phone_id(request.value("phone_id", json())) : request.contains("phone_id")) {
    return "phone_id";
  }
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

json PhonesRelay::state(const json& request_id, const std::string& action, std::int64_t now_ms) {
  json document = {{"schema_version", "pfn.v1"},
                   {"event_id", hm::new_id()},
                   {"request_id", request_id},
                   {"source_node_id", node_id_},
                   {"target_node_id", kPhonesUiNode},
                   {"created_at_ms", now_ms},
                   {"action", action}};
  if (simulated_) document["simulated"] = true;
  try {
    const std::map<std::string, std::int64_t> seen = tokens_.last_seen();
    json phones = json::array();
    for (const auto& record : tokens_.list()) {
      if (phones.size() >= kMaxPhones) break;
      if (!is_phone_id(record.id)) continue;  // tokens.json written by hand: not something the UI can unpair
      const auto last = seen.find(record.id);
      phones.push_back({{"phone_id", record.id},
                        {"device_name", record.device_name.empty() ? "unnamed" : record.device_name},
                        {"paired_at", record.created_at.empty() ? "unknown" : record.created_at},
                        {"last_seen_at_ms", last == seen.end() ? json(nullptr) : json(last->second)}});
    }
    document["status"] = "ok";
    document["phones"] = std::move(phones);
  } catch (const std::exception& error) {
    hm::log(LogLevel::warning, "phones", std::string("paired phone list unavailable: ") + error.what());
    document["status"] = "unavailable";
    document["reason"] = "the paired phone list cannot be read on the Mobile API";
    document["phones"] = json::array();
  }
  return document;
}

std::optional<json> PhonesRelay::on_request(const json& request, std::int64_t now_ms) {
  const std::string problem = check(request, now_ms);
  if (!problem.empty()) {
    hm::log(LogLevel::info, "phones", "UI phone list request ignored (" + problem + ")");
    return std::nullopt;
  }
  const std::string id = request["request_id"].get<std::string>();
  seen_.push_back(id);
  if (seen_.size() > kSeenRequests) seen_.pop_front();
  const std::string action = request["action"].get<std::string>();
  bool revoked = false;
  std::string failure;
  if (action == "revoke") {
    try {
      revoked = tokens_.revoke(request["phone_id"].get<std::string>());
    } catch (const std::exception& error) {
      failure = error.what();
    }
  }
  json document = state(request["request_id"], action, now_ms);
  if (!failure.empty()) {
    hm::log(LogLevel::warning, "phones", "unpair from the 7-inch UI failed: " + failure);
    document["status"] = "unavailable";
    document["reason"] = "the phone could not be unpaired on the Mobile API";
  } else if (action == "revoke" && document["status"] == "ok") {
    document["revoked"] = revoked;
  }
  if (!publish_(kPhonesState, document)) {
    hm::log(LogLevel::warning, "phones", "could not publish the phone list (request " + id.substr(0, 8) + ")");
  } else if (action == "revoke") {
    hm::log(LogLevel::info, "phones",
            "phone " + request["phone_id"].get<std::string>() +
                (revoked ? " unpaired from the 7-inch UI" : " not unpaired (no such phone)"));
  }
  return document;
}

json PhonesRelay::notify_paired(std::int64_t now_ms) {
  json document = state(nullptr, "paired", now_ms);
  if (!publish_(kPhonesState, document)) {
    hm::log(LogLevel::info, "phones", "phone list after pairing not sent (no broker connection)");
  }
  return document;
}

}  // namespace hm::mobile
