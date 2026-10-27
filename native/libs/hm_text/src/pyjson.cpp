#include "hm/pyjson.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "hm/text.hpp"

namespace hm::py {

namespace {

using value_t = nlohmann::json::value_t;

// CPython raises RecursionError somewhere around this depth (C recursion limit minus the caller's frames).
constexpr int kMaxDepth = 1000;

std::string error_text(const std::string& msg, const std::u32string& doc, std::size_t pos) {
  std::size_t line = 1;
  std::size_t last_newline = std::string::npos;
  for (std::size_t i = 0; i < pos && i < doc.size(); ++i) {
    if (doc[i] == U'\n') {
      ++line;
      last_newline = i;
    }
  }
  const std::size_t column = last_newline == std::string::npos ? pos + 1 : pos - last_newline;
  return msg + ": line " + std::to_string(line) + " column " + std::to_string(column) + " (char " +
         std::to_string(pos) + ")";
}

bool is_ws(char32_t c) { return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r'; }
bool is_digit(char32_t c) { return c >= U'0' && c <= U'9'; }

int hex_value(char32_t c) {
  if (c >= U'0' && c <= U'9') return static_cast<int>(c - U'0');
  if (c >= U'a' && c <= U'f') return static_cast<int>(c - U'a' + 10);
  if (c >= U'A' && c <= U'F') return static_cast<int>(c - U'A' + 10);
  return -1;
}

// Port of Modules/_json.c (scan_once_unicode and friends), CPython 3.12.
class Scanner {
 public:
  explicit Scanner(const std::u32string& doc) : s_(doc), n_(doc.size()) {}

  ojson scan_once(std::size_t idx, std::size_t& next) {
    if (idx >= n_) fail("Expecting value", idx);
    switch (s_[idx]) {
      case U'"': {
        std::string value = scanstring(idx + 1, next);
        return ojson(std::move(value));
      }
      case U'{':
        return nested(idx, next, true);
      case U'[':
        return nested(idx, next, false);
      case U'n':
        if (idx + 3 < n_ && s_[idx + 1] == U'u' && s_[idx + 2] == U'l' && s_[idx + 3] == U'l') {
          next = idx + 4;
          return nullptr;
        }
        break;
      case U't':
        if (idx + 3 < n_ && s_[idx + 1] == U'r' && s_[idx + 2] == U'u' && s_[idx + 3] == U'e') {
          next = idx + 4;
          return true;
        }
        break;
      case U'f':
        if (idx + 4 < n_ && s_[idx + 1] == U'a' && s_[idx + 2] == U'l' && s_[idx + 3] == U's' &&
            s_[idx + 4] == U'e') {
          next = idx + 5;
          return false;
        }
        break;
      case U'N':
        if (idx + 2 < n_ && s_[idx + 1] == U'a' && s_[idx + 2] == U'N') {
          next = idx + 3;
          return std::numeric_limits<double>::quiet_NaN();
        }
        break;
      case U'I':
        if (idx + 7 < n_ && matches(idx + 1, U"nfinity")) {
          next = idx + 8;
          return std::numeric_limits<double>::infinity();
        }
        break;
      case U'-':
        if (idx + 8 < n_ && matches(idx + 1, U"Infinity")) {
          next = idx + 9;
          return -std::numeric_limits<double>::infinity();
        }
        break;
      default:
        break;
    }
    return match_number(idx, next);
  }

  std::size_t skip_ws(std::size_t idx) const {
    while (idx < n_ && is_ws(s_[idx])) ++idx;
    return idx;
  }

  [[noreturn]] void fail(const char* msg, std::size_t pos) const { throw JsonDecodeError(msg, s_, pos); }

 private:
  bool matches(std::size_t idx, std::u32string_view word) const {
    return s_.compare(idx, word.size(), word.data(), word.size()) == 0;
  }

  ojson nested(std::size_t idx, std::size_t& next, bool object) {
    if (++depth_ > kMaxDepth) {
      throw RecursionError(object ? "maximum recursion depth exceeded while decoding a JSON object from a unicode string"
                                  : "maximum recursion depth exceeded while decoding a JSON array from a unicode string");
    }
    ojson value = object ? parse_object(idx + 1, next) : parse_array(idx + 1, next);
    --depth_;
    return value;
  }

  std::string scanstring(std::size_t end, std::size_t& next_end) {
    const std::size_t begin = end - 1;
    std::u32string chunks;
    for (;;) {
      std::size_t next = end;
      char32_t c = 0;
      for (; next < n_; ++next) {
        c = s_[next];
        if (c == U'"' || c == U'\\') break;
        if (c <= 0x1F) fail("Invalid control character at", next);
      }
      if (next >= n_ || !(c == U'"' || c == U'\\')) fail("Unterminated string starting at", begin);
      chunks.append(s_, end, next - end);
      ++next;
      if (c == U'"') {
        end = next;
        break;
      }
      if (next == n_) fail("Unterminated string starting at", begin);
      c = s_[next];
      if (c != U'u') {
        end = next + 1;
        switch (c) {
          case U'"': case U'\\': case U'/': break;
          case U'b': c = U'\b'; break;
          case U'f': c = U'\f'; break;
          case U'n': c = U'\n'; break;
          case U'r': c = U'\r'; break;
          case U't': c = U'\t'; break;
          default: c = 0;
        }
        if (c == 0) fail("Invalid \\escape", end - 2);
      } else {
        c = 0;
        ++next;
        end = next + 4;
        if (end >= n_) fail("Invalid \\uXXXX escape", next - 1);
        for (; next < end; ++next) {
          const int digit = hex_value(s_[next]);
          if (digit < 0) fail("Invalid \\uXXXX escape", end - 5);
          c = (c << 4) | static_cast<char32_t>(digit);
        }
        if (c >= 0xD800 && c <= 0xDBFF && end + 6 < n_ && s_[next++] == U'\\' && s_[next++] == U'u') {
          char32_t c2 = 0;
          end += 6;
          for (; next < end; ++next) {
            const int digit = hex_value(s_[next]);
            if (digit < 0) fail("Invalid \\uXXXX escape", end - 5);
            c2 = (c2 << 4) | static_cast<char32_t>(digit);
          }
          if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
            c = 0x10000 + (((c - 0xD800) << 10) | (c2 - 0xDC00));
          } else {
            end -= 6;
          }
        }
      }
      chunks.push_back(c);
    }
    next_end = end;
    return text::encode(chunks);
  }

  ojson parse_object(std::size_t idx, std::size_t& next) {
    ojson result = ojson::object();
    idx = skip_ws(idx);
    if (idx >= n_ || s_[idx] != U'}') {
      for (;;) {
        if (idx >= n_ || s_[idx] != U'"') fail("Expecting property name enclosed in double quotes", idx);
        std::size_t after = 0;
        std::string key = scanstring(idx + 1, after);
        idx = skip_ws(after);
        if (idx >= n_ || s_[idx] != U':') fail("Expecting ':' delimiter", idx);
        idx = skip_ws(idx + 1);
        ojson value = scan_once(idx, after);
        result[key] = std::move(value);  // duplicate key: first position, last value (like dict)
        idx = skip_ws(after);
        if (idx < n_ && s_[idx] == U'}') break;
        if (idx >= n_ || s_[idx] != U',') fail("Expecting ',' delimiter", idx);
        idx = skip_ws(idx + 1);
      }
    }
    next = idx + 1;
    return result;
  }

  ojson parse_array(std::size_t idx, std::size_t& next) {
    ojson result = ojson::array();
    idx = skip_ws(idx);
    if (idx >= n_ || s_[idx] != U']') {
      for (;;) {
        std::size_t after = 0;
        result.push_back(scan_once(idx, after));
        idx = skip_ws(after);
        if (idx < n_ && s_[idx] == U']') break;
        if (idx >= n_ || s_[idx] != U',') fail("Expecting ',' delimiter", idx);
        idx = skip_ws(idx + 1);
      }
    }
    next = idx + 1;
    return result;
  }

  ojson match_number(std::size_t start, std::size_t& next) {
    std::size_t idx = start;
    if (s_[idx] == U'-') {
      ++idx;
      if (idx >= n_) fail("Expecting value", start);
    }
    if (s_[idx] >= U'1' && s_[idx] <= U'9') {
      ++idx;
      while (idx < n_ && is_digit(s_[idx])) ++idx;
    } else if (s_[idx] == U'0') {
      ++idx;
    } else {
      fail("Expecting value", start);
    }
    bool is_float = false;
    if (idx + 1 < n_ && s_[idx] == U'.' && is_digit(s_[idx + 1])) {
      is_float = true;
      idx += 2;
      while (idx < n_ && is_digit(s_[idx])) ++idx;
    }
    if (idx + 1 < n_ && (s_[idx] == U'e' || s_[idx] == U'E')) {
      const std::size_t e_start = idx;
      ++idx;
      if (idx + 1 < n_ && (s_[idx] == U'-' || s_[idx] == U'+')) ++idx;
      while (idx < n_ && is_digit(s_[idx])) ++idx;
      if (is_digit(s_[idx - 1])) {
        is_float = true;
      } else {
        idx = e_start;
      }
    }
    std::string number;
    for (std::size_t i = start; i < idx; ++i) number.push_back(static_cast<char>(s_[i]));
    next = idx;
    if (!is_float) {
      std::int64_t signed_value = 0;
      auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), signed_value);
      if (ec == std::errc() && ptr == number.data() + number.size()) return signed_value;
      if (number.front() != '-') {
        std::uint64_t unsigned_value = 0;
        auto [uptr, uec] = std::from_chars(number.data(), number.data() + number.size(), unsigned_value);
        if (uec == std::errc() && uptr == number.data() + number.size()) return unsigned_value;
      }
      const std::size_t digits = number.size() - (number.front() == '-' ? 1 : 0);
      if (digits > 4300) {  // sys.get_int_max_str_digits()
        throw ValueError("Exceeds the limit (4300 digits) for integer string conversion: value has " +
                         std::to_string(digits) + " digits; use sys.set_int_max_str_digits() to increase the limit");
      }
      return ojson::binary(std::vector<std::uint8_t>(number.begin(), number.end()), kBigInt);
    }
    return std::strtod(number.c_str(), nullptr);  // overflow -> +-inf like float()
  }

