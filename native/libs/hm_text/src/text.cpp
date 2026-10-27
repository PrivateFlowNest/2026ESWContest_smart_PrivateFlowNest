#include "hm/text.hpp"

#include <utf8proc.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hm::text {

namespace {

bool is_continuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

// One code point from WTF-8 at `i` (surrogates allowed). Returns bytes consumed, 0 if invalid.
std::size_t next_code_point(std::string_view s, std::size_t i, char32_t& out) {
  const auto b = static_cast<unsigned char>(s[i]);
  if (b < 0x80) {
    out = b;
    return 1;
  }
  std::size_t need = 0;
  char32_t cp = 0;
  char32_t minimum = 0;
  if ((b & 0xE0) == 0xC0) {
    need = 1, cp = b & 0x1F, minimum = 0x80;
  } else if ((b & 0xF0) == 0xE0) {
    need = 2, cp = b & 0x0F, minimum = 0x800;
  } else if ((b & 0xF8) == 0xF0) {
    need = 3, cp = b & 0x07, minimum = 0x10000;
  } else {
    return 0;
  }
  if (i + need >= s.size()) return 0;
  for (std::size_t k = 1; k <= need; ++k) {
    const auto c = static_cast<unsigned char>(s[i + k]);
    if (!is_continuation(c)) return 0;
    cp = (cp << 6) | (c & 0x3F);
  }
  if (cp < minimum || cp > 0x10FFFF) return 0;
  out = cp;
  return need + 1;
}

std::string hex(unsigned value, int width) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%0*x", width, value);
  return buffer;
}

}  // namespace

void append_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

std::u32string decode(std::string_view utf8) {
  std::u32string out;
  out.reserve(utf8.size());
  std::size_t i = 0;
  while (i < utf8.size()) {
    char32_t cp = 0;
    const std::size_t used = next_code_point(utf8, i, cp);
    if (used == 0) {
      out.push_back(0xFFFD);
      ++i;
    } else {
      out.push_back(cp);
      i += used;
    }
  }
  return out;
}

std::string encode(std::u32string_view code_points) {
  std::string out;
  out.reserve(code_points.size() * 3);
  for (char32_t c : code_points) append_utf8(out, c);
  return out;
}

std::optional<std::string> utf8_decode_error(std::string_view bytes) {
  // Same classification and positions as CPython's UTF-8 decoder (Objects/stringlib/codecs.h).
  const std::size_t n = bytes.size();
  auto at = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
  auto fail = [&](std::size_t start, std::size_t end, const char* reason) {
    std::string message = "'utf-8' codec can't decode ";
    if (end == start + 1) {
      message += "byte 0x" + hex(at(start), 2) + " in position " + std::to_string(start);
    } else {
      message += "bytes in position " + std::to_string(start) + "-" + std::to_string(end - 1);
    }
    return message + ": " + reason;
  };
  std::size_t i = 0;
  while (i < n) {
    const unsigned char ch = at(i);
    if (ch < 0x80) {
      ++i;
      continue;
    }
    if (ch < 0xC2 || ch > 0xF4) return fail(i, i + 1, "invalid start byte");
    const std::size_t left = n - i;
    if (ch < 0xE0) {
      if (left < 2) return fail(i, n, "unexpected end of data");
      if (!is_continuation(at(i + 1))) return fail(i, i + 1, "invalid continuation byte");
      i += 2;
      continue;
    }
    if (ch < 0xF0) {
      auto bad2 = [&](unsigned char c2) {
        return !is_continuation(c2) || (ch == 0xE0 && c2 < 0xA0) || (ch == 0xED && c2 >= 0xA0);
      };
      if (left < 3) {
        if (left < 2) return fail(i, n, "unexpected end of data");
        if (bad2(at(i + 1))) return fail(i, i + 1, "invalid continuation byte");
        return fail(i, n, "unexpected end of data");
      }
      if (bad2(at(i + 1))) return fail(i, i + 1, "invalid continuation byte");
      if (!is_continuation(at(i + 2))) return fail(i, i + 2, "invalid continuation byte");
      i += 3;
      continue;
    }
    auto bad2 = [&](unsigned char c2) {
      return !is_continuation(c2) || (ch == 0xF0 && c2 < 0x90) || (ch == 0xF4 && c2 >= 0x90);
    };
    if (left < 4) {
      if (left < 2) return fail(i, n, "unexpected end of data");
      if (bad2(at(i + 1))) return fail(i, i + 1, "invalid continuation byte");
      if (left < 3) return fail(i, n, "unexpected end of data");
      if (!is_continuation(at(i + 2))) return fail(i, i + 2, "invalid continuation byte");
      return fail(i, n, "unexpected end of data");
    }
    if (bad2(at(i + 1))) return fail(i, i + 1, "invalid continuation byte");
    if (!is_continuation(at(i + 2))) return fail(i, i + 2, "invalid continuation byte");
    if (!is_continuation(at(i + 3))) return fail(i, i + 3, "invalid continuation byte");
    i += 4;
  }
  return std::nullopt;
}

bool is_valid_utf8(std::string_view bytes) { return !utf8_decode_error(bytes).has_value(); }

std::string sanitize(std::string_view bytes) {
  if (is_valid_utf8(bytes)) return std::string(bytes);
  std::string out;
  out.reserve(bytes.size());
  std::size_t i = 0;
  while (i < bytes.size()) {
    char32_t cp = 0;
    const std::size_t used = next_code_point(bytes, i, cp);
    if (used == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) {
      append_utf8(out, 0xFFFD);
      i += used == 0 ? 1 : used;
    } else {
      out.append(bytes.substr(i, used));
      i += used;
    }
  }
  return out;
}

std::size_t length(std::string_view utf8) {
  std::size_t count = 0;
  for (char c : utf8) count += !is_continuation(static_cast<unsigned char>(c));
  return count;
}

