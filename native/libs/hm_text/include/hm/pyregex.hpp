// Python `re` (str patterns, no flags) on PCRE2 (8-bit, UTF + UCP). Patterns are written exactly as in the
// Python source; \s and \S are rewritten to Python's whitespace set (str.isspace) and \d to \p{Nd}, so the
// Unicode classes match what Python matches. Offsets are byte offsets into the UTF-8 subject.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct pcre2_real_code_8;

namespace hm::py {

struct Match {
  std::size_t start = 0;
  std::size_t end = 0;
  std::vector<std::pair<std::size_t, std::size_t>> groups;  // [0] = whole match; npos when not set
  std::string group(std::string_view subject, std::size_t index) const;
};

class Regex {
 public:
  explicit Regex(std::string_view python_pattern);
  ~Regex();
  Regex(const Regex&) = delete;
  Regex& operator=(const Regex&) = delete;

  // re.search(pattern, subject) starting at byte offset `from`
  std::optional<Match> search(std::string_view subject, std::size_t from = 0) const;
  // [m.start() for m in re.finditer(...)], as byte offsets
  std::vector<std::size_t> finditer_starts(std::string_view subject) const;
  const std::string& pcre2_pattern() const { return translated_; }

 private:
  std::string translated_;
  pcre2_real_code_8* code_ = nullptr;
};

// The rewrite applied to Python patterns (exposed for tests).
std::string translate_python_pattern(std::string_view python_pattern);

}  // namespace hm::py
