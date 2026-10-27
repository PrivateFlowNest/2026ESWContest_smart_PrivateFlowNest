// Field checks shared by the MQTT relays to the 7-inch UI (pairing_relay, phones_relay).
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

#include "hm/common.hpp"

namespace hm::mobile::relay {

inline bool is_uuid_text(const json& value) {
  if (!value.is_string()) return false;
  const auto& text = value.get_ref<const std::string&>();
  if (text.size() != 36) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? text[i] != '-' : !std::isxdigit(static_cast<unsigned char>(text[i]))) return false;
  }
  return true;
}

inline bool is_string(const json& document, const char* key, std::size_t max_size) {
  return document.contains(key) && document[key].is_string() && !document[key].get_ref<const std::string&>().empty() &&
         document[key].get_ref<const std::string&>().size() <= max_size;
}

inline bool is_time(const json& document, const char* key) {
  return document.contains(key) && document[key].is_number_integer() && document[key].get<std::int64_t>() >= 0;
}

}  // namespace hm::mobile::relay
