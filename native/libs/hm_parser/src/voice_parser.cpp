// Port of services/runtime/src/homemind_runtime/voice.py (VoiceParser.handle).
#include "hm/voice_parser.hpp"

#include <set>
#include <stdexcept>
#include <utility>

#include "hm/guardrails.hpp"
#include "hm/pfn.hpp"
#include "hm/pfn_schema.hpp"
#include "hm/pyjson.hpp"
#include "hm/text.hpp"
#include "hm/validation.hpp"

namespace hm {

namespace {

using py::ojson;

// Scenario intents (contracts/pfn.v1/scenario-rules.json) are not in the EXAONE vocabulary, so the parser maps
// them by keyword, only after the guard's negation/injection/risk checks (same table and order as voice.py).
const std::vector<std::pair<const char*, std::vector<const char*>>>& scenario_keywords() {
  static const std::vector<std::pair<const char*, std::vector<const char*>>> table = {
      {"quiet_mode_off", {"조용 모드 해제", "조용 모드 꺼", "회의 끝났", "회의 끝", "회의 다 했"}},
      {"prepare_home", {"귀가", "다녀왔", "집에 왔", "나 왔어", "왔어 집"}},
      {"prepare_sleep", {"취침", "잘게", "자러 갈", "잘 준비", "자야겠"}},
      {"night_bed_exit", {"밤에 화장실", "야간 조명", "화장실 좀 다녀올게", "화장실 가야겠"}},
  };
  return table;
}

const char* scenario_of(const std::string& text_value) {
  const std::string folded = text::casefold(text_value);
  for (const auto& [intent, keywords] : scenario_keywords()) {
    for (const char* keyword : keywords) {
      if (folded.find(keyword) != std::string::npos) return intent;
    }
  }
  return nullptr;
}

bool guard_stopped(const ojson& proposal) {
  static const std::set<std::string> kStop = {"NEGATED_REQUEST", "CONFLICTING_REQUEST", "HIGH_RISK"};
  if (!proposal.is_object() || !proposal.contains("safety_flags")) return false;
  for (const auto& flag : proposal["safety_flags"]) {
    if (flag.is_string() && kStop.count(flag.get<std::string>())) return true;
  }
  return false;
}

std::string joined(const std::vector<std::string>& items, const char* separator) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) out += (i ? separator : "") + items[i];
  return out;
}

// pfn.task_request(...) also validates the document (task-request + device-control schemas) and raises.
json checked_task_request(const pfn::TaskRequestArgs& args) {
  pfn::TaskRequestArgs rounded = args;
  // round(max(0, min(1, x)), 4) exactly like Python; hm_common's own rounding then leaves the value as is.
  rounded.confidence = py::round_digits(std::max(0.0, std::min(1.0, args.confidence)), 4);
  json document = pfn::task_request(rounded);
  if (const std::string problem = guard::validate_task_request(document); !problem.empty()) {
    throw std::invalid_argument(problem);
  }
  return document;
}

}  // namespace

struct VoiceParser::Impl {
  const ActionMap& action_map;
  VoiceParserOptions options;
  ojson capabilities;
  ojson schema;
};

VoiceParser::VoiceParser(const ActionMap& action_map, VoiceParserOptions options)
    : impl_(new Impl{action_map, std::move(options), nullptr, nullptr}) {
  try {
    impl_->capabilities = guard::load_json_object(impl_->options.capabilities_path.empty()
                                                      ? guard::default_capabilities_path()
                                                      : impl_->options.capabilities_path);
    impl_->schema = guard::load_json_object(guard::default_schema_path(impl_->options.contracts_dir));
  } catch (...) {
    delete impl_;
    throw;
  }
}

VoiceParser::~VoiceParser() { delete impl_; }

