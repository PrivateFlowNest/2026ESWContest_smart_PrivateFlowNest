// Port of services/exaone-edge/tests/test_client.py (QueueClient -> queued fake transport).
#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <vector>

#include "hm/exaone_client.hpp"
#include "hm/validation.hpp"

namespace {

using hm::py::ojson;

std::string response(const std::string& content) {
  ojson body = ojson::object();
  body["choices"] = ojson::array({ojson::object({{"message", ojson::object({{"content", content}})}})});
  return hm::py::dumps(body);
}

std::string light_json(const char* device = "LIGHT") {
  ojson p = hm::py::loads(R"({"intent": "DEVICE_CONTROL", "device": "LIGHT", "action": "TURN_ON", "parameters": {},
                              "ambiguous": false, "safety_flags": [], "clarification_question": null})");
  p["device"] = device;
  return hm::py::dumps(p);
}

struct Queue {
  std::deque<hm::http::Response> replies;
  std::vector<std::pair<std::string, ojson>> payloads;  // (url, parsed body)

  void reply(const std::string& body, int status = 200) { replies.push_back({status, body, {}, {}}); }
  void offline() { replies.push_back({0, {}, {}, "connect 127.0.0.1:8080: Connection refused"}); }
};

hm::guard::ExaoneClient client(Queue& queue, int retries, bool guardrails = true, bool few_shots = true) {
  hm::guard::ExaoneClientOptions options;
  options.contracts_dir = HM_TEST_CONTRACTS_DIR;
  options.capabilities = hm::guard::load_json_object(HM_TEST_CAPABILITIES);
  options.retries = retries;
  options.enable_guardrails = guardrails;
  options.include_few_shots = few_shots;
  options.transport = [&queue](const std::string&, const std::string& url, const std::string& body, const std::string&,
                               int) {
    queue.payloads.emplace_back(url, hm::py::loads(body));
    hm::http::Response next = queue.replies.front();
    queue.replies.pop_front();
    return next;
  };
  return hm::guard::ExaoneClient(options);
}

TEST(Client, RequestUsesStrictJsonSchemaAndDisablesThinking) {
  Queue queue;
  queue.reply(response(light_json()));
  auto c = client(queue, 0);
  const auto result = c.interpret("에어컨 세기를 높여줘");
  EXPECT_EQ(result.model_output_valid, true);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.origin, "model");
  const ojson& payload = queue.payloads.at(0).second;
  EXPECT_EQ(queue.payloads[0].first, "http://127.0.0.1:8080/v1/chat/completions");
  EXPECT_EQ(payload["response_format"]["type"], "json_schema");
  EXPECT_EQ(payload["response_format"]["json_schema"]["strict"], true);
  EXPECT_EQ(payload["chat_template_kwargs"]["enable_thinking"], false);
  EXPECT_EQ(payload["temperature"], 0.0);
  EXPECT_EQ(payload["messages"].size(), 2u + 2 * 10);
}

TEST(Client, InvalidOutputIsRetriedWithValidationFeedback) {
  Queue queue;
  queue.reply(response(R"({"intent": "DEVICE_CONTROL"})"));
  queue.reply(response(light_json()));
  auto c = client(queue, 1);
  const auto result = c.interpret("에어컨 세기를 높여줘");
  EXPECT_EQ(result.model_output_valid, true);
  EXPECT_EQ(result.attempts, 2);
  const ojson second = hm::py::loads(queue.payloads.at(1).second["messages"].back()["content"].get<std::string>());
  ASSERT_TRUE(second.contains("previous_output_errors"));
  EXPECT_EQ(second["previous_output_errors"][0], "missing field: action");
  EXPECT_EQ(second["instruction"], "오류를 고쳐 JSON 객체만 다시 출력한다.");
}

TEST(Client, MarkdownWrappedJsonIsRejectedAndFailsClosed) {
  Queue queue;
  queue.reply(response("```json\n" + light_json() + "\n```"));
  auto c = client(queue, 0);
  const auto result = c.interpret("에어컨 세기를 높여줘");
  EXPECT_EQ(result.model_output_valid, false);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.origin, "fallback");
  EXPECT_EQ(result.proposal["intent"], "CLARIFY");
  EXPECT_EQ(result.proposal["safety_flags"], ojson::array({"MODEL_OUTPUT_INVALID"}));
  EXPECT_EQ(result.errors, (std::vector<std::string>{"invalid JSON: Expecting value"}));
}

