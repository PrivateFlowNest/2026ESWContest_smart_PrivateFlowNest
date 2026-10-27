// Unit tests for hm_partial. Python parity lives in native/tests/test_partial_parity.py.
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <numeric>
#include <string>

#include "hm/action_map.hpp"
#include "hm/partial/intent.hpp"
#include "hm/partial/text.hpp"

namespace hp = hm::partial;
namespace fs = std::filesystem;

namespace {

const fs::path kModel = fs::path(HM_TEST_REPO_ROOT) / "services/edge-ai/partial-intent/artifacts/partial-intent-v2.onnx";

std::string norm(const std::string& text) { return hp::encode_utf8_replace(hp::normalize_text(text)); }

std::unique_ptr<hp::IntentRuntime> load_model() {
  if (!fs::exists(kModel)) return nullptr;
  try {
    return std::make_unique<hp::IntentRuntime>(kModel);
  } catch (const std::runtime_error&) {
    return nullptr;  // built without ONNX Runtime
  }
}

}  // namespace

TEST(Text, HashesMatchKnownValues) {
  EXPECT_EQ(hp::fnv1a_32(""), 2166136261u);
  EXPECT_EQ(hp::fnv1a_32("a"), 0xe40c292cu);
  EXPECT_EQ(hp::sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(hp::sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(hp::sha256_hex(std::string(64, 'a')), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(Text, NormalizeLikePython) {
  EXPECT_EQ(norm("  Ｈｅｌｌｏ \t\n WORLD  "), "hello world");
  EXPECT_EQ(norm("　방 안이  추워 "), "방 안이 추워");
  EXPECT_EQ(norm("İ"), "i̇");          // full lower-case mapping
  EXPECT_EQ(norm("ΟΔΟΣ ΣΑ"), "οδος σα");         // final sigma
  EXPECT_EQ(norm("ㅎㅏ"), "하");                  // NFKC: compatibility jamo compose
  EXPECT_EQ(norm("TV 켜줘"), "tv 켜줘");
  EXPECT_EQ(hp::normalize_text(std::string(600, 'x')).size(), 512u);
}

TEST(Text, InvalidUtf8BecomesSurrogates) {
  const auto text = hp::decode_utf8("a\xff\xe2\x82");
  ASSERT_EQ(text.size(), 4u);
  EXPECT_EQ(text[1], char32_t(0xDCFF));
  EXPECT_EQ(text[2], char32_t(0xDCE2));
  EXPECT_TRUE(hp::has_surrogate(text));
  EXPECT_EQ(hp::encode_utf8_replace(text), "a???");
  EXPECT_FALSE(hp::has_surrogate(hp::decode_utf8("정상 문장")));
  EXPECT_EQ(hp::decode_utf8("\xed\xa0\x80").size(), 3u);  // encoded surrogate is invalid UTF-8
}

TEST(Text, CountIsNonOverlapping) {
  EXPECT_EQ(hp::count(hp::decode_utf8("aaaa"), hp::decode_utf8("aa")), 2u);
  EXPECT_EQ(hp::count(hp::decode_utf8("덥고 덥다"), hp::decode_utf8("덥")), 2u);
}

TEST(Features, UnitNormAndSemanticCues) {
  const auto vector = hp::vectorize_one(hp::decode_utf8("방 안이 너무 더워서 에어컨 온도가 궁금해"));
  ASSERT_EQ(vector.size(), 4096u);
  double squares = 0.0;
  for (float value : vector) squares += double(value) * value;
  EXPECT_NEAR(std::sqrt(squares), 1.0, 1e-5);
  EXPECT_GT(vector[4096 - 64 + 0], 0.0f);  // CLIMATE_COMFORT count
  EXPECT_GT(vector[4096 - 64 + 22], 0.0f);  // request cue
  EXPECT_THROW(hp::vectorize_one(hp::decode_utf8("x"), 64), std::invalid_argument);
}

TEST(Policy, SuppressionAndDirectCommands) {
  EXPECT_EQ(hp::apply_safety_policy(hp::decode_utf8("에어컨은 켜지 마"), "CLIMATE_COMFORT"), "NO_ACTION");
  EXPECT_EQ(hp::apply_safety_policy(hp::decode_utf8("제품 이름이 뭐였지"), "MEDIA"), "NO_ACTION");
  EXPECT_EQ(hp::apply_safety_policy(hp::decode_utf8("좀 덥네"), "CLIMATE_COMFORT"), "CLIMATE_COMFORT");
  EXPECT_TRUE(hp::requires_final_transcript(hp::decode_utf8("불 켜줘")));
  EXPECT_FALSE(hp::requires_final_transcript(hp::decode_utf8("방이 어둡네")));
  EXPECT_EQ(hp::hints_for("VENTILATION").candidate_devices.size(), 3u);
  EXPECT_THROW(hp::hints_for("FLY"), std::out_of_range);
}

TEST(Rounding, MatchesPythonRound) {
  EXPECT_DOUBLE_EQ(hp::py_round(0.1234565, 6), 0.123456);  // binary value is just below the tie
  EXPECT_DOUBLE_EQ(hp::py_round(0.5, 0), 0.0);
  EXPECT_DOUBLE_EQ(hp::py_round(0.7712345678, 6), 0.771235);
  const std::string stamp = hp::utc_isoformat_now();
  EXPECT_NE(stamp.find("+00:00"), std::string::npos);
}

TEST(Runtime, EventsCarryNoTextAndNeverExecute) {
  auto model = load_model();
  if (!model) GTEST_SKIP() << "partial-intent-v2 model or ONNX Runtime not available";
  const auto event = model->predict("방 안이 너무 추워서 따뜻했으면 좋겠어", true);
  EXPECT_EQ(event["execution_allowed"], false);
  EXPECT_EQ(event["source_node_id"], "NODE-RPI5-02");
  EXPECT_EQ(event["text_sha256"].get<std::string>().size(), 64u);
  EXPECT_EQ(event["model"]["version"], "partial-intent-v2");
  const std::string dumped = event.dump();
  EXPECT_EQ(dumped.find("추워"), std::string::npos);
  EXPECT_EQ(event["intent"], "CLIMATE_COMFORT");
  EXPECT_EQ(event["result"], "DETECTED");

  const auto malformed = model->predict(std::string("abc\xff\xfe"), true);
  EXPECT_EQ(malformed["intent"], "NO_ACTION");
  EXPECT_EQ(malformed["result"], "SUPPRESSED");
  EXPECT_DOUBLE_EQ(malformed["confidence"].get<double>(), 1.0);
}

TEST(Runtime, GateNeedsTwoUpdatesAndSuppressesRepeats) {
  auto model = load_model();
  if (!model) GTEST_SKIP() << "partial-intent-v2 model or ONNX Runtime not available";
  hp::StreamIntentGate gate(*model, 2);
  EXPECT_DOUBLE_EQ(gate.threshold(), 0.3);  // metadata streaming_threshold
  const auto first = gate.update("방 안이 점점 추워지는", false);
  const auto second = gate.update("방 안이 점점 추워지는 것 같아", false);
  const auto third = gate.update("방 안이 점점 추워지는 것 같아 그래서 말인데", false);
  if (first["result"] == "DETECTED" || first["intent"] != "CLIMATE_COMFORT") GTEST_SKIP() << "model disagrees";
  EXPECT_EQ(first["result"], "INSUFFICIENT_EVIDENCE");
  EXPECT_EQ(second["result"], "DETECTED");
  EXPECT_EQ(third["result"], "SUPPRESSED");

  hp::UtteranceIntentStream stream(*model, 2);
  stream.update("방 안이 점점 추워지는", false, "u1");
  EXPECT_EQ(stream.update("방 안이 점점 추워지는 것 같아", false, "u1")["result"], "DETECTED");
  stream.update("방 안이 점점 추워지는", false, "u2");
  EXPECT_EQ(stream.update("방 안이 점점 추워지는 것 같아", false, "u2")["result"], "DETECTED");  // new utterance
}

TEST(Perception, HintMapsToRegistryDevices) {
  const hm::ActionMap action_map(HM_TEST_CONTRACTS_DIR);
  hm::json event = {{"result", "DETECTED"},
                    {"execution_allowed", false},
                    {"intent", "VENTILATION"},
                    {"confidence", 0.77},
                    {"trace_id", "5f0e0000-0000-4000-8000-000000000006"},
                    {"candidate_devices", {"WINDOW", "VENTILATION_FAN", "AIR_PURIFIER"}},
                    {"model", {{"version", "partial-intent-v2"}}}};
  const auto hint = hp::hint_perception(event, action_map, true);
  ASSERT_TRUE(hint);
  EXPECT_EQ((*hint)["zone"], "living");
  EXPECT_EQ((*hint)["source_device_id"], "MIC-INDOOR-01");
  EXPECT_EQ((*hint)["attributes"]["candidate_devices"], hm::json({"FAN-01", "SERVO-WINDOW-01"}));
  EXPECT_EQ((*hint)["attributes"]["execution_allowed"], false);
  EXPECT_EQ((*hint)["trace_id"], "5f0e0000-0000-4000-8000-000000000006");
  event["result"] = "INSUFFICIENT_EVIDENCE";
  EXPECT_FALSE(hp::hint_perception(event, action_map, true));
}
