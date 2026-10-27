#include "hm/pyregex.hpp"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace hm::py {

namespace {

// Python's \s for str patterns: exactly the characters for which str.isspace() is true.
constexpr std::string_view kSpaceSet =
    "\\t\\n\\x0b\\f\\r\\x1c-\\x1f \\x{85}\\x{a0}\\x{1680}\\x{2000}-\\x{200a}\\x{2028}\\x{2029}\\x{202f}"
    "\\x{205f}\\x{3000}";

struct MatchData {
  pcre2_match_data* data;
  explicit MatchData(const pcre2_code* code) : data(pcre2_match_data_create_from_pattern(code, nullptr)) {
    if (!data) throw std::bad_alloc();
  }
  ~MatchData() { pcre2_match_data_free(data); }
  MatchData(const MatchData&) = delete;
  MatchData& operator=(const MatchData&) = delete;
};

std::string error_message(int code) {
  PCRE2_UCHAR buffer[256];
  pcre2_get_error_message(code, buffer, sizeof buffer);
  return reinterpret_cast<const char*>(buffer);
}

}  // namespace

std::string translate_python_pattern(std::string_view pattern) {
  std::string out;
  bool in_class = false;
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    const char c = pattern[i];
    if (c == '\\' && i + 1 < pattern.size()) {
      const char escaped = pattern[++i];
      switch (escaped) {
        case 's':
          if (in_class) {
            out += kSpaceSet;
          } else {
            out += "[";
            out += kSpaceSet;
            out += "]";
          }
          break;
        case 'S':
          if (in_class) throw std::invalid_argument("\\S inside a character class is not supported");
          out += "[^";
          out += kSpaceSet;
          out += "]";
          break;
        case 'd': out += "\\p{Nd}"; break;
        case 'D':
          if (in_class) throw std::invalid_argument("\\D inside a character class is not supported");
          out += "\\P{Nd}";
          break;
        default:
          out.push_back(c);
          out.push_back(escaped);
      }
      continue;
    }
    if (!in_class && c == '[') {
      in_class = true;
      out.push_back(c);
      if (i + 1 < pattern.size() && pattern[i + 1] == '^') out.push_back(pattern[++i]);
      if (i + 1 < pattern.size() && pattern[i + 1] == ']') out.push_back(pattern[++i]);
      continue;
    }
    if (in_class && c == ']') in_class = false;
    out.push_back(c);
  }
  return out;
}

std::string Match::group(std::string_view subject, std::size_t index) const {
  if (index >= groups.size() || groups[index].first == std::string::npos) return {};
  return std::string(subject.substr(groups[index].first, groups[index].second - groups[index].first));
}

Regex::Regex(std::string_view python_pattern) : translated_(translate_python_pattern(python_pattern)) {
  int error = 0;
  PCRE2_SIZE offset = 0;
  code_ = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(translated_.data()), translated_.size(), PCRE2_UTF | PCRE2_UCP,
                        &error, &offset, nullptr);
  if (!code_) {
    throw std::invalid_argument("regex " + std::string(python_pattern) + ": " + error_message(error) + " at " +
                                std::to_string(offset));
  }
}

Regex::~Regex() { pcre2_code_free(code_); }

std::optional<Match> Regex::search(std::string_view subject, std::size_t from) const {
  MatchData match(code_);
  const int rc = pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(), from, 0,
                             match.data, nullptr);
  if (rc == PCRE2_ERROR_NOMATCH) return std::nullopt;
  if (rc < 0) throw std::runtime_error("regex match failed: " + error_message(rc));
  const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match.data);
  const std::uint32_t pairs = pcre2_get_ovector_count(match.data);
  Match result;
  result.start = ovector[0];
  result.end = ovector[1];
  for (std::uint32_t i = 0; i < pairs; ++i) {
    if (ovector[2 * i] == PCRE2_UNSET) {
      result.groups.emplace_back(std::string::npos, std::string::npos);
    } else {
      result.groups.emplace_back(ovector[2 * i], ovector[2 * i + 1]);
    }
  }
  return result;
}

std::vector<std::size_t> Regex::finditer_starts(std::string_view subject) const {
  std::vector<std::size_t> starts;
  std::size_t from = 0;
  while (from <= subject.size()) {
    const auto found = search(subject, from);
    if (!found) break;
    starts.push_back(found->start);
    if (found->end > found->start) {
      from = found->end;
    } else {  // empty match: step over one code point
      from = found->end + 1;
      while (from < subject.size() && (static_cast<unsigned char>(subject[from]) & 0xC0) == 0x80) ++from;
    }
  }
  return starts;
}

}  // namespace hm::py
