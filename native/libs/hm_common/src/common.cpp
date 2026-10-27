#include "hm/common.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>

namespace hm {

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string new_id() {
  thread_local std::mt19937_64 engine{[] {
    std::random_device device;  // /dev/urandom on Linux
    std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
    return std::mt19937_64(seed);
  }()};
  std::uint64_t high = engine();
  std::uint64_t low = engine();
  high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
  low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;    // RFC 4122 variant
  char text[37];
  std::snprintf(text, sizeof text, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(high >> 32),
                static_cast<unsigned>((high >> 16) & 0xFFFF), static_cast<unsigned>(high & 0xFFFF),
                static_cast<unsigned>(low >> 48), static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
  return text;
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  std::string text = buffer.str();
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // utf-8-sig
  return text;
}

json read_json_file(const std::filesystem::path& path) {
  try {
    return json::parse(read_text_file(path));
  } catch (const json::parse_error& error) {
    throw std::runtime_error(path.string() + ": " + error.what());
  }
}

double round_to(double value, int digits) {
  // Python round(x, n): round the exact binary value to n decimals (ties to even), then back to the nearest
  // double. glibc printf rounds the exact value correctly, so "%.*f" + strtod gives the same result.
  if (!std::isfinite(value)) return value;
  char text[512];
  std::snprintf(text, sizeof text, "%.*f", digits, value);
  return std::strtod(text, nullptr);
}

double confidence4(double value) { return round_to(std::clamp(value, 0.0, 1.0), 4); }

std::string dump_compact(const json& document) {
  return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::info)};
std::mutex g_log_mutex;
}  // namespace

void set_log_level(LogLevel level) { g_level = static_cast<int>(level); }

LogLevel parse_log_level(std::string_view name) {
  std::string upper(name);
  std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
  if (upper == "DEBUG") return LogLevel::debug;
  if (upper == "WARNING" || upper == "WARN") return LogLevel::warning;
  if (upper == "ERROR") return LogLevel::error;
  return LogLevel::info;
}

void log(LogLevel level, std::string_view component, std::string_view message) {
  if (static_cast<int>(level) < g_level.load()) return;
  static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR"};
  std::lock_guard lock(g_log_mutex);
  std::fprintf(stderr, "%s %.*s %.*s\n", names[static_cast<int>(level)], static_cast<int>(component.size()),
               component.data(), static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

}  // namespace hm