std::string prefix(std::string_view utf8, std::size_t count) {
  std::size_t seen = 0;
  for (std::size_t i = 0; i < utf8.size(); ++i) {
    if (!is_continuation(static_cast<unsigned char>(utf8[i]))) {
      if (seen == count) return std::string(utf8.substr(0, i));
      ++seen;
    }
  }
  return std::string(utf8);
}

bool is_space(char32_t c) {
  // Objects/unicodetype_db.h _PyUnicode_IsWhitespace + the ASCII table (\x1c-\x1f are whitespace in Python).
  if (c <= 0x20) return c == 0x20 || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F);
  switch (c) {
    case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
      return true;
    default:
      return c >= 0x2000 && c <= 0x200A;
  }
}

bool is_printable(char32_t c) {
  if (c == 0x20) return true;
  if (c > 0x10FFFF) return false;
  switch (utf8proc_category(static_cast<utf8proc_int32_t>(c))) {
    case UTF8PROC_CATEGORY_CC: case UTF8PROC_CATEGORY_CF: case UTF8PROC_CATEGORY_CS: case UTF8PROC_CATEGORY_CO:
    case UTF8PROC_CATEGORY_CN: case UTF8PROC_CATEGORY_ZL: case UTF8PROC_CATEGORY_ZP: case UTF8PROC_CATEGORY_ZS:
      return false;
    default:
      return true;
  }
}

int decimal_value(char32_t c) {
  if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
  auto is_nd = [](char32_t x) {
    return x <= 0x10FFFF && utf8proc_category(static_cast<utf8proc_int32_t>(x)) == UTF8PROC_CATEGORY_ND;
  };
  if (!is_nd(c)) return -1;
  // Nd characters always come in contiguous, ascending runs of ten starting at 0 (Unicode stability policy).
  int offset = 0;
  while (c - offset > 0 && is_nd(c - offset - 1)) ++offset;
  return offset % 10;
}

std::string strip(std::string_view utf8) {
  const std::u32string cps = decode(utf8);
  std::size_t begin = 0;
  std::size_t end = cps.size();
  while (begin < end && is_space(cps[begin])) ++begin;
  while (end > begin && is_space(cps[end - 1])) --end;
  if (begin == 0 && end == cps.size() && is_valid_utf8(utf8)) return std::string(utf8);
  return encode(std::u32string_view(cps).substr(begin, end - begin));
}

std::string casefold(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (char32_t c : decode(utf8)) {
    if (c < 0x80) {
      out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));
      continue;
    }
    utf8proc_int32_t folded[8];
    int boundclass = 0;
    const auto count = utf8proc_decompose_char(static_cast<utf8proc_int32_t>(c), folded, 8, UTF8PROC_CASEFOLD,
                                               &boundclass);
    if (count <= 0 || count > 8) {
      append_utf8(out, c);
      continue;
    }
    for (utf8proc_ssize_t k = 0; k < count; ++k) append_utf8(out, static_cast<char32_t>(folded[k]));
  }
  return out;
}

std::string repr(std::string_view utf8) {
  const std::u32string cps = decode(utf8);
  bool single = false;
  bool dbl = false;
  for (char32_t c : cps) {
    single |= c == '\'';
    dbl |= c == '"';
  }
  const char32_t quote = single && !dbl ? '"' : '\'';
  std::string out(1, static_cast<char>(quote));
  for (char32_t c : cps) {
    if (c == quote || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c < ' ' || c == 0x7F) {
      out += "\\x" + hex(c, 2);
    } else if (c < 0x7F) {
      out.push_back(static_cast<char>(c));
    } else if (is_printable(c)) {
      append_utf8(out, c);
    } else if (c <= 0xFF) {
      out += "\\x" + hex(c, 2);
    } else if (c <= 0xFFFF) {
      out += "\\u" + hex(c, 4);
    } else {
      out += "\\U" + hex(c, 8);
    }
  }
  out.push_back(static_cast<char>(quote));
  return out;
}

std::string float_repr(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
  std::string_view sci(buffer, static_cast<std::size_t>(result.ptr - buffer));  // [-]d[.ddd]e[+-]XX
  std::string out;
  if (!sci.empty() && sci.front() == '-') {
    out.push_back('-');
    sci.remove_prefix(1);
  }
  const auto e = sci.find('e');
  std::string digits;
  for (char c : sci.substr(0, e)) {
    if (c != '.') digits.push_back(c);
  }
  int exponent = 0;
  const auto exp_text = sci.substr(e + 1);
  std::from_chars(exp_text.data() + (exp_text.front() == '+' ? 1 : 0), exp_text.data() + exp_text.size(), exponent);
  // Python's decpt: value = 0.DIGITS * 10**decpt; repr switches to exponent form when decpt <= -4 or > 16.
  const int decpt = exponent + 1;
  const int ndigits = static_cast<int>(digits.size());
  if (decpt <= -4 || decpt > 16) {
    out += digits.substr(0, 1);
    if (ndigits > 1) out += "." + digits.substr(1);
    const int shown = decpt - 1;
    char exp_buffer[16];
    std::snprintf(exp_buffer, sizeof exp_buffer, "e%c%02d", shown < 0 ? '-' : '+', std::abs(shown));
    return out + exp_buffer;
  }
  if (decpt <= 0) return out + "0." + std::string(static_cast<std::size_t>(-decpt), '0') + digits;
  if (decpt >= ndigits) return out + digits + std::string(static_cast<std::size_t>(decpt - ndigits), '0') + ".0";
  return out + digits.substr(0, static_cast<std::size_t>(decpt)) + "." + digits.substr(static_cast<std::size_t>(decpt));
}

}  // namespace hm::text
