#include "hm/partial/text.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <utf8proc.h>

namespace hm::partial {

namespace {

inline bool is_surrogate(char32_t c) { return c >= 0xD800 && c <= 0xDFFF; }

void append_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// Python's Case_Ignorable / Cased properties, approximated from utf8proc categories (only used for the
// Final_Sigma context of U+03A3 in str.lower()).
bool case_ignorable(char32_t c) {
  switch (utf8proc_category(static_cast<utf8proc_int32_t>(c))) {
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_ME:
    case UTF8PROC_CATEGORY_CF:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_SK:
      return true;
    default:
      break;
  }
  static constexpr char32_t kWordBreakMid[] = {0x27,   0x2E,   0x3A,   0xB7,   0x387,  0x55F,
                                               0x5F4,  0x2018, 0x2019, 0x2024, 0x2027, 0xFE13,
                                               0xFE52, 0xFE55, 0xFF07, 0xFF0E, 0xFF1A};
  for (char32_t m : kWordBreakMid) {
    if (c == m) return true;
  }
  return false;
}

bool cased(char32_t c) {
  const auto cp = static_cast<utf8proc_int32_t>(c);
  const auto category = utf8proc_category(cp);
  if (category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LT) {
    return true;
  }
  if (utf8proc_tolower(cp) != cp || utf8proc_toupper(cp) != cp) return true;
  return c == 0xAA || c == 0xBA || (c >= 0x2B0 && c <= 0x2B8) || (c >= 0x2C0 && c <= 0x2C1) ||
         (c >= 0x2E0 && c <= 0x2E4) || c == 0x345 || c == 0x37A || (c >= 0x1D2C && c <= 0x1D6A) || c == 0x1D78 ||
         (c >= 0x1D9B && c <= 0x1DBF) || c == 0x2071 || c == 0x207F || (c >= 0x2090 && c <= 0x209C) ||
         (c >= 0x2C7C && c <= 0x2C7D) || (c >= 0xA69C && c <= 0xA69D) || c == 0xA770 ||
         (c >= 0xA7F8 && c <= 0xA7F9) || (c >= 0xAB5C && c <= 0xAB5F) || (c >= 0x1F130 && c <= 0x1F189);
}

// unicodeobject.c handle_capital_sigma
bool final_sigma(const CodePoints& text, std::size_t i) {
  std::size_t j = i;
  char32_t c = 0;
  bool found = false;
  while (j > 0) {
    c = text[--j];
    if (!case_ignorable(c)) {
      found = true;
      break;
    }
  }
  bool result = found && cased(c);
  if (result && i + 1 < text.size()) {
    std::size_t k = i + 1;
    for (; k < text.size(); ++k) {
      c = text[k];
      if (!case_ignorable(c)) break;
    }
    result = k == text.size() || !cased(c);
  }
  return result;
}

}  // namespace

CodePoints decode_utf8(std::string_view bytes) {
  CodePoints out;
  out.reserve(bytes.size());
  const auto* s = reinterpret_cast<const unsigned char*>(bytes.data());
  const std::size_t n = bytes.size();
  std::size_t i = 0;
  auto continuation = [&](std::size_t at, unsigned lo = 0x80, unsigned hi = 0xBF) {
    return at < n && s[at] >= lo && s[at] <= hi;
  };
  while (i < n) {
    const unsigned b = s[i];
    if (b < 0x80) {
      out += static_cast<char32_t>(b);
      ++i;
      continue;
    }
    std::size_t length = 0;
    char32_t value = 0;
    if (b >= 0xC2 && b <= 0xDF && continuation(i + 1)) {
      length = 2;
      value = ((b & 0x1F) << 6) | (s[i + 1] & 0x3F);
    } else if (b >= 0xE0 && b <= 0xEF) {
      const unsigned lo = b == 0xE0 ? 0xA0 : 0x80, hi = b == 0xED ? 0x9F : 0xBF;
      if (continuation(i + 1, lo, hi) && continuation(i + 2)) {
        length = 3;
        value = ((b & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
      }
    } else if (b >= 0xF0 && b <= 0xF4) {
      const unsigned lo = b == 0xF0 ? 0x90 : 0x80, hi = b == 0xF4 ? 0x8F : 0xBF;
      if (continuation(i + 1, lo, hi) && continuation(i + 2) && continuation(i + 3)) {
        length = 4;
        value = ((b & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
      }
    }
    if (length == 0) {
      out += static_cast<char32_t>(0xDC00 + b);  // surrogateescape, one byte at a time
      ++i;
    } else {
      out += value;
      i += length;
    }
  }
  return out;
}

std::string encode_utf8_replace(const CodePoints& text) {
  std::string out;
  out.reserve(text.size() * 3);
  for (char32_t c : text) {
    if (is_surrogate(c) || c > 0x10FFFF) {
      out += '?';
    } else {
      append_utf8(out, c);
    }
  }
  return out;
}

bool has_surrogate(const CodePoints& text) {
  for (char32_t c : text) {
    if (is_surrogate(c)) return true;
  }
  return false;
}

bool is_python_space(char32_t c) {
  return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

CodePoints nfkc(const CodePoints& text) {
  // Lone surrogates never combine, so NFKC is applied to each valid run between them.
  CodePoints out;
  std::size_t start = 0;
  auto flush = [&](std::size_t end) {
    if (end <= start) return;
    const std::string run = encode_utf8_replace(text.substr(start, end - start));
    utf8proc_uint8_t* mapped = nullptr;
    const auto length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(run.data()),
                                     static_cast<utf8proc_ssize_t>(run.size()), &mapped,
                                     static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT));
    if (length < 0) throw std::runtime_error(std::string("NFKC failed: ") + utf8proc_errmsg(length));
    out += decode_utf8(std::string_view(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(length)));
    std::free(mapped);
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (is_surrogate(text[i])) {
      flush(i);
      out += text[i];
      start = i + 1;
    }
  }
  flush(text.size());
  return out;
}

CodePoints python_lower(const CodePoints& text) {
  CodePoints out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char32_t c = text[i];
    if (c == 0x130) {
      out += U'i';
      out += static_cast<char32_t>(0x307);
    } else if (c == 0x3A3) {
      out += static_cast<char32_t>(final_sigma(text, i) ? 0x3C2 : 0x3C3);
    } else if (is_surrogate(c)) {
      out += c;
    } else {
      out += static_cast<char32_t>(utf8proc_tolower(static_cast<utf8proc_int32_t>(c)));
    }
  }
  return out;
}

CodePoints normalize_text(const CodePoints& text) {
  const CodePoints composed = nfkc(text);
  std::size_t begin = 0, end = composed.size();
  while (begin < end && is_python_space(composed[begin])) ++begin;
  while (end > begin && is_python_space(composed[end - 1])) --end;
  const CodePoints lowered = python_lower(composed.substr(begin, end - begin));
  CodePoints out;
  out.reserve(lowered.size());
  bool in_space = false;
  for (char32_t c : lowered) {
    if (is_python_space(c)) {
      if (!in_space) out += U' ';
      in_space = true;
    } else {
      out += c;
      in_space = false;
    }
  }
  if (out.size() > 512) out.resize(512);
  return out;
}

CodePoints normalize_text(std::string_view utf8) { return normalize_text(decode_utf8(utf8)); }

std::size_t count(const CodePoints& haystack, const CodePoints& needle) {
  if (needle.empty()) return haystack.size() + 1;  // Python semantics for ""
  std::size_t total = 0;
  for (std::size_t at = haystack.find(needle); at != CodePoints::npos; at = haystack.find(needle, at + needle.size())) {
    ++total;
  }
  return total;
}

bool contains(const CodePoints& haystack, const CodePoints& needle) { return haystack.find(needle) != CodePoints::npos; }

std::uint32_t fnv1a_32(std::string_view bytes) {
  std::uint32_t result = 2166136261u;
  for (unsigned char byte : bytes) {
    result ^= byte;
    result *= 16777619u;
  }
  return result;
}

// ------------------------------------------------------------------------------------------- SHA-256

namespace {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

std::string sha256_hex(std::string_view bytes) {
  std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string message(bytes);
  const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
  message += static_cast<char>(0x80);
  while (message.size() % 64 != 56) message += '\0';
  for (int i = 7; i >= 0; --i) message += static_cast<char>((bit_length >> (i * 8)) & 0xFF);
  for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(message.data() + chunk + 4 * i);
      w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto v = h;
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const std::uint32_t t1 = v[7] + s1 + ch + kK[i] + w[i];
      const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const std::uint32_t t2 = s0 + maj;
      v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (int i = 0; i < 8; ++i) h[i] += v[i];
  }
  char out[65];
  for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
  return std::string(out, 64);
}

}  // namespace hm::partial
