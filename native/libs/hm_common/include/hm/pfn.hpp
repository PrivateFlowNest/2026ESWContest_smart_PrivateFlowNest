// pfn.v1 message builders (contracts/pfn.v1). Same documents as services/runtime pfn.py.
// The C++ side does not run JSON Schema; the PC tests validate every emitted message against
// the contract schemas (native/tests), and core validates again on receipt.
#pragma once

#include <optional>
#include <string>

#include "hm/common.hpp"

namespace hm::pfn {

inline constexpr const char* kPerception = "pfn/v1/ai/perception";
inline constexpr const char* kTaskRequest = "pfn/v1/core/task/request";
inline constexpr const char* kTaskUpdate = "pfn/v1/core/task/update";
inline constexpr const char* kLlmRequest = "pfn/v1/ai/llm/request";
inline constexpr const char* kLlmResult = "pfn/v1/ai/llm/result";
inline constexpr const char* kParserRuleVersion = "guardrails-2026-10-22";

struct TaskRequestArgs {
  std::string source_node_id;
  std::string intent;
  std::string target_device;
  json parameters = json::object();
  double confidence = 0.9;
  std::optional<std::string> trace_id;
  std::string safety_level = "normal";
};
json task_request(const TaskRequestArgs& args);

struct LlmRequestArgs {
  std::string text;
  std::string utterance_id;
  std::string stt_model_version;
  std::optional<double> stt_confidence;
  std::string parser_result = "NO_MATCH";
  json context = nullptr;  // activity-context object or null
  int ttl_ms = 10000;
  std::optional<std::string> trace_id;
  bool simulated = false;
  std::string source_node_id = "NODE-RPI5-02";
};
json llm_request(const LlmRequestArgs& args);

struct LlmResultArgs {
  const json* request = nullptr;  // the llm/request being answered (trace_id, request_id)
  std::string status;
  std::optional<std::string> task_id;
  json proposal = nullptr;  // AI proposal object or null
  std::string origin;       // model | guard | guard_fallback | none
  double latency_ms = 0.0;
  std::string model_name;
  bool simulated = false;
  std::string source_node_id = "NODE-RPI5-01";
};
json llm_result(const LlmResultArgs& args);

struct PerceptionArgs {
  std::string observation_type;
  std::string zone;
  double confidence = 0.0;
  json attributes = json::object();
  std::optional<std::string> source_device_id;
  std::optional<std::string> trace_id;
  bool simulated = false;
  std::string source_node_id = "NODE-RPI5-02";
};
json perception(const PerceptionArgs& args);

// Minimal structural check of an incoming llm/request (required fields and types).
// Returns an empty string when acceptable, else the first problem.
std::string check_llm_request(const json& request);

}  // namespace hm::pfn