TEST(Client, ServerFailureReturnsNonActionableFallback) {
  Queue queue;
  queue.offline();
  auto c = client(queue, 1);
  const auto result = c.interpret("에어컨 세기를 높여줘");
  EXPECT_EQ(result.model_output_valid, false);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.attempts, 1);
  EXPECT_TRUE(result.proposal["device"].is_null());
  EXPECT_TRUE(result.proposal["action"].is_null());
  EXPECT_EQ(result.proposal["safety_flags"], ojson::array({"MODEL_UNAVAILABLE"}));
}

TEST(Client, HttpErrorIsModelUnavailable) {
  Queue queue;
  queue.reply("{}", 503);
  auto c = client(queue, 1);
  const auto result = c.interpret("에어컨 세기를 높여줘");
  EXPECT_EQ(result.errors, (std::vector<std::string>{"model unavailable: HTTP Error 503: Service Unavailable"}));
  EXPECT_EQ(result.origin, "fallback");
}

TEST(Client, RawModelEvaluationCanDisableGuardrailsAndFewShots) {
  Queue queue;
  queue.reply(response(light_json()));
  auto c = client(queue, 0, false, false);
  const auto result = c.interpret("불 켜줘");
  EXPECT_EQ(result.origin, "model");
  EXPECT_EQ(queue.payloads.at(0).second["messages"].size(), 2u);
}

TEST(Client, ClearCommandCallsModelButDeterministicResultWinsDisagreement) {
  Queue queue;
  queue.reply(response(light_json("TV")));
  auto c = client(queue, 0);
  const auto result = c.interpret("거실 불을 켜 줘");
  EXPECT_EQ(queue.payloads.size(), 1u);
  EXPECT_EQ(result.origin, "model_guard");
  EXPECT_EQ(result.proposal["device"], "LIGHT");
  EXPECT_EQ(result.proposal["action"], "TURN_ON");
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0], "deterministic correction: device");
  const ojson user = hm::py::loads(queue.payloads[0].second["messages"].back()["content"].get<std::string>());
  EXPECT_EQ(user["validated_local_candidate"]["device"], "LIGHT");
}

TEST(Client, ClearCommandUsesDeterministicCandidateWhenModelIsOffline) {
  Queue queue;
  queue.offline();
  auto c = client(queue, 0);
  const auto result = c.interpret("창문 열어 줘");
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.origin, "guard_fallback");
  EXPECT_EQ(result.proposal["device"], "WINDOW");
  EXPECT_EQ(result.proposal["action"], "OPEN");
}

TEST(Client, GuardAnswersWithoutModel) {
  Queue queue;
  auto c = client(queue, 1);
  const auto result = c.interpret("거실 불은 켜지 마.");
  EXPECT_EQ(result.origin, "guard");
  EXPECT_FALSE(result.model_output_valid.has_value());
  EXPECT_TRUE(queue.payloads.empty());
}

TEST(Client, ContentAndTypeErrorsBecomeFeedback) {
  Queue queue;
  queue.reply(R"({"choices": []})");
  queue.reply(response("   "));
  queue.reply(response(R"({"intent": "NO_ACTION", "device": null, "action": null, "parameters": {}, "ambiguous": false,
                          "safety_flags": [["x"]], "clarification_question": null})"));
  auto c = client(queue, 2);
  const auto result = c.interpret("오늘 기분이 좋아");
  EXPECT_EQ(result.errors, (std::vector<std::string>{"llama.cpp response has no assistant content",
                                                     "llama.cpp returned empty assistant content",
                                                     "unhashable type: 'list'"}));
  EXPECT_EQ(result.attempts, 3);
  const ojson last = hm::py::loads(queue.payloads.at(2).second["messages"].back()["content"].get<std::string>());
  EXPECT_EQ(last["previous_output_errors"], ojson::array({"llama.cpp returned empty assistant content"}));
}

TEST(Client, InvalidArguments) {
  Queue queue;
  auto c = client(queue, 0);
  EXPECT_THROW(c.interpret("  \n"), std::invalid_argument);
  EXPECT_THROW(c.interpret("불 켜줘", hm::py::loads(R"({"activity": "DANCING"})")), hm::guard::ProposalValidationError);
}

}  // namespace
