// Port of services/exaone-edge/tests/test_validation.py (+ error text/order details the retry feedback relies on)
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hm/validation.hpp"

namespace {

using hm::guard::ProposalValidationError;
using hm::py::ojson;

const ojson& schema() {
  static const ojson value = hm::guard::load_json_object(hm::guard::default_schema_path(HM_TEST_CONTRACTS_DIR));
  return value;
}
const ojson& context_schema() {
  static const ojson value =
      hm::guard::load_json_object(hm::guard::default_activity_context_schema_path(HM_TEST_CONTRACTS_DIR));
  return value;
}
const ojson& capabilities() {
  static const ojson value = hm::guard::load_json_object(HM_TEST_CAPABILITIES);
  return value;
}

ojson light() {
  return hm::py::loads(R"({"intent": "DEVICE_CONTROL", "device": "LIGHT", "action": "TURN_ON", "parameters": {},
                           "ambiguous": false, "safety_flags": [], "clarification_question": null})");
}

std::vector<std::string> errors_of(const ojson& proposal) {
  try {
    hm::guard::validate_proposal(proposal, schema(), capabilities());
  } catch (const ProposalValidationError& error) {
    return error.errors();
  }
  return {};
}

bool any_contains(const std::vector<std::string>& errors, const std::string& needle) {
  for (const auto& error : errors) {
    if (error.find(needle) != std::string::npos) return true;
  }
  return false;
}

TEST(Validation, ValidDeviceProposalIsAccepted) {
  EXPECT_EQ(hm::guard::validate_proposal(light(), schema(), capabilities())["action"], "TURN_ON");
}

TEST(Validation, UnknownFieldIsRejected) {
  ojson p = light();
  p["executed"] = true;
  EXPECT_TRUE(any_contains(errors_of(p), "unknown field: executed"));
}

TEST(Validation, DisabledDeviceIsRejectedEvenIfSchemaKnowsIt) {
  ojson p = light();
  p["device"] = "ROBOT_VACUUM";
  p["action"] = "START";
  EXPECT_TRUE(any_contains(errors_of(p), "device is unavailable"));
}

TEST(Validation, DeviceActionPairIsEnforced) {
  ojson p = light();
  p["action"] = "OPEN";
  EXPECT_TRUE(any_contains(errors_of(p), "not allowed for LIGHT"));
}

TEST(Validation, ClarifyRequiresNullTargetAndQuestion) {
  ojson p = light();
  p["intent"] = "CLARIFY";
  p["ambiguous"] = true;
  p["clarification_question"] = "어느 기기를 말씀하셨나요?";
  EXPECT_TRUE(any_contains(errors_of(p), "must not contain device"));
}

TEST(Validation, NumericParametersAreBoundedAndRequired) {
  ojson p = light();
  p["action"] = "SET_BRIGHTNESS";
  p["parameters"] = hm::py::loads(R"({"brightness": 101})");
  EXPECT_TRUE(any_contains(errors_of(p), "above maximum"));
  p["parameters"] = ojson::object();
  EXPECT_TRUE(any_contains(errors_of(p), "requires parameters.brightness"));
}

TEST(Validation, NegatedRequestCannotBecomeControl) {
  ojson p = light();
  p["safety_flags"] = ojson::array({"NEGATED_REQUEST"});
  EXPECT_TRUE(any_contains(errors_of(p), "must use intent=NO_ACTION"));
}

TEST(Validation, UnsupportedActionFlagCannotMarkValidControlAsExecutable) {
  ojson p = light();
  p["safety_flags"] = ojson::array({"UNSUPPORTED_ACTION"});
  EXPECT_TRUE(any_contains(errors_of(p), "must use intent=CLARIFY"));
}

TEST(Validation, SafeFallbackItselfPassesValidation) {
  const ojson fallback = hm::guard::safe_fallback(true);
  EXPECT_EQ(hm::guard::validate_proposal(fallback, schema(), capabilities()), fallback);
}

TEST(Validation, EmptyActivityContextIsNormalized) {
  const ojson context = hm::guard::validate_activity_context(ojson::object(), context_schema());
  EXPECT_EQ(context["presence_zone"], "UNKNOWN");
  EXPECT_EQ(context["quiet_required"], false);
}

TEST(Validation, RawMediaCannotEnterActivityContext) {
  try {
    hm::guard::validate_activity_context(hm::py::loads(R"({"image": "raw-frame"})"), context_schema());
    FAIL();
  } catch (const ProposalValidationError& error) {
    EXPECT_TRUE(any_contains(error.errors(), "unknown field: image"));
  }
}

TEST(Validation, ActivityContextBoundsConfidence) {
  try {
    hm::guard::validate_activity_context(hm::py::loads(R"({"confidence": 1.5})"), context_schema());
    FAIL();
  } catch (const ProposalValidationError& error) {
    EXPECT_TRUE(any_contains(error.errors(), "above maximum"));
  }
}

TEST(Validation, QuietModeCanBeResumedAfterContextEnds) {
  ojson p = light();
  p["device"] = "ALL_QUIET_DEVICES";
  p["action"] = "RESUME";
  p["parameters"] = hm::py::loads(R"({"reason": "회의 상황 종료"})");
  EXPECT_EQ(hm::guard::validate_proposal(p, schema(), capabilities())["action"], "RESUME");
}

TEST(Validation, ErrorTextAndOrderFollowPython) {
  // key order of the model output decides the order of value errors; missing/unknown come first, sorted
  const ojson p = hm::py::loads(
      R"({"zeta": 1, "intent": 5, "device": "OVEN", "parameters": {"volume": 1.5, "x": 1}, "safety_flags": ["HIGH_RISK", true], "alpha": 2})");
  EXPECT_EQ(errors_of(p), (std::vector<std::string>{
                              "missing field: action", "missing field: ambiguous", "missing field: clarification_question",
                              "unknown field: alpha", "unknown field: zeta", "intent: unsupported value 5",
                              "device: unsupported value 'OVEN'", "parameters: unknown field x",
                              "parameters.volume: invalid type", "safety_flags[1]: unsupported value True"}));
  EXPECT_EQ(errors_of(hm::py::loads("[1]")), (std::vector<std::string>{"proposal: expected object"}));
  ojson nested = light();
  nested["safety_flags"] = hm::py::loads(R"([["HIGH_RISK"]])");
  EXPECT_THROW(hm::guard::validate_proposal(nested, schema(), capabilities()), hm::guard::PyTypeError);
  ojson duplicate = light();
  duplicate["safety_flags"] = hm::py::loads(R"([1, true])");
  EXPECT_EQ(errors_of(duplicate), (std::vector<std::string>{"safety_flags: duplicate values",
                                                            "safety_flags[0]: unsupported value 1",
                                                            "safety_flags[1]: unsupported value True"}));
}

}  // namespace
