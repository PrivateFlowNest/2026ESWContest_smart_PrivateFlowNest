// LlmNode (port of services/runtime llm_node.py) with a scripted llama-server transport.
#include <gtest/gtest.h>

#include <deque>

#include "hm/pfn.hpp"
#include "hm/pfn_schema.hpp"
#include "hm/validation.hpp"
#include "llm_node.hpp"

namespace {

using hm::json;

std::string reply(const std::string& content) {
  return hm::dump_compact({{"choices", json::array({{{"message", {{"content", content}}}}})}});
}

std::string proposal_json(const std::string& intent, json device, json action, json parameters = json::object()) {
  return hm::dump_compact({{"intent", intent}, {"device", device}, {"action", action}, {"parameters", parameters},
                           {"ambiguous", false}, {"safety_flags", json::array()}, {"clarification_question", nullptr}});
}

struct Fixture {
  std::deque<hm::http::Response> replies;
  int calls = 0;
  hm::ActionMap map{HM_TEST_CONTRACTS_DIR};
  hm::guard::ExaoneClient client{[this] {
    hm::guard::ExaoneClientOptions options;
    options.model = "exaone-1.2b-homemind-v1";
    options.contracts_dir = HM_TEST_CONTRACTS_DIR;
    options.capabilities = hm::guard::load_json_object(HM_TEST_CAPABILITIES);
    options.transport = [this](const std::string&, const std::string&, const std::string&, const std::string&, int) {
      ++calls;
      if (replies.empty()) return hm::http::Response{0, {}, {}, "connect refused"};
      auto next = replies.front();
      replies.pop_front();
      return next;
    };
    return options;
  }()};
  hm::LlmNode node{client, map, {.simulated = true}};

  json request(const std::string& text, json context = nullptr, int ttl_ms = 10000) {
    return hm::pfn::llm_request({.text = text, .utterance_id = hm::new_id(), .stt_model_version = "keyboard",
                                 .context = context, .ttl_ms = ttl_ms, .simulated = true});
  }
};

TEST(LlmNode, ExpiredRequestGivesExpiredResult) {
  Fixture f;
  const auto messages = f.node.handle(f.request("아 좀 덥네", nullptr, -1));
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].first, hm::pfn::kLlmResult);
  EXPECT_EQ(messages[0].second["status"], "expired");
  EXPECT_EQ(messages[0].second["model"]["origin"], "none");
  EXPECT_EQ(messages[0].second["model"]["name"], "exaone-1.2b-homemind-v1");
  EXPECT_EQ(f.calls, 0);
}

TEST(LlmNode, ModelProposalBecomesTaskRequestAndResult) {
  Fixture f;
  f.replies.push_back({200, reply(proposal_json("DEVICE_CONTROL", "AIR_CONDITIONER", "TURN_ON")), {}, {}});
  const json request = f.request("아 좀 덥네");
  const auto messages = f.node.handle(request);
  ASSERT_EQ(messages.size(), 2u);
  const auto& [task_topic, task] = messages[0];
  EXPECT_EQ(task_topic, hm::pfn::kTaskRequest);
  EXPECT_EQ(task["intent"], "device_control");
  EXPECT_EQ(task["source_node_id"], "NODE-RPI5-01");
  EXPECT_EQ(task["confidence"], 0.7);
  EXPECT_EQ(task["trace_id"], request["trace_id"]);
  EXPECT_EQ(task["parameters"]["origin"], "llm");
  EXPECT_EQ(hm::guard::validate_task_request(task), "");
  const auto& result = messages[1].second;
  EXPECT_EQ(result["status"], "task_requested");
  EXPECT_EQ(result["task_id"], task["task_id"]);
  EXPECT_EQ(result["model"]["origin"], "model");
  EXPECT_EQ(result["simulated"], true);
  EXPECT_FALSE(result.dump().find("덥네") != std::string::npos);  // never carries the transcript
}

TEST(LlmNode, StatusesFollowPython) {
  Fixture f;
  EXPECT_EQ(f.node.handle(f.request("거실 불은 켜지 마."))[0].second["status"], "no_action");  // guard, no model call
  EXPECT_EQ(f.calls, 0);
  auto unavailable = f.node.handle(f.request("아 좀 덥네"));
  EXPECT_EQ(unavailable[0].second["status"], "model_unavailable");
  EXPECT_EQ(unavailable[0].second["model"]["origin"], "guard_fallback");  // "fallback" is reported as guard_fallback
  f.replies.push_back({200, reply("garbage"), {}, {}});
  f.replies.push_back({200, reply("```json```"), {}, {}});
  EXPECT_EQ(f.node.handle(f.request("아 좀 덥네"))[0].second["status"], "invalid_output");
  // clear command, model offline: deterministic candidate still becomes the task
  auto offline = f.node.handle(f.request("창문 열어 줘"));
  ASSERT_EQ(offline.size(), 2u);
  EXPECT_EQ(offline[0].second["target_device"], "SERVO-WINDOW-01");
  EXPECT_EQ(offline[1].second["model"]["origin"], "guard_fallback");
  f.replies.push_back({200, reply(proposal_json("DEVICE_CONTROL", "LIGHT", "SET_BRIGHTNESS", {{"brightness", 40}})), {}, {}});
  EXPECT_EQ(f.node.handle(f.request("좀 어둡네"))[0].second["status"], "rejected_guard");
  f.replies.push_back({200, reply(proposal_json("DEVICE_QUERY", "LIGHT", "QUERY_STATUS")), {}, {}});
  EXPECT_EQ(f.node.handle(f.request("불 켜져 있어?"))[0].second["status"], "no_action");
  f.replies.push_back({200, reply(R"({"intent": "CLARIFY", "device": null, "action": null, "parameters": {},
    "ambiguous": true, "safety_flags": ["AMBIGUOUS_TARGET"], "clarification_question": "어느 기기요?"})"), {}, {}});
  auto clarify = f.node.handle(f.request("음 그냥 좀"));
  EXPECT_EQ(clarify[0].second["status"], "clarify");
  EXPECT_EQ(clarify[0].second["clarification_question"], "어느 기기요?");
}

TEST(LlmNode, InvalidRequestsThrowLikePython) {
  Fixture f;
  json request = f.request("아 좀 덥네");
  request["extra"] = 1;
  EXPECT_THROW(f.node.handle(request), std::invalid_argument);
  json blank = f.request("   ");
  EXPECT_THROW(f.node.handle(blank), std::invalid_argument);  // "utterance must be a non-empty string"
  json context = f.request("아 좀 덥네", {{"presence_zone", "DESK"}, {"activity", "MEETING"}, {"audio_context", "QUIET"},
                                           {"quiet_required", true}, {"confidence", 2}});
  EXPECT_THROW(f.node.handle(context), std::invalid_argument);
  EXPECT_EQ(f.calls, 0);
}

}  // namespace