  const std::u32string& s_;
  std::size_t n_;
  int depth_ = 0;
};

template <class J>
bool big(const J& value) {
  return value.is_binary() && value.get_binary().has_subtype() && value.get_binary().subtype() == kBigInt;
}

template <class J>
std::string big_digits(const J& value) {
  const auto& bytes = value.get_binary();
  return std::string(bytes.begin(), bytes.end());
}

void escape_u(std::string& out, char32_t value) {
  static const char* kHex = "0123456789abcdef";
  out += "\\u";
  for (int shift = 12; shift >= 0; shift -= 4) out.push_back(kHex[(value >> shift) & 0xF]);
}

void dump_string(const std::string& value, std::string& out, bool ensure_ascii) {
  // c_encode_basestring: '"', '\\' and C0 controls are escaped (and every non-ASCII character with ensure_ascii).
  static const char* kHex = "0123456789abcdef";
  out.push_back('"');
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    const auto c = static_cast<unsigned char>(ch);
    if (ensure_ascii && c >= 0x80) {
      std::size_t end = i + 1;
      while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) ++end;
      for (char32_t cp : text::decode(std::string_view(value).substr(i, end - i))) {
        if (cp >= 0x10000) {
          escape_u(out, 0xD800 + ((cp - 0x10000) >> 10));
          escape_u(out, 0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
          escape_u(out, cp);
        }
      }
      i = end - 1;
      continue;
    }
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c <= 0x1F) {
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

template <class J>
void dump_value(const J& value, std::string& out, bool compact, bool ensure_ascii) {
  const char* item_separator = compact ? "," : ", ";
  const char* key_separator = compact ? ":" : ": ";
  switch (value.type()) {
    case value_t::null: out += "null"; break;
    case value_t::boolean: out += value.template get<bool>() ? "true" : "false"; break;
    case value_t::number_integer: out += std::to_string(value.template get<std::int64_t>()); break;
    case value_t::number_unsigned: out += std::to_string(value.template get<std::uint64_t>()); break;
    case value_t::number_float: {
      const double number = value.template get<double>();
      if (std::isnan(number)) {
        out += "NaN";
      } else if (std::isinf(number)) {
        out += number > 0 ? "Infinity" : "-Infinity";
      } else {
        out += text::float_repr(number);
      }
      break;
    }
    case value_t::string: dump_string(value.template get_ref<const std::string&>(), out, ensure_ascii); break;
    case value_t::array: {
      out.push_back('[');
      bool first = true;
      for (const auto& item : value) {
        if (!first) out += item_separator;
        first = false;
        dump_value(item, out, compact, ensure_ascii);
      }
      out.push_back(']');
      break;
    }
    case value_t::object: {
      out.push_back('{');
      bool first = true;
      for (auto it = value.begin(); it != value.end(); ++it) {
        if (!first) out += item_separator;
        first = false;
        dump_string(it.key(), out, ensure_ascii);
        out += key_separator;
        dump_value(it.value(), out, compact, ensure_ascii);
      }
      out.push_back('}');
      break;
    }
    case value_t::binary:
      if (big(value)) {
        out += big_digits(value);
        break;
      }
      [[fallthrough]];
    default:
      throw std::invalid_argument("value is not JSON serializable");
  }
}

template <class J>
void repr_value(const J& value, std::string& out) {
  switch (value.type()) {
    case value_t::null: out += "None"; break;
    case value_t::boolean: out += value.template get<bool>() ? "True" : "False"; break;
    case value_t::number_integer: out += std::to_string(value.template get<std::int64_t>()); break;
    case value_t::number_unsigned: out += std::to_string(value.template get<std::uint64_t>()); break;
    case value_t::number_float: out += text::float_repr(value.template get<double>()); break;
    case value_t::string: out += text::repr(value.template get_ref<const std::string&>()); break;
    case value_t::array: {
      out.push_back('[');
      bool first = true;
      for (const auto& item : value) {
        if (!first) out += ", ";
        first = false;
        repr_value(item, out);
      }
      out.push_back(']');
      break;
    }
    case value_t::object: {
      out.push_back('{');
      bool first = true;
      for (auto it = value.begin(); it != value.end(); ++it) {
        if (!first) out += ", ";
        first = false;
        out += text::repr(it.key());
        out += ": ";
        repr_value(it.value(), out);
      }
      out.push_back('}');
      break;
    }
    default:
      out += big(value) ? big_digits(value) : std::string("<binary>");
  }
}

template <class J>
bool truthy_value(const J& value) {
  switch (value.type()) {
    case value_t::null: return false;
    case value_t::boolean: return value.template get<bool>();
    case value_t::number_integer: return value.template get<std::int64_t>() != 0;
    case value_t::number_unsigned: return value.template get<std::uint64_t>() != 0;
    case value_t::number_float: return value.template get<double>() != 0.0 || std::isnan(value.template get<double>());
    case value_t::string: return !value.template get_ref<const std::string&>().empty();
    case value_t::array:
    case value_t::object: return !value.empty();
    default: return true;
  }
}

bool numeric(const ojson& v) { return v.is_number() || v.is_boolean() || big(v); }

// Exact decimal text of an integral value (bool, int64, uint64, big int) or of an integral double.
std::string int_text(const ojson& v) {
  if (big(v)) return big_digits(v);
  if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
  if (v.is_number_unsigned()) return std::to_string(v.get<std::uint64_t>());
  return std::to_string(v.get<std::int64_t>());
}

std::string exact_text(double integral) {
  char buffer[512];
  std::snprintf(buffer, sizeof buffer, "%.0f", integral);  // glibc prints the exact value
  return std::string(buffer) == "-0" ? "0" : buffer;
}

// Signed comparison of decimal integers without leading zeros.
int compare_decimal(const std::string& a, const std::string& b) {
  const bool an = !a.empty() && a.front() == '-';
  const bool bn = !b.empty() && b.front() == '-';
  if (an != bn) return an ? -1 : 1;
  const std::string_view am = std::string_view(a).substr(an ? 1 : 0);
  const std::string_view bm = std::string_view(b).substr(bn ? 1 : 0);
  int magnitude = am.size() != bm.size() ? (am.size() < bm.size() ? -1 : 1) : am.compare(bm) < 0 ? -1 : am.compare(bm) > 0;
  return an ? -magnitude : magnitude;
}

// Exact comparison of a 64-bit integer (as sign + magnitude) with a double.
int compare_int_double(bool negative, std::uint64_t magnitude, double d) {
  // returns -1, 0, 1 for int < d, ==, >; d is finite
  if (negative && d >= 0) return -1;
  if (!negative && d < 0) return magnitude == 0 && d == 0 ? 0 : 1;
  const double ad = std::fabs(d);
  if (ad >= 18446744073709551616.0) return negative ? 1 : -1;  // |d| >= 2**64 > |int|
  const double whole = std::floor(ad);
  const auto whole_int = static_cast<std::uint64_t>(whole);
  int sign_free = 0;  // compare magnitude with ad
  if (magnitude < whole_int) {
    sign_free = -1;
  } else if (magnitude > whole_int) {
    sign_free = 1;
  } else {
    sign_free = ad > whole ? -1 : 0;
  }
  return negative ? -sign_free : sign_free;
}

void split_int(const ojson& v, bool& negative, std::uint64_t& magnitude) {
  if (v.is_boolean()) {
    negative = false;
    magnitude = v.get<bool>() ? 1 : 0;
  } else if (v.is_number_unsigned()) {
    negative = false;
    magnitude = v.get<std::uint64_t>();
  } else {
    const auto value = v.get<std::int64_t>();
    negative = value < 0;
    magnitude = negative ? static_cast<std::uint64_t>(-(value + 1)) + 1 : static_cast<std::uint64_t>(value);
  }
}

bool numbers_equal(const ojson& a, const ojson& b) {
  if (big(a) || big(b)) {
    if (a.is_number_float() || b.is_number_float()) {
      const double d = a.is_number_float() ? a.get<double>() : b.get<double>();
      if (!std::isfinite(d) || std::floor(d) != d) return false;
      return compare_decimal(int_text(a.is_number_float() ? b : a), exact_text(d)) == 0;
    }
    return compare_decimal(int_text(a), int_text(b)) == 0;
  }
  const bool a_float = a.is_number_float();
  const bool b_float = b.is_number_float();
  if (a_float && b_float) {
    const double x = a.get<double>();
    const double y = b.get<double>();
    return x == y || (std::isnan(x) && std::isnan(y));
  }
  if (a_float || b_float) {
    const double d = a_float ? a.get<double>() : b.get<double>();
    if (!std::isfinite(d)) return false;
    bool negative = false;
    std::uint64_t magnitude = 0;
    split_int(a_float ? b : a, negative, magnitude);
    return compare_int_double(negative, magnitude, d) == 0;
  }
  bool an = false, bn = false;
  std::uint64_t am = 0, bm = 0;
  split_int(a, an, am);
  split_int(b, bn, bm);
  return an == bn && am == bm;
}

int compare_number(const ojson& value, double bound) {
  if (big(value)) {
    if (std::isnan(bound)) return 0;
    if (std::isinf(bound)) return bound > 0 ? -1 : 1;
    const double whole = std::floor(bound);
    const int order = compare_decimal(big_digits(value), exact_text(whole));
    if (order != 0) return order;
    return bound > whole ? -1 : 0;
  }
  if (value.is_number_float()) {
    const double d = value.get<double>();
    if (d < bound) return -1;
    if (d > bound) return 1;
    return 0;  // NaN compares as neither
  }
  bool negative = false;
  std::uint64_t magnitude = 0;
  split_int(value, negative, magnitude);
  if (std::isnan(bound)) return 0;
  if (std::isinf(bound)) return bound > 0 ? -1 : 1;
  return compare_int_double(negative, magnitude, bound);
}

template <class From, class To>
To convert(const From& value) {
  switch (value.type()) {
    case value_t::array: {
      To out = To::array();
      for (const auto& item : value) out.push_back(convert<From, To>(item));
      return out;
    }
    case value_t::object: {
      To out = To::object();
      for (auto it = value.begin(); it != value.end(); ++it) out[it.key()] = convert<From, To>(it.value());
      return out;
    }
    case value_t::null: return To(nullptr);
    case value_t::boolean: return To(value.template get<bool>());
    case value_t::number_integer: return To(value.template get<std::int64_t>());
    case value_t::number_unsigned: return To(value.template get<std::uint64_t>());
    case value_t::number_float: return To(value.template get<double>());
    case value_t::string: return To(value.template get_ref<const std::string&>());
    case value_t::binary:  // big int -> nearest float for the std::map JSON used by hm_common
      if (big(value)) return To(std::strtod(big_digits(value).c_str(), nullptr));
      return To(nullptr);
    default: return To(nullptr);
  }
}

}  // namespace

