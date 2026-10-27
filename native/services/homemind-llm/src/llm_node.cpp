#include "llm_node.hpp"

#include <cstdio>
#include <set>
#include <stdexcept>

#include "hm/pfn.hpp"
#include "hm/pfn_schema.hpp"
#include "hm/pyjson.hpp"

namespace hm {

namespace {

json checked(json document, std::string (*validator)(const json&)) {
  if (const std::string problem = validator(document); !problem.empty()) throw std::invalid_argument(problem);
  return document;
}

std::string text_or_none(const json& proposal, const char* key) {
  const json value = proposal.value(key, json(nullptr));
  return value.is_string() ? value.get<std::string>() : std::string("None");
}

}  // namespace

LlmNode::LlmNode(guard::ExaoneClient& client, const ActionMap& action_map, LlmNodeOptions options)
    : client_(client), action_map_(action_map), options_(std::move(options)) {}

std::vector<std::pair<std::string, json>> LlmNode::handle(const json& request) {
  if (const std::string problem = guard::validate_llm_request(request); !problem.empty()) {
    throw std::invalid_argument(problem);  // never includes the transcript
  }
  if (request["expires_at_ms"].get<double>() <= static_cast<double>(now_ms())) {
    json result = pfn::llm_result({.request = &request,
                                   .status = "expired",
                                   .proposal = nullptr,
                                   .origin = "none",
                                   .latency_ms = 0.0,
                                   .model_name = client_.model(),
                                   .simulated = options_.simulated});
    return {{pfn::kLlmResult, checked(std::move(result), guard::validate_llm_result)}};
  }

  const json context = request.contains("context") ? request["context"] : json(nullptr);
  const guard::InterpretationResult interpretation =
      client_.interpret(request["transcript"]["text"].get<std::string>(), py::to_ordered(context));
  const json proposal = py::to_plain(interpretation.proposal);

  std::set<std::string> flags;
  if (proposal.contains("safety_flags")) {
    for (const auto& flag : proposal["safety_flags"]) {
      if (flag.is_string()) flags.insert(flag.get<std::string>());
    }
  }
  std::string status;
  if (flags.count("MODEL_UNAVAILABLE")) {
    status = "model_unavailable";
  } else if (flags.count("MODEL_OUTPUT_INVALID")) {
    status = "invalid_output";
  }
  std::vector<std::pair<std::string, json>> messages;
  std::optional<std::string> task_id;
  if (status.empty()) {
    const std::string intent = proposal.value("intent", std::string());
    if (intent == "CLARIFY") {
      status = "clarify";
    } else if (intent == "NO_ACTION") {
      status = "no_action";
    } else {
      const Resolution resolution = action_map_.resolve(proposal);
      if (resolution.kind == "device_control" || resolution.kind == "scenario") {
        pfn::TaskRequestArgs args{.source_node_id = options_.node_id,
                                  .confidence = py::round_digits(0.7, 4),
                                  .trace_id = request["trace_id"].get<std::string>()};
        if (resolution.kind == "device_control") {
          args.intent = "device_control";
          args.target_device = resolution.device_id.value_or("");
          args.parameters = control_parameters(resolution, "llm", proposal);
        } else {
          args.intent = resolution.intent.value_or("");
          args.target_device = "HOME-ENV-01";
          args.parameters = scenario_parameters("llm", proposal);
        }
        json task = checked(pfn::task_request(args), guard::validate_task_request);
        task_id = task["task_id"].get<std::string>();
        messages.emplace_back(pfn::kTaskRequest, std::move(task));
        status = "task_requested";
      } else if (resolution.kind == "query") {
        status = "no_action";
      } else {
        status = "rejected_guard";
        log(LogLevel::info, "llm", "LLM proposal not executable: " + resolution.reason.value_or("None"));
      }
    }
  }
  json result = pfn::llm_result({.request = &request,
                                 .status = status,
                                 .task_id = task_id,
                                 .proposal = proposal,
                                 .origin = interpretation.origin != "fallback" ? interpretation.origin : "guard_fallback",
                                 .latency_ms = interpretation.latency_ms,
                                 .model_name = interpretation.model,
                                 .simulated = options_.simulated});
  messages.emplace_back(pfn::kLlmResult, checked(std::move(result), guard::validate_llm_result));
  char latency[32];
  std::snprintf(latency, sizeof latency, "%.0f", interpretation.latency_ms);
  log(LogLevel::info, "llm",
      "llm result status=" + status + " ai=" + text_or_none(proposal, "intent") + "/" + text_or_none(proposal, "device") +
          "/" + text_or_none(proposal, "action") + " latency_ms=" + latency);
  return messages;
}

}  // namespace hm
