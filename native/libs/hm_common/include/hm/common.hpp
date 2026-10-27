// Small shared helpers: JSON alias, time, ids, files, logging.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace hm {

using json = nlohmann::json;

std::int64_t now_ms();
std::string new_id();  // RFC 4122 version 4 UUID, lowercase
std::string read_text_file(const std::filesystem::path& path);
json read_json_file(const std::filesystem::path& path);
// Clamp to [0, 1] and round to 4 decimals (pfn.v1 confidence fields).
double confidence4(double value);
// Round to `digits` decimals, like Python round() for the values we publish.
double round_to(double value, int digits);
// Compact UTF-8 JSON (ensure_ascii=False, separators=(",", ":")), like the Python Bus.
std::string dump_compact(const json& document);

enum class LogLevel { debug = 0, info = 1, warning = 2, error = 3 };
void set_log_level(LogLevel level);
LogLevel parse_log_level(std::string_view name);  // DEBUG|INFO|WARNING|ERROR (case-insensitive)
// One line to stderr (journald adds the timestamp): "<LEVEL> <component> <message>".
void log(LogLevel level, std::string_view component, std::string_view message);

}  // namespace hm
