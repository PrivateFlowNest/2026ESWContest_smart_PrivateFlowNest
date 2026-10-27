#include "hm_uuid.h"

#include <string.h>

namespace hm {

void makeUuidV4(Random32 random32, char out[37]) {
  uint8_t bytes[16];
  for (int word = 0; word < 4; ++word) {
    const uint32_t value = random32();
    bytes[word * 4 + 0] = static_cast<uint8_t>(value >> 24);
    bytes[word * 4 + 1] = static_cast<uint8_t>(value >> 16);
    bytes[word * 4 + 2] = static_cast<uint8_t>(value >> 8);
    bytes[word * 4 + 3] = static_cast<uint8_t>(value);
  }
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant
  static const char hex[] = "0123456789abcdef";
  size_t position = 0;
  for (int index = 0; index < 16; ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out[position++] = '-';
    out[position++] = hex[bytes[index] >> 4];
    out[position++] = hex[bytes[index] & 0x0F];
  }
  out[position] = '\0';
}

bool isUuid(const char* text) {
  if (text == nullptr || strlen(text) != 36) return false;
  for (int index = 0; index < 36; ++index) {
    const char c = text[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

}  // namespace hm
