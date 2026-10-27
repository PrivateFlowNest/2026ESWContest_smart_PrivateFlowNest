// Python-compatible number and datetime formatting for the context events (parity with the Python reference).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "hm/common.hpp"

namespace hm::ctx {

// round(value, ndigits) exactly like CPython (correctly rounded decimal, ties to even), ndigits >= 0.
double py_round(double value, int ndigits);
// f"{value:.{decimals}f}"
std::string py_fixed(double value, int decimals);
// repr(float): shortest round-trip text ("0.1", "3.0", "1e-05").
std::string py_repr(double value);
// str() of a JSON number as Python sees it: integer -> "3", float -> repr.
std::string py_str(const json& number);
// Python truthiness-free conversion of a JSON number (int or float) to double; throws on other types.
double number(const json& value, std::string_view name);

// Instants are int64 microseconds since the Unix epoch, UTC (Python datetime resolution).
using Micros = std::int64_t;
// datetime.fromisoformat(text.replace("Z", "+00:00")) converted to UTC (naive -> UTC).
// Throws std::invalid_argument for text Python rejects (common ISO forms are supported).
Micros parse_iso(std::string_view text);
// UTC datetime.isoformat(): 2026-10-22T01:02:03.456000+00:00 (fraction omitted when zero).
std::string iso_python(Micros value);
// datetime.isoformat(timespec="milliseconds").replace("+00:00", "Z") of a UTC datetime.
std::string iso_millis_z(Micros value);
// timedelta(seconds=value) in microseconds (round half to even).
Micros seconds_to_micros(double seconds);

}  // namespace hm::ctx
