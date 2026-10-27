#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

namespace homemind {

inline std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline std::int64_t uptime_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline std::string uuid_v4() {
  thread_local std::mt19937_64 generator(std::random_device{}());
  std::uniform_int_distribution<unsigned int> distribution(0, 255);
  std::array<std::uint8_t, 16> bytes{};
  for (auto& value : bytes) {
    value = static_cast<std::uint8_t>(distribution(generator));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      output << '-';
    }
    output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return output.str();
}

inline bool is_uuid(const std::string& value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
    const char character = value[index];
    if (hyphen) {
      if (character != '-') {
        return false;
      }
    } else if (!((character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f') ||
                 (character >= 'A' && character <= 'F'))) {
      return false;
    }
  }
  return true;
}

inline void require_mosquitto(int result, const std::string& operation);

}  // namespace homemind

