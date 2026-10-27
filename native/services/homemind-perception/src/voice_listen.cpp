#include "voice_listen.hpp"

#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string_view>

#if HM_PERCEPTION_HAVE_GUARD
#include "hm/pfn_schema.hpp"
#endif

namespace hm::perception {

namespace {

// Code points of a (valid, nlohmann-parsed) UTF-8 string: jsonschema minLength/maxLength count characters.
std::size_t code_points(const std::string& text) {
  std::size_t count = 0;
  for (unsigned char c : text) count += (c & 0xC0) != 0x80;
  return count;
}

#if !HM_PERCEPTION_HAVE_GUARD
bool canonical_uuid(const std::string& text) {
  if (text.size() != 36) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? text[i] != '-' : !std::isxdigit(static_cast<unsigned char>(text[i]))) return false;
  }
  return true;
}
#endif

bool uuid_format(const std::string& text) {
#if HM_PERCEPTION_HAVE_GUARD
  return hm::guard::jsonschema_uuid(text);  // uuid.UUID(value) + dashes at 8/13/18/23, like jsonschema
#else
  return canonical_uuid(text);  // stricter subset when libs/hm_guard is not built
#endif
}

// "integer" as jsonschema sees it: an int, or a float with an integral value (5.0).
bool json_integer(const json& value) {
  if (value.is_number_integer()) return true;
  if (!value.is_number_float()) return false;
  const double number = value.get<double>();
  return std::isfinite(number) && std::floor(number) == number;
}

std::int64_t as_int64(const json& value) {
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    return number > static_cast<std::uint64_t>(kMax) ? kMax : static_cast<std::int64_t>(number);
  }
  if (value.is_number_integer()) return value.get<std::int64_t>();
  const double number = value.get<double>();
  return number >= 9.2e18 ? kMax : static_cast<std::int64_t>(number);
}

}  // namespace

std::string validate_voice_listen(const json& document) {
  if (!document.is_object()) return "message: is not of type 'object'";
  static const std::initializer_list<std::string_view> required = {
      "schema_version", "event_id", "trace_id", "source_node_id", "target_node_id", "client_id", "action",
      "sent_at_ms", "expires_at_ms"};
  for (std::string_view name : required) {
    if (!document.contains(std::string(name))) return std::string(name) + ": is a required property";
  }
  for (const auto& [key, value] : document.items()) {
    bool known = key == "simulated";
    for (std::string_view name : required) known |= key == name;
    if (!known) return "message: additional properties are not allowed";
  }
  auto constant = [&](const char* key, const char* expected) {
    const json& value = document.at(key);
    return value.is_string() && value.get_ref<const std::string&>() == expected;
  };
  if (!constant("schema_version", "pfn.v1")) return "schema_version: must be 'pfn.v1'";
  for (const char* key : {"event_id", "trace_id"}) {
    const json& value = document.at(key);
    if (!value.is_string()) return std::string(key) + ": is not of type 'string'";
    if (!uuid_format(value.get_ref<const std::string&>())) return std::string(key) + ": is not a 'uuid'";
  }
  if (!constant("source_node_id", "NODE-RPI5-01")) return "source_node_id: must be 'NODE-RPI5-01'";
  if (!constant("target_node_id", "NODE-RPI5-02")) return "target_node_id: must be 'NODE-RPI5-02'";
  const json& client = document.at("client_id");
  if (!client.is_string()) return "client_id: is not of type 'string'";
  const std::size_t length = code_points(client.get_ref<const std::string&>());
  if (length < 1) return "client_id: is too short";
  if (length > 64) return "client_id: is too long";
  if (!constant("action", "start") && !constant("action", "cancel")) return "action: is not one of ['start', 'cancel']";
  for (const char* key : {"sent_at_ms", "expires_at_ms"}) {
    const json& value = document.at(key);
    if (!json_integer(value)) return std::string(key) + ": is not of type 'integer'";
    if (value.get<double>() < 0) return std::string(key) + ": is less than the minimum of 0";
  }
  if (document.contains("simulated") && !document.at("simulated").is_boolean()) {
    return "simulated: is not of type 'boolean'";
  }
  return "";
}

RemoteListen::RemoteListen(std::string node_id, std::size_t remembered_events)
    : node_id_(std::move(node_id)), remembered_(remembered_events == 0 ? 1 : remembered_events) {}

void RemoteListen::attach(hm::audio::PttControl* control) {
  std::lock_guard lock(mutex_);
  control_ = control;
}

RemoteListen::Counts RemoteListen::counts() const {
  std::lock_guard lock(mutex_);
  return counts_;
}

bool RemoteListen::remember(const std::string& event_id) {
  if (!seen_.insert(event_id).second) return false;
  seen_order_.push_back(event_id);
  while (seen_order_.size() > remembered_) {
    seen_.erase(seen_order_.front());
    seen_order_.pop_front();
  }
  return true;
}

std::string RemoteListen::handle(const json& document, std::int64_t now_ms) {
  static const char* kWhat = "ignored pfn/v1/ai/voice/listen: ";
  const std::string problem = validate_voice_listen(document);
  std::lock_guard lock(mutex_);
  if (!problem.empty()) {
    ++counts_.invalid;
    hm::log(hm::LogLevel::warning, "perception", std::string(kWhat) + "invalid (" + problem + ")");
    return "ignored:invalid";
  }
  if (document.at("target_node_id").get<std::string>() != node_id_) {
    ++counts_.wrong_target;
    hm::log(hm::LogLevel::warning, "perception", std::string(kWhat) + "addressed to another node");
    return "ignored:wrong_target";
  }
  const std::int64_t expires = as_int64(document.at("expires_at_ms"));
  if (now_ms > expires) {
    ++counts_.expired;
    hm::log(hm::LogLevel::warning, "perception",
            std::string(kWhat) + "expired " + std::to_string(now_ms - expires) +
                " ms ago (stale press, or the RPI5 clocks differ)");
    return "ignored:expired";
  }
  if (!remember(document.at("event_id").get<std::string>())) {
    ++counts_.duplicate;
    hm::log(hm::LogLevel::info, "perception", std::string(kWhat) + "duplicate event (redelivery)");
    return "ignored:duplicate";
  }
  if (!control_) {
    ++counts_.inactive;
    hm::log(hm::LogLevel::warning, "perception", std::string(kWhat) + "push-to-talk is not active on this node");
    return "ignored:inactive";
  }
  const bool start = document.at("action").get<std::string>() == "start";
  control_->push(start ? hm::audio::PttCommand::start : hm::audio::PttCommand::cancel,
                 document.at("trace_id").get<std::string>());  // the press trace becomes the utterance trace
  ++(start ? counts_.start : counts_.cancel);
  hm::log(hm::LogLevel::info, "perception",
          std::string("remote push-to-talk ") + (start ? "start" : "cancel") + " (trace " +
              document.at("trace_id").get<std::string>() + ")");
  return start ? "start" : "cancel";
}

}  // namespace hm::perception
