// Unicode helpers and stt.py clean_transcript (Python str semantics on UTF-8 text).
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "hm/stt.hpp"

namespace hm::stt {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

std::vector<char32_t> decode(const std::string& text) {
  std::vector<char32_t> out;
  out.reserve(text.size());
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t n = text.size();
  std::size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    int length = 0;
    char32_t cp = 0;
    if (c < 0x80) {
      length = 1;
      cp = c;
    } else if (c >= 0xC2 && c <= 0xDF) {
      length = 2;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      length = 3;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      length = 4;
      cp = c & 0x07;
    }
    bool ok = length > 0 && i + static_cast<std::size_t>(length) <= n;
    for (int k = 1; ok && k < length; ++k) {
      const unsigned char next = p[i + static_cast<std::size_t>(k)];
      if ((next & 0xC0) != 0x80) ok = false;
      cp = (cp << 6) | (next & 0x3F);
    }
    if (ok && ((length == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (length == 4 && (cp < 0x10000 || cp > 0x10FFFF)))) {
      ok = false;
    }
    if (!ok) {
      out.push_back(kReplacement);
      ++i;
      continue;
    }
    out.push_back(cp);
    i += static_cast<std::size_t>(length);
  }
  return out;
}

std::string encode(const std::vector<char32_t>& cps, std::size_t begin = 0, std::size_t end = std::string::npos) {
  std::string out;
  end = std::min(end, cps.size());
  for (std::size_t i = begin; i < end; ++i) {
    const char32_t cp = cps[i];
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

// Python str.isspace() (also what `re` uses for \s on str patterns).
bool is_space(char32_t c) {
  return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// str.splitlines() boundaries.
bool is_line_break(char32_t c) {
  return c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x85 ||
         c == 0x2028 || c == 0x2029;
}

template <typename Pred>
std::vector<char32_t> strip_if(const std::vector<char32_t>& value, Pred pred, bool left = true, bool right = true) {
  std::size_t begin = 0;
  std::size_t end = value.size();
  if (left) {
    while (begin < end && pred(value[begin])) ++begin;
  }
  if (right) {
    while (end > begin && pred(value[end - 1])) --end;
  }
  return {value.begin() + static_cast<std::ptrdiff_t>(begin), value.begin() + static_cast<std::ptrdiff_t>(end)};
}

bool is_opener(char32_t c) { return c == U'[' || c == U'(' || c == 0xFF08 || c == 0x3010; }
bool is_closer(char32_t c) { return c == U']' || c == U')' || c == 0xFF09 || c == 0x3011; }

const std::array<const char*, 8> kHallucinations = {
    "감사합니다", "시청해주셔서 감사합니다", "시청해 주셔서 감사합니다", "구독과 좋아요 부탁드립니다",
    "다음 영상에서 만나요", "MBC 뉴스 이덕영입니다", "끝", "자막 제공 및 자막 편집",
};

}  // namespace

std::string sanitize_utf8(const std::string& text) { return encode(decode(text)); }

std::string strip(const std::string& text) { return encode(strip_if(decode(text), is_space)); }

std::string clean_transcript(const std::string& text) {
  const std::vector<char32_t> cps = decode(text);
  // " ".join(line.strip().lstrip("-").strip() for line in text.splitlines() if line.strip())
  std::vector<char32_t> value;
  std::size_t start = 0;
  bool first = true;
  for (std::size_t i = 0; i <= cps.size(); ++i) {
    const bool boundary = i == cps.size() || is_line_break(cps[i]);
    if (!boundary) continue;
    const std::vector<char32_t> line(cps.begin() + static_cast<std::ptrdiff_t>(start),
                                     cps.begin() + static_cast<std::ptrdiff_t>(i));
    if (i < cps.size() && cps[i] == 0x0D && i + 1 < cps.size() && cps[i + 1] == 0x0A) ++i;  // \r\n
    start = i + 1;
    auto stripped = strip_if(line, is_space);
    if (stripped.empty()) continue;
    stripped = strip_if(strip_if(stripped, [](char32_t c) { return c == U'-'; }, true, false), is_space);
    if (!first) value.push_back(U' ');
    value.insert(value.end(), stripped.begin(), stripped.end());
    first = false;
  }
  // _BRACKETED.sub(" ", value): an opener up to the first closer after it (any pairing) -> one space.
  std::vector<char32_t> unbracketed;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (is_opener(value[i])) {
      std::size_t j = i + 1;
      while (j < value.size() && !is_closer(value[j])) ++j;
      if (j < value.size()) {
        unbracketed.push_back(U' ');
        i = j;
        continue;
      }
    }
    unbracketed.push_back(value[i]);
  }
  // re.sub(r"\s+", " ", value).strip()
  std::vector<char32_t> collapsed;
  bool in_run = false;
  for (char32_t c : unbracketed) {
    if (is_space(c)) {
      if (!in_run) collapsed.push_back(U' ');
      in_run = true;
      continue;
    }
    collapsed.push_back(c);
    in_run = false;
  }
  collapsed = strip_if(collapsed, is_space);
  const auto bare = strip_if(collapsed, [](char32_t c) {
    return c == U' ' || c == U'.' || c == U',' || c == U'!' || c == U'?' || c == U'~' || c == 0x2026;
  });
  if (bare.empty()) return "";
  const std::string bare_text = encode(bare);
  if (std::find(kHallucinations.begin(), kHallucinations.end(), bare_text) != kHallucinations.end()) return "";
  const bool meaningful = std::any_of(bare.begin(), bare.end(), [](char32_t c) {
    return (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') || (c >= 0xAC00 && c <= 0xD7A3);
  });
  return meaningful ? encode(collapsed) : "";
}

}  // namespace hm::stt