VoiceDecision VoiceParser::handle(const std::string& raw_text, const json& context,
                                  const std::string& stt_model_version, std::optional<double> stt_confidence,
                                  std::optional<std::string> utterance_id, std::optional<std::string> trace_id) {
  const Impl& self = *impl_;
  const std::string text_value = text::strip(text::sanitize(raw_text));
  if (text_value.empty()) return {"no_action", nullptr, {}, "empty transcript"};
  const std::string trace = trace_id && !trace_id->empty() ? *trace_id : new_id();
  const ojson proposal = guard::preflight_proposal(text_value, self.capabilities, py::to_ordered(context));
  const char* scenario = guard_stopped(proposal) ? nullptr : scenario_of(text_value);
  const double confidence = stt_confidence.value_or(0.9);

  if (scenario) {
    json request = checked_task_request({.source_node_id = self.options.node_id,
                                         .intent = scenario,
                                         .target_device = "HOME-ENV-01",
                                         .parameters = {{"origin", "voice_parser"}, {"ai_device", nullptr}, {"ai_action", nullptr}},
                                         .confidence = confidence,
                                         .trace_id = trace});
    return {"task_request", nullptr, {{pfn::kTaskRequest, std::move(request)}}, scenario};
  }
  if (proposal.is_null()) {
    json request = pfn::llm_request({.text = text_value,
                                     .utterance_id = utterance_id && !utterance_id->empty() ? *utterance_id : new_id(),
                                     .stt_model_version = stt_model_version,
                                     .stt_confidence = stt_confidence,
                                     .parser_result = "NO_MATCH",
                                     .context = context,
                                     .ttl_ms = self.options.llm_ttl_ms,
                                     .trace_id = trace,
                                     .simulated = self.options.simulated});
    if (const std::string problem = guard::validate_llm_request(request); !problem.empty()) {
      throw std::invalid_argument(problem);
    }
    return {"llm_request", nullptr, {{pfn::kLlmRequest, std::move(request)}}, "parser has no rule"};
  }

  ojson validated;
  try {
    validated = guard::validate_proposal(proposal, self.schema, self.capabilities);
  } catch (const guard::ProposalValidationError& error) {
    return {"invalid", py::to_plain(proposal), {}, joined(error.errors(), "; ")};
  }
  const json plain = py::to_plain(validated);
  const std::string intent = plain.value("intent", std::string());
  if (intent == "CLARIFY") {
    const json& question = plain["clarification_question"];
    return {"clarify", plain, {}, question.is_string() ? question.get<std::string>() : std::string()};
  }
  if (intent == "NO_ACTION") {
    std::vector<std::string> flags;
    for (const auto& flag : plain["safety_flags"]) flags.push_back(flag.get<std::string>());
    return {"no_action", plain, {}, joined(flags, ",")};
  }
  const Resolution resolution = self.action_map.resolve(plain);
  if (resolution.kind == "device_control") {
    json request = checked_task_request({.source_node_id = self.options.node_id,
                                         .intent = "device_control",
                                         .target_device = resolution.device_id.value_or(""),
                                         .parameters = control_parameters(resolution, "voice_parser", plain),
                                         .confidence = confidence,
                                         .trace_id = trace,
                                         .safety_level = self.action_map.is_door_open(resolution) ? "caution" : "normal"});
    return {"task_request", plain, {{pfn::kTaskRequest, std::move(request)}}, ""};
  }
  if (resolution.kind == "scenario") {
    json request = checked_task_request({.source_node_id = self.options.node_id,
                                         .intent = resolution.intent.value_or(""),
                                         .target_device = "HOME-ENV-01",
                                         .parameters = scenario_parameters("voice_parser", plain),
                                         .confidence = confidence,
                                         .trace_id = trace});
    return {"task_request", plain, {{pfn::kTaskRequest, std::move(request)}}, ""};
  }
  if (resolution.kind == "query") return {"query", plain, {}, resolution.device_id.value_or("")};
  return {"unsupported", plain, {}, resolution.reason.value_or("")};
}

}  // namespace hm
