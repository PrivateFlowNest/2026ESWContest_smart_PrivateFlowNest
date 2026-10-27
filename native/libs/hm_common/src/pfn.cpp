#include "hm/pfn.hpp"

namespace hm::pfn {

namespace {

// First `limit` Unicode code points of UTF-8 text (Python text[:limit]).
std::string utf8_prefix(const std::string& text, std::size_t limit) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      if (count == limit) return text.substr(0, i);
      ++count;
    }
  }
  return text;
}

json value_or_null(const json& object, const char* key) {
  if (!object.is_object()) return nullptr;
  auto it = object.find(key);
  return it == object.end() ? json(nullptr) : *it;
}

bool is_uuid(const json& value) {
  if (!value.is_string()) return false;
  const auto& text = value.get_ref<const std::string&>();
  if (text.size() != 36) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

}  // namespace

json task_request(const TaskRequestArgs& args) {
  return {
      {"schema_version", "pfn.v1"},
      {"event_id", new_id()},
      {"trace_id", args.trace_id.value_or(new_id())},
      {"task_id", new_id()},
      {"source_node_id", args.source_node_id},
      {"intent", args.intent},
      {"target_device", args.target_device},
      {"priority", 50},
      {"safety_level", args.safety_level},
      {"interruptible", true},
      {"resume_policy", "never"},
      {"confidence", confidence4(args.confidence)},
      {"parameters", args.parameters},
      {"requested_at_ms", now_ms()},
  };
}

json llm_request(const LlmRequestArgs& args) {
  const auto requested = now_ms();
  json document = {
      {"schema_version", "pfn.v1"},
      {"event_id", new_id()},
      {"trace_id", args.trace_id.value_or(new_id())},
      {"request_id", new_id()},
      {"source_node_id", args.source_node_id},
      {"utterance_id", args.utterance_id},
      {"transcript",
       {{"text", utf8_prefix(args.text, 500)},
        {"language", "ko"},
        {"stt_model_version", args.stt_model_version},
        {"stt_confidence", args.stt_confidence ? json(*args.stt_confidence) : json(nullptr)}}},
      {"parser", {{"result", args.parser_result}, {"rule_version", kParserRuleVersion}}},
      {"requested_at_ms", requested},
      {"expires_at_ms", requested + args.ttl_ms},
  };
  if (args.context.is_object() && !args.context.empty()) document["context"] = args.context;
  if (args.simulated) document["simulated"] = true;
  return document;
}

json llm_result(const LlmResultArgs& args) {
  const json& request = *args.request;
  json flags = json::array();
  if (args.proposal.is_object() && args.proposal.contains("safety_flags")) flags = args.proposal["safety_flags"];
  json document = {
      {"schema_version", "pfn.v1"},
      {"event_id", new_id()},
      {"trace_id", request.at("trace_id")},
      {"request_id", request.at("request_id")},
      {"source_node_id", args.source_node_id},
      {"status", args.status},
      {"task_id", args.task_id ? json(*args.task_id) : json(nullptr)},
      {"ai_intent", value_or_null(args.proposal, "intent")},
      {"ai_device", value_or_null(args.proposal, "device")},
      {"ai_action", value_or_null(args.proposal, "action")},
      {"safety_flags", flags},
      {"clarification_question", value_or_null(args.proposal, "clarification_question")},
      {"model", {{"name", args.model_name}, {"origin", args.origin}, {"latency_ms", round_to(args.latency_ms, 3)}}},
      {"completed_at_ms", now_ms()},
  };
  if (args.simulated) document["simulated"] = true;
  return document;
}

json perception(const PerceptionArgs& args) {
  json document = {
      {"schema_version", "pfn.v1"},
      {"event_id", new_id()},
      {"trace_id", args.trace_id.value_or(new_id())},
      {"source_node_id", args.source_node_id},
      {"observation_type", args.observation_type},
      {"zone", args.zone},
      {"confidence", confidence4(args.confidence)},
      {"attributes", args.attributes},
      {"observed_at_ms", now_ms()},
  };
  if (args.source_device_id) document["source_device_id"] = *args.source_device_id;
  if (args.simulated) document["simulated"] = true;
  return document;
}

std::string check_llm_request(const json& request) {
  if (!request.is_object()) return "not an object";
  if (request.value("schema_version", "") != "pfn.v1") return "schema_version";
  for (const char* key : {"event_id", "trace_id", "request_id", "utterance_id"}) {
    if (!request.contains(key) || !is_uuid(request[key])) return std::string(key) + " must be a uuid";
  }
  if (request.value("source_node_id", "") != "NODE-RPI5-02") return "source_node_id";
  for (const char* key : {"requested_at_ms", "expires_at_ms"}) {
    if (!request.contains(key) || !request[key].is_number_integer() || request[key].get<std::int64_t>() < 0) {
      return std::string(key) + " must be a non-negative integer";
    }
  }
  const json& transcript = request.contains("transcript") ? request["transcript"] : json();
  if (!transcript.is_object() || !transcript.contains("text") || !transcript["text"].is_string() ||
      transcript["text"].get_ref<const std::string&>().empty()) {
    return "transcript.text";
  }
  if (transcript.value("language", "") != "ko") return "transcript.language";
  const json& parser = request.contains("parser") ? request["parser"] : json();
  if (!parser.is_object() || !parser.contains("result") || !parser["result"].is_string()) return "parser.result";
  if (request.contains("context") && !request["context"].is_object()) return "context";
  return {};
}

}  // namespace hm::pfn