JsonDecodeError::JsonDecodeError(const std::string& msg, const std::u32string& doc, std::size_t pos)
    : std::runtime_error(error_text(msg, doc, pos)), msg_(msg), pos_(pos) {}

ojson loads(std::string_view text_value) {
  const std::u32string doc = text::decode(text_value);
  Scanner scanner(doc);
  if (!doc.empty() && doc.front() == 0xFEFF) scanner.fail("Unexpected UTF-8 BOM (decode using utf-8-sig)", 0);
  std::size_t end = 0;
  ojson value = scanner.scan_once(scanner.skip_ws(0), end);
  end = scanner.skip_ws(end);
  if (end != doc.size()) scanner.fail("Extra data", end);
  return value;
}

std::string dumps(const ojson& value, bool compact, bool ensure_ascii) {
  std::string out;
  dump_value(value, out, compact, ensure_ascii);
  return out;
}

std::string dumps(const nlohmann::json& value, bool compact, bool ensure_ascii) {
  std::string out;
  dump_value(value, out, compact, ensure_ascii);
  return out;
}

std::string repr(const ojson& value) {
  std::string out;
  repr_value(value, out);
  return out;
}

std::string repr(const nlohmann::json& value) {
  std::string out;
  repr_value(value, out);
  return out;
}

