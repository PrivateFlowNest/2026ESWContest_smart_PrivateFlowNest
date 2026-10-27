#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "hm/pyjson.hpp"
#include "hm/pyregex.hpp"
#include "hm/text.hpp"

namespace {

using hm::py::ojson;

std::string decode_error(const std::string& text) {
  try {
    hm::py::loads(text);
  } catch (const hm::py::JsonDecodeError& error) {
    return error.msg();
  }
  return "<ok>";
}

TEST(Text, CasefoldAndStrip) {
  EXPECT_EQ(hm::text::casefold("TV 켜줘"), "tv 켜줘");
  EXPECT_EQ(hm::text::casefold("Straße"), "strasse");
  EXPECT_EQ(hm::text::casefold("\xC4\xB0"), "i\xCC\x87");  // U+0130 -> i + U+0307 (full folding)
  EXPECT_EQ(hm::text::casefold("ΣΑΣ"), "σασ");
  EXPECT_EQ(hm::text::strip("\xE3\x80\x80 불 켜줘 \n"), "불 켜줘");  // U+3000 is whitespace
  EXPECT_EQ(hm::text::strip("\x1c a \x1f"), "a");
  EXPECT_EQ(hm::text::strip("\xE2\x80\x8B" "a"), "\xE2\x80\x8B" "a");  // U+200B is not
  EXPECT_EQ(hm::text::length("한글 ok"), 5u);
  EXPECT_EQ(hm::text::prefix("한글입니다", 2), "한글");
}

TEST(Text, Repr) {
  EXPECT_EQ(hm::text::repr("FOO"), "'FOO'");
  EXPECT_EQ(hm::text::repr("a'b"), "\"a'b\"");
  EXPECT_EQ(hm::text::repr("a'\"b"), "'a\\'\"b'");
  EXPECT_EQ(hm::text::repr("\n\t\x7f\\"), "'\\n\\t\\x7f\\\\'");
  EXPECT_EQ(hm::text::repr("\xE2\x80\x8B"), "'\\u200b'");
  EXPECT_EQ(hm::text::repr("\xC2\xA0"), "'\\xa0'");
  EXPECT_EQ(hm::text::repr("조명"), "'조명'");
}

TEST(Text, FloatRepr) {
  EXPECT_EQ(hm::text::float_repr(0.0), "0.0");
  EXPECT_EQ(hm::text::float_repr(-0.0), "-0.0");
  EXPECT_EQ(hm::text::float_repr(0.93), "0.93");
  EXPECT_EQ(hm::text::float_repr(1e16), "1e+16");
  EXPECT_EQ(hm::text::float_repr(1234567890123456.0), "1234567890123456.0");
  EXPECT_EQ(hm::text::float_repr(0.0001), "0.0001");
  EXPECT_EQ(hm::text::float_repr(0.00001), "1e-05");
  EXPECT_EQ(hm::text::float_repr(1.5e300), "1.5e+300");
  EXPECT_EQ(hm::text::float_repr(100.0), "100.0");
  EXPECT_EQ(hm::text::float_repr(std::numeric_limits<double>::infinity()), "inf");
}

TEST(Text, DecimalDigits) {
  EXPECT_EQ(hm::text::decimal_value(U'7'), 7);
  EXPECT_EQ(hm::text::decimal_value(0xFF12), 2);   // fullwidth 2
  EXPECT_EQ(hm::text::decimal_value(0x0663), 3);   // arabic-indic 3
  EXPECT_EQ(hm::text::decimal_value(0x1D7D8), 0);  // double-struck 0 (5 runs of ten in a row)
  EXPECT_EQ(hm::text::decimal_value(U'a'), -1);
  EXPECT_EQ(hm::text::decimal_value(0x2460), -1);  // circled 1 is No, not Nd
}

TEST(Text, Utf8DecodeErrors) {
  EXPECT_FALSE(hm::text::utf8_decode_error("한글").has_value());
  EXPECT_EQ(*hm::text::utf8_decode_error("ab\xff"), "'utf-8' codec can't decode byte 0xff in position 2: invalid start byte");
  EXPECT_EQ(*hm::text::utf8_decode_error("\xed\x95"), "'utf-8' codec can't decode bytes in position 0-1: unexpected end of data");
  EXPECT_EQ(*hm::text::utf8_decode_error("\xed\xa0\x80"), "'utf-8' codec can't decode byte 0xed in position 0: invalid continuation byte");
  EXPECT_EQ(*hm::text::utf8_decode_error("\xe1\x80" "a"), "'utf-8' codec can't decode bytes in position 0-1: invalid continuation byte");
}

TEST(PyJson, LoadsValuesAndOrder) {
  const ojson value = hm::py::loads(R"({"b": 1, "a": [1.5, -0, 1e5, NaN, -Infinity, "x\u00e9\ud83d\ude00"], "b": 2})");
  ASSERT_TRUE(value.is_object());
  EXPECT_EQ(value.begin().key(), "b");  // first position, last value
  EXPECT_EQ(value["b"], 2);
  EXPECT_TRUE(value["a"][1].is_number_integer());
  EXPECT_TRUE(value["a"][2].is_number_float());
  EXPECT_TRUE(std::isnan(value["a"][3].get<double>()));
  EXPECT_EQ(value["a"][5], "x\xC3\xA9\xF0\x9F\x98\x80");
  EXPECT_EQ(hm::py::dumps(value), R"({"b": 2, "a": [1.5, 0, 100000.0, NaN, -Infinity, "xé😀"]})");
  EXPECT_EQ(hm::py::dumps(hm::py::loads(R"(["\"\\\/\b\f\n\r\t\u0001\u007f"])")), R"(["\"\\/\b\f\n\r\t\u0001)" "\x7f\"]");
  EXPECT_EQ(hm::py::dumps(value, true), R"({"b":2,"a":[1.5,0,100000.0,NaN,-Infinity,"xé😀"]})");
}

TEST(PyJson, DecodeErrorMessagesMatchCPython312) {
  EXPECT_EQ(decode_error(""), "Expecting value");
  EXPECT_EQ(decode_error("```json\n{}\n```"), "Expecting value");
  EXPECT_EQ(decode_error("{"), "Expecting property name enclosed in double quotes");
  EXPECT_EQ(decode_error("{\"a\":1,}"), "Expecting property name enclosed in double quotes");
  EXPECT_EQ(decode_error("[1,]"), "Expecting value");
  EXPECT_EQ(decode_error("[1 2]"), "Expecting ',' delimiter");
  EXPECT_EQ(decode_error("{\"a\" 1}"), "Expecting ':' delimiter");
  EXPECT_EQ(decode_error("\"abc"), "Unterminated string starting at");
  EXPECT_EQ(decode_error("\"\\x\""), "Invalid \\escape");
  EXPECT_EQ(decode_error("\"\\u12\""), "Invalid \\uXXXX escape");
  EXPECT_EQ(decode_error("\"a\nb\""), "Invalid control character at");
  EXPECT_EQ(decode_error("{} {}"), "Extra data");
  EXPECT_EQ(decode_error("\xEF\xBB\xBF{}"), "Unexpected UTF-8 BOM (decode using utf-8-sig)");
  EXPECT_EQ(decode_error("-"), "Expecting value");
  EXPECT_EQ(decode_error("01"), "Extra data");
  EXPECT_EQ(decode_error("1e"), "Extra data");
  try {
    hm::py::loads("[1, x]");
    FAIL();
  } catch (const hm::py::JsonDecodeError& error) {
    EXPECT_STREQ(error.what(), "Expecting value: line 1 column 5 (char 4)");
  }
}

TEST(PyJson, ReprEqualTruthy) {
  EXPECT_EQ(hm::py::repr(hm::py::loads(R"({"a": [null, true, 1, 2.0, "x"]})")), "{'a': [None, True, 1, 2.0, 'x']}");
  EXPECT_TRUE(hm::py::equal(ojson(1), ojson(true)));
  EXPECT_TRUE(hm::py::equal(ojson(20), ojson(20.0)));
  EXPECT_FALSE(hm::py::equal(ojson(9007199254740993LL), ojson(9007199254740992.0)));
  EXPECT_TRUE(hm::py::equal(hm::py::loads(R"({"a":1,"b":2})"), hm::py::loads(R"({"b":2.0,"a":1})")));
  EXPECT_FALSE(hm::py::equal(ojson("1"), ojson(1)));
  EXPECT_FALSE(hm::py::truthy(ojson::object()));
  EXPECT_TRUE(hm::py::truthy(ojson("x")));
  EXPECT_FALSE(hm::py::truthy(ojson(0.0)));
  EXPECT_TRUE(hm::py::less_than(ojson(-1), 0.0));
  EXPECT_TRUE(hm::py::greater_than(ojson(101), 100.0));
  EXPECT_FALSE(hm::py::greater_than(ojson(100), 100.0));
}

TEST(PyRegex, PythonClasses) {
  hm::py::Regex volume(R"((?:볼륨|소리)[^0-9]{0,12}(\d{1,3}))");
  const std::string subject = "tv 소리 ２０";
  const auto match = volume.search(subject);
  ASSERT_TRUE(match);
  EXPECT_EQ(match->group(subject, 1), "０");  // greedy [^0-9] also eats the fullwidth 2, like Python
  hm::py::Regex space(R"(a\sb)");
  EXPECT_TRUE(space.search("a\xE3\x80\x80" "b"));
  EXPECT_TRUE(space.search("a\x1c" "b"));
  EXPECT_FALSE(space.search("a\xE2\x80\x8B" "b"));
  EXPECT_FALSE(space.search("a\xE1\xA0\x8E" "b"));  // U+180E: PCRE2 \h but not Python whitespace
  hm::py::Regex off("(?:꺼|끄)(?![져진졌])");
  EXPECT_EQ(off.finditer_starts("꺼진 거 꺼줘 끄고").size(), 2u);
  hm::py::Regex end(R"(있어\s*\??\s*$)");
  EXPECT_TRUE(end.search("불 켜져 있어?"));
  EXPECT_TRUE(end.search("있어\n"));  // $ before a final newline, like Python
}

}  // namespace
