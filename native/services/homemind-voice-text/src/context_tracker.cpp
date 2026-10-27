#include "context_tracker.hpp"

#include <cmath>
#include <cstdlib>
#include <map>
#include <stdexcept>

#include "hm/pyjson.hpp"
#include "hm/text.hpp"

namespace hm {

namespace {

std::string ai_zone(const json& zone) {
  static const std::map<std::string, std::string> kRegistryToAiZone = {
      {"bed", "BED"}, {"desk", "DESK"}, {"bathroom", "BATHROOM"}, {"entrance", "ENTRANCE"}, {"living", "COMMON"},
      {"outdoor", "OUTDOOR"}};
  if (!zone.is_string()) return "UNKNOWN";
  auto it = kRegistryToAiZone.find(zone.get<std::string>());
  return it == kRegistryToAiZone.end() ? "UNKNOWN" : it->second;
}

// float(value): numbers, bools and numeric strings, like Python.
double python_float(const json& value) {
  if (value.is_number()) return value.get<double>();
  if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
  if (value.is_string()) {
    const std::string text_value = text::strip(value.get<std::string>());
    char* end = nullptr;
    const double parsed = std::strtod(text_value.c_str(), &end);
    if (!text_value.empty() && end == text_value.c_str() + text_value.size()) return parsed;
  }
  throw std::invalid_argument("could not convert value to float");
}

// int(value): truncation for floats, bools, decimal strings, like Python.
std::int64_t python_int(const json& value) {
  if (value.is_number_integer()) return value.get<std::int64_t>();
  if (value.is_number_float()) {
    const double number = value.get<double>();
    if (!std::isfinite(number)) throw std::invalid_argument("cannot convert float to integer");
    return static_cast<std::int64_t>(std::trunc(number));
  }
  if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
  if (value.is_string()) {
    const std::string text_value = text::strip(value.get<std::string>());
    char* end = nullptr;
    const long long parsed = std::strtoll(text_value.c_str(), &end, 10);
    if (!text_value.empty() && end == text_value.c_str() + text_value.size()) return parsed;
  }
  throw std::invalid_argument("invalid literal for int()");
}

}  // namespace

void ContextTracker::update(const std::string&, const json& document) {
  if (!document.is_object() || document.value("observation_type", json(nullptr)) != "ACTIVITY_CONTEXT") return;
  const json& attributes = document.at("attributes");
  json context = {
      {"presence_zone", ai_zone(document.at("zone"))},
      {"activity", attributes.at("activity")},
      {"audio_context", attributes.at("audio_context")},
      {"quiet_required", py::truthy(attributes.at("quiet_required"))},
      {"confidence", python_float(document.at("confidence"))},
  };
  std::lock_guard lock(mutex_);
  context_ = std::move(context);
  // Python assigns the context first; a bad valid_until_ms raises after that.
  valid_until_ms_ = attributes.contains("valid_until_ms") ? python_int(attributes["valid_until_ms"]) : 0;
}

json ContextTracker::current() const {
  std::lock_guard lock(mutex_);
  if (context_.is_null() || valid_until_ms_ < now_ms()) return nullptr;
  return context_;
}

}  // namespace hm
