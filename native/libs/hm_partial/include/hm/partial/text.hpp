// Text handling of services/edge-ai/partial-intent/src/homemind_partial_intent/features.py, code point for
// code point: normalize_text() = NFKC -> str.strip() -> str.lower() -> re.sub(r"\s+", " ") -> [:512].
// Strings are UTF-32 (one element per Python code point). Invalid UTF-8 bytes decode like Python's
// surrogateescape (U+DC80..U+DCFF), which the runtime treats as malformed text (NO_ACTION).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hm::partial {

using CodePoints = std::u32string;

CodePoints decode_utf8(std::string_view bytes);       // invalid bytes -> lone surrogates
std::string encode_utf8_replace(const CodePoints& text);  // lone surrogates -> '?' (encode errors="replace")
bool has_surrogate(const CodePoints& text);

bool is_python_space(char32_t c);  // str.isspace() / re "\s" for str patterns
CodePoints nfkc(const CodePoints& text);
CodePoints python_lower(const CodePoints& text);  // full lowercase incl. U+0130 and final sigma
CodePoints normalize_text(const CodePoints& text);
CodePoints normalize_text(std::string_view utf8);

// Non-overlapping occurrences (str.count) and containment (`in`).
std::size_t count(const CodePoints& haystack, const CodePoints& needle);
bool contains(const CodePoints& haystack, const CodePoints& needle);

std::uint32_t fnv1a_32(std::string_view bytes);
std::string sha256_hex(std::string_view bytes);

}  // namespace hm::partial
