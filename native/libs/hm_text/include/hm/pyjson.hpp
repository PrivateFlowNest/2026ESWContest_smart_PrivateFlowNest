// Python json / object semantics for code that must match the Python reference exactly:
// json.loads (CPython 3.12 C scanner, same error messages), json.dumps(ensure_ascii=False), repr(),
// ==, truthiness. Objects keep insertion order (nlohmann::ordered_json) like Python dicts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace hm::py {

using ojson = nlohmann::ordered_json;

// json.JSONDecodeError. msg() is exc.msg ("Expecting value", "Extra data", ...);
// what() is str(exc) ("Expecting value: line 1 column 1 (char 0)").
class JsonDecodeError : public std::runtime_error {
 public:
  JsonDecodeError(const std::string& msg, const std::u32string& doc, std::size_t pos);
  const std::string& msg() const { return msg_; }
  std::size_t pos() const { return pos_; }

 private:
  std::string msg_;
  std::size_t pos_;
};

// RecursionError while decoding very deep documents (Python raises it too; not a ValueError).
class RecursionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// ValueError raised by json.loads outside the decoder proper (int() digit limit of 4300 digits).
class ValueError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// json.loads(text). Accepts NaN/Infinity/-Infinity; duplicate keys keep the first position and the last value.
// Integers outside int64/uint64 stay exact Python ints: they are held as a binary value (subtype kBigInt)
// with the decimal digits, which every helper here treats as an int.
ojson loads(std::string_view text);
inline constexpr std::uint8_t kBigInt = 0x49;
bool is_big_int(const ojson& value);
std::string big_int_digits(const ojson& value);  // "-123..."

// json.dumps(value, ensure_ascii=...): separators (", ", ": "), or (",", ":") when compact.
std::string dumps(const ojson& value, bool compact = false, bool ensure_ascii = false);
std::string dumps(const nlohmann::json& value, bool compact = false, bool ensure_ascii = false);

// repr(value) of the Python object json.loads would build.
std::string repr(const ojson& value);
std::string repr(const nlohmann::json& value);

// bool(value)
bool truthy(const ojson& value);
bool truthy(const nlohmann::json& value);

// a == b (True == 1 == 1.0, dict order ignored). NaN equals NaN: json.loads returns one shared NaN object
// and containers, `in` and set() compare identity first.
bool equal(const ojson& a, const ojson& b);

// type(value).__name__ (NoneType, bool, int, float, str, list, dict)
std::string type_name(const ojson& value);

// isinstance(value, int) and not bool / isinstance(value, (int, float)) and not bool
bool is_int(const ojson& value);
bool is_number(const ojson& value);
// value < bound / value > bound for an int or float value (exact for 64-bit integers).
bool less_than(const ojson& value, double bound);
bool greater_than(const ojson& value, double bound);

// round(value, ndigits) for a float: correctly rounded on the exact binary value, ties to even.
double round_digits(double value, int ndigits);

ojson to_ordered(const nlohmann::json& value);
nlohmann::json to_plain(const ojson& value);

}  // namespace hm::py
