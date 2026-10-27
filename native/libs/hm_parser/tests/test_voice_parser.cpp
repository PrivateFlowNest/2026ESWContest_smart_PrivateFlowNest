// Port of the voice parser tests in services/runtime/tests/test_perception.py (+ scenario/guard ordering).
#include <gtest/gtest.h>

#include "hm/pfn.hpp"
#include "hm/pfn_schema.hpp"
#include "hm/voice_parser.hpp"

namespace {

hm::VoiceParser& parser() {
  static hm::ActionMap map(HM_TEST_CONTRACTS_DIR);
  static hm::VoiceParser instance(map, {.capabilities_path = HM_TEST_CAPABILITIES,
                                        .simulated = true,
                                        .contracts_dir = HM_TEST_CONTRACTS_DIR});
  return instance;
}

TEST(VoiceParser, LightOn) {
  const auto decision = parser().handle("불 켜줘", nullptr, "whisper-test");
  ASSERT_EQ(decision.kind, "task_request");
  const auto& [topic, request] = decision.messages.at(0);
  EXPECT_EQ(topic, "pfn/v1/core/task/request");
  EXPECT_EQ(request["intent"], "device_control");
  EXPECT_EQ(request["target_device"], "LED-LIVING-01");
  EXPECT_EQ(request["parameters"]["action"], "set_switch");
  EXPECT_EQ(request["parameters"]["parameters"], (hm::json{{"on", true}}));
  EXPECT_EQ(request["parameters"]["origin"], "voice_parser");
  EXPECT_EQ(hm::guard::validate_task_request(request), "");
}

TEST(VoiceParser, OpenDoorIsCaution) {
  const auto decision = parser().handle("현관문 열어줘", nullptr, "whisper-test");
  ASSERT_EQ(decision.kind, "task_request");
  const auto& request = decision.messages.at(0).second;
  EXPECT_EQ(request["parameters"]["action"], "set_position");
  EXPECT_EQ(request["parameters"]["parameters"]["position_degrees"], 90);
  EXPECT_EQ(request["safety_level"], "caution");
}

TEST(VoiceParser, VagueGoesToLlmWithContext) {
  EXPECT_EQ(parser().handle("아 좀 덥네", nullptr, "whisper-test").kind, "llm_request");
  const hm::json context = {{"presence_zone", "UNKNOWN"}, {"activity", "UNKNOWN"}, {"audio_context", "SELF_TALK"},
                            {"quiet_required", false}, {"confidence", 0.8}};
  const auto decision = parser().handle("아 좀 덥네", context, "whisper-test");
  ASSERT_EQ(decision.kind, "llm_request");
  const auto& [topic, request] = decision.messages.at(0);
  EXPECT_EQ(topic, "pfn/v1/ai/llm/request");
  EXPECT_EQ(request["context"], context);
  EXPECT_TRUE(request["transcript"]["stt_confidence"].is_null());
  EXPECT_EQ(request["simulated"], true);
  EXPECT_EQ(hm::guard::validate_llm_request(request), "");
}

TEST(VoiceParser, DuringMeeting) {
  const hm::json context = {{"presence_zone", "UNKNOWN"}, {"activity", "MEETING"}, {"audio_context", "ZOOM_SPEAKER"},
                            {"quiet_required", true}, {"confidence", 0.9}};
  EXPECT_EQ(parser().handle("아 좀 덥네", context, "whisper-test").kind, "llm_request");
  const auto quiet = parser().handle("조용히 해줘", context, "whisper-test");
  ASSERT_EQ(quiet.kind, "task_request");
  EXPECT_EQ(quiet.messages.at(0).second["intent"], "quiet_mode_on");
  EXPECT_EQ(parser().handle("불 켜줘", context).messages.at(0).second["target_device"], "LED-LIVING-01");
}

TEST(VoiceParser, ScenariosGuardAndNotes) {
  const auto home = parser().handle("나 왔어");
  ASSERT_EQ(home.kind, "task_request");
  EXPECT_EQ(home.note, "prepare_home");
  EXPECT_EQ(home.messages.at(0).second["intent"], "prepare_home");
  EXPECT_TRUE(home.proposal.is_null());
  // the guard's negation stops the scenario keywords
  const auto negated = parser().handle("회의 끝났으니 불 켜지 마");
  EXPECT_EQ(negated.kind, "no_action");
  EXPECT_EQ(negated.note, "NEGATED_REQUEST");
  const auto clarify = parser().handle("그거 켜줘");
  EXPECT_EQ(clarify.kind, "clarify");
  EXPECT_EQ(clarify.note, "어느 기기를 말씀하셨나요?");
  EXPECT_EQ(parser().handle("   ").note, "empty transcript");
  EXPECT_EQ(parser().handle("세탁기 켜줘").kind, "invalid");  // WASHER/TURN_ON is not in the capabilities
  EXPECT_EQ(parser().handle("불 상태 알려줘").kind, "query");
}

TEST(VoiceParser, ConfidenceIsRoundedLikePython) {
  const auto decision = parser().handle("불 켜줘", nullptr, "whisper-test", 0.12345);
  EXPECT_DOUBLE_EQ(decision.messages.at(0).second["confidence"].get<double>(), 0.1235);  // round(0.12345, 4)
  EXPECT_THROW(parser().handle("아 좀 덥네", nullptr, "whisper-test", 1.5), std::invalid_argument);
  EXPECT_THROW(parser().handle("불 켜줘", nullptr, "keyboard", std::nullopt, std::nullopt, "not-a-uuid"),
               std::invalid_argument);
}

}  // namespace