bool truthy(const ojson& value) { return truthy_value(value); }
bool truthy(const nlohmann::json& value) { return truthy_value(value); }

bool equal(const ojson& a, const ojson& b) {
  if (numeric(a) && numeric(b)) return numbers_equal(a, b);
  if (a.type() != b.type()) return false;
  switch (a.type()) {
    case value_t::null: return true;
    case value_t::string: return a.get_ref<const std::string&>() == b.get_ref<const std::string&>();
    case value_t::array: {
      if (a.size() != b.size()) return false;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (!equal(a[i], b[i])) return false;
      }
      return true;
    }
    case value_t::object: {
      if (a.size() != b.size()) return false;
      for (auto it = a.begin(); it != a.end(); ++it) {
        auto other = b.find(it.key());
        if (other == b.end() || !equal(it.value(), *other)) return false;
      }
      return true;
    }
    default: return a == b;
  }
}

std::string type_name(const ojson& value) {
  switch (value.type()) {
    case value_t::null: return "NoneType";
    case value_t::boolean: return "bool";
    case value_t::number_integer:
    case value_t::number_unsigned: return "int";
    case value_t::number_float: return "float";
    case value_t::string: return "str";
    case value_t::array: return "list";
    case value_t::object: return "dict";
    default: return big(value) ? "int" : "bytes";
  }
}

bool is_int(const ojson& value) { return value.is_number_integer() || big(value); }
bool is_number(const ojson& value) { return value.is_number() || big(value); }
bool is_big_int(const ojson& value) { return big(value); }
std::string big_int_digits(const ojson& value) { return big_digits(value); }
bool less_than(const ojson& value, double bound) { return compare_number(value, bound) < 0; }
bool greater_than(const ojson& value, double bound) { return compare_number(value, bound) > 0; }

double round_digits(double value, int ndigits) {
  if (!std::isfinite(value) || ndigits > 300 || ndigits < 0) return value;
  // CPython double_round: _Py_dg_dtoa(mode 3) then strtod; glibc printf rounds the exact value the same way.
  char buffer[512];
  std::snprintf(buffer, sizeof buffer, "%.*f", ndigits, value);
  return std::strtod(buffer, nullptr);
}

ojson to_ordered(const nlohmann::json& value) { return convert<nlohmann::json, ojson>(value); }
nlohmann::json to_plain(const ojson& value) { return convert<ojson, nlohmann::json>(value); }

}  // namespace hm::py
