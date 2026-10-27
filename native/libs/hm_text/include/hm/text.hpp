// UTF-8 text helpers with Python str semantics (services/exaone-edge and services/runtime are the reference).
// Strings stay UTF-8 std::string. Lone surrogates (possible in Python str via JSON "\ud800") are carried as
// 3-byte WTF-8 sequences so that lengths, repr and JSON round trips keep Python's behaviour.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace hm::text {

// Lenient decode: invalid bytes become U+FFFD (Python str never holds invalid UTF-8, so callers sanitize first).
std::u32string decode(std::string_view utf8);
std::string encode(std::u32string_view code_points);
void append_utf8(std::string& out, char32_t code_point);
// Valid UTF-8 (surrogates rejected), i.e. what Python bytes.decode("utf-8") accepts.
bool is_valid_utf8(std::string_view bytes);
// Python bytes.decode("utf-8") error text ("'utf-8' codec can't decode byte 0xff in position 3: invalid start
// byte"), nullopt when the bytes decode cleanly.
std::optional<std::string> utf8_decode_error(std::string_view bytes);
// Replace invalid sequences with U+FFFD (input from stdin/sockets before it is treated as a Python str).
std::string sanitize(std::string_view bytes);

// len(str): number of code points.
std::size_t length(std::string_view utf8);
// str[:count]
std::string prefix(std::string_view utf8, std::size_t count);

// str.isspace() for one character (Py_UNICODE_ISSPACE).
bool is_space(char32_t c);
// str.isprintable() for one character (repr() keeps printable characters as they are).
bool is_printable(char32_t c);
// Decimal digit value of a Unicode Nd character (what re \d matches and int() accepts), or -1.
int decimal_value(char32_t c);

// str.strip() with no arguments.
std::string strip(std::string_view utf8);
// str.casefold(): full Unicode case folding (CaseFolding.txt C + F), via utf8proc.
std::string casefold(std::string_view utf8);

// repr(str)
std::string repr(std::string_view utf8);
// repr(float): shortest round-trip digits, exponent form outside 1e-4 <= |x| < 1e16, "inf"/"nan".
std::string float_repr(double value);

}  // namespace hm::text
