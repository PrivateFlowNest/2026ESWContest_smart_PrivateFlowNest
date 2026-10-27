#include "hm/context/pyfmt.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace hm::ctx {

double py_round(double value, int ndigits) {
  if (!std::isfinite(value)) return value;
  // glibc printf is exact on the binary value and rounds ties to even, like CPython's dtoa mode 3.
  char buffer[400];
  std::snprintf(buffer, sizeof buffer, "%.*f", ndigits, value);
  return std::strtod(buffer, nullptr);
}

std::string py_fixed(double value, int decimals) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
  char buffer[400];
  std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
  return buffer;
}

std::string py_repr(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
  if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
  char buffer[48];
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buffer, sizeof buffer, "%.*e", precision - 1, value);
    if (std::strtod(buffer, nullptr) == value) break;
  }
  std::string text(buffer);
  std::string sign;
  if (text[0] == '-') {
    sign = "-";
    text.erase(0, 1);
  }
  const auto e = text.find('e');
  const int exponent = std::atoi(text.c_str() + e + 1);
  std::string digits;
  for (std::size_t i = 0; i < e; ++i)
    if (text[i] != '.') digits += text[i];
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  const int decpt = exponent + 1;
  const int count = static_cast<int>(digits.size());
  if (decpt <= -4 || decpt > 16) {  // CPython repr switches to exponent notation here
    std::string out = sign + digits.substr(0, 1);
    if (count > 1) out += "." + digits.substr(1);
    char tail[16];
    std::snprintf(tail, sizeof tail, "e%c%02d", exponent < 0 ? '-' : '+', std::abs(exponent));
    return out + tail;
  }
  if (decpt <= 0) return sign + "0." + std::string(static_cast<std::size_t>(-decpt), '0') + digits;
  if (decpt >= count) return sign + digits + std::string(static_cast<std::size_t>(decpt - count), '0') + ".0";
  return sign + digits.substr(0, static_cast<std::size_t>(decpt)) + "." + digits.substr(static_cast<std::size_t>(decpt));
}

std::string py_str(const json& value) {
  if (value.is_number_integer()) return value.dump();
  if (value.is_number_float()) return py_repr(value.get<double>());
  if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
  if (value.is_string()) return value.get<std::string>();
  return value.dump();
}

double number(const json& value, std::string_view name) {
  if (!value.is_number()) throw std::invalid_argument(std::string(name) + " must be a number");
  return value.get<double>();
}

namespace {

// Howard Hinnant's civil calendar algorithms (proleptic Gregorian).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2));
}

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned days_in_month(int y, unsigned m) {
  static const unsigned table[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : table[m - 1];
}

[[noreturn]] void bad(std::string_view text) {
  throw std::invalid_argument("Invalid isoformat string: '" + std::string(text) + "'");
}

class Cursor {
 public:
  explicit Cursor(std::string_view text) : text_(text) {}
  bool done() const { return pos_ >= text_.size(); }
  char peek() const { return done() ? '\0' : text_[pos_]; }
  std::size_t pos() const { return pos_; }
  void skip() { ++pos_; }
  bool digit() const { return !done() && text_[pos_] >= '0' && text_[pos_] <= '9'; }
  int digits(std::size_t count, std::string_view original) {
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (!digit()) bad(original);
      value = value * 10 + (text_[pos_++] - '0');
    }
    return value;
  }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
};

// HH[:MM[:SS[.ffffff]]] or HH[MM[SS[.ffffff]]] -> microseconds of day (fraction truncated to 6 digits).
std::int64_t parse_clock(Cursor& cursor, std::string_view original, bool allow_fraction) {
  const int hour = cursor.digits(2, original);
  int minute = 0, second = 0, micro = 0;
  const bool extended = cursor.peek() == ':';
  auto component = [&](int& out) {
    if (extended) {
      if (cursor.peek() != ':') return false;
      cursor.skip();
    } else if (!cursor.digit()) {
      return false;
    }
    out = cursor.digits(2, original);
    return true;
  };
  if (component(minute) && component(second) && allow_fraction && (cursor.peek() == '.' || cursor.peek() == ',')) {
    cursor.skip();
    if (!cursor.digit()) bad(original);
    int parsed = 0;
    while (cursor.digit()) {
      if (parsed < 6) {
        micro = micro * 10 + (cursor.peek() - '0');
        ++parsed;
      }
      cursor.skip();
    }
    while (parsed < 6) {
      micro *= 10;
      ++parsed;
    }
  }
  if (hour > 23 || minute > 59 || second > 59) bad(original);
  return ((static_cast<std::int64_t>(hour) * 60 + minute) * 60 + second) * 1000000LL + micro;
}

}  // namespace

Micros parse_iso(std::string_view original) {
  std::string text;
  for (char c : original) {
    if (c == 'Z') text += "+00:00";
    else text += c;
  }
  Cursor cursor(text);
  const int year = cursor.digits(4, original);
  unsigned month, day;
  if (cursor.peek() == '-') {
    cursor.skip();
    month = static_cast<unsigned>(cursor.digits(2, original));
    if (cursor.peek() != '-') bad(original);
    cursor.skip();
    day = static_cast<unsigned>(cursor.digits(2, original));
  } else {
    month = static_cast<unsigned>(cursor.digits(2, original));
    day = static_cast<unsigned>(cursor.digits(2, original));
  }
  if (year < 1 || month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) bad(original);
  std::int64_t time_of_day = 0, offset = 0;
  if (!cursor.done()) {
    cursor.skip();  // any single separator character (Python 3.11+)
    if (cursor.done()) bad(original);
    time_of_day = parse_clock(cursor, original, true);
    if (!cursor.done()) {
      const char sign = cursor.peek();
      if (sign != '+' && sign != '-') bad(original);
      cursor.skip();
      offset = parse_clock(cursor, original, true);
      if (offset >= 86400000000LL) bad(original);
      if (sign == '-') offset = -offset;
      if (!cursor.done()) bad(original);
    }
  }
  const std::int64_t days = days_from_civil(year, month, day);
  return days * 86400000000LL + time_of_day - offset;
}

namespace {

std::string clock_text(Micros value, std::int64_t& micro) {
  std::int64_t seconds = value / 1000000;
  micro = value % 1000000;
  if (micro < 0) {
    micro += 1000000;
    --seconds;
  }
  std::int64_t days = seconds / 86400;
  std::int64_t rest = seconds % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  int y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02uT%02d:%02d:%02d", y, m, d, static_cast<int>(rest / 3600),
                static_cast<int>(rest / 60 % 60), static_cast<int>(rest % 60));
  return buffer;
}

}  // namespace

std::string iso_python(Micros value) {
  std::int64_t micro = 0;
  std::string out = clock_text(value, micro);
  if (micro != 0) {
    char fraction[16];
    std::snprintf(fraction, sizeof fraction, ".%06d", static_cast<int>(micro));
    out += fraction;
  }
  return out + "+00:00";
}

std::string iso_millis_z(Micros value) {
  std::int64_t micro = 0;
  std::string out = clock_text(value, micro);
  char fraction[16];
  std::snprintf(fraction, sizeof fraction, ".%03dZ", static_cast<int>(micro / 1000));
  return out + fraction;
}

Micros seconds_to_micros(double seconds) { return static_cast<Micros>(std::nearbyint(seconds * 1e6)); }

}  // namespace hm::ctx
