// JSONL probe for PC parity/contract tests (native/tests): one request per stdin line, one JSON line out.
//   {"op":"resolve","proposal":{...}}           -> {"kind":..., "device_id":..., ..., "door_open":bool}
//   {"op":"task_request","args":{...}}          -> pfn.v1 task request
//   {"op":"llm_request","args":{...}}           -> pfn.v1 llm request
//   {"op":"llm_result","request":{...},"args":{...}} -> pfn.v1 llm result
//   {"op":"perception","args":{...}}            -> pfn.v1 perception
#include <iostream>
#include <string>

#include "hm/action_map.hpp"
#include "hm/config.hpp"
#include "hm/pfn.hpp"

namespace {

hm::json opt(const std::optional<std::string>& value) { return value ? hm::json(*value) : hm::json(nullptr); }

std::optional<std::string> opt_string(const hm::json& args, const char* key) {
  if (!args.contains(key) || args[key].is_null()) return std::nullopt;
  return args[key].get<std::string>();
}

hm::json handle(const hm::ActionMap& map, const hm::json& line) {
  const std::string op = line.at("op");
  const hm::json args = line.value("args", hm::json::object());
  if (op == "resolve") {
    const auto r = map.resolve(line.at("proposal"));
    return {{"kind", r.kind}, {"device_id", opt(r.device_id)}, {"command", opt(r.command)},
            {"parameters", r.parameters}, {"intent", opt(r.intent)}, {"reason", opt(r.reason)},
            {"door_open", map.is_door_open(r)}};
  }
  if (op == "task_request") {
    return hm::pfn::task_request({.source_node_id = args.at("source_node_id"),
                                  .intent = args.at("intent"),
                                  .target_device = args.at("target_device"),
                                  .parameters = args.value("parameters", hm::json::object()),
                                  .confidence = args.value("confidence", 0.9),
                                  .trace_id = opt_string(args, "trace_id"),
                                  .safety_level = args.value("safety_level", std::string("normal"))});
  }
  if (op == "llm_request") {
    std::optional<double> stt;
    if (args.contains("stt_confidence") && !args["stt_confidence"].is_null()) stt = args["stt_confidence"].get<double>();
    return hm::pfn::llm_request({.text = args.at("text"),
                                 .utterance_id = args.value("utterance_id", hm::new_id()),
                                 .stt_model_version = args.value("stt_model_version", std::string("keyboard")),
                                 .stt_confidence = stt,
                                 .parser_result = args.value("parser_result", std::string("NO_MATCH")),
                                 .context = args.value("context", hm::json(nullptr)),
                                 .ttl_ms = args.value("ttl_ms", 10000),
                                 .simulated = args.value("simulated", false)});
  }
  if (op == "llm_result") {
    const hm::json request = line.at("request");
    return hm::pfn::llm_result({.request = &request,
                                .status = args.at("status"),
                                .task_id = opt_string(args, "task_id"),
                                .proposal = args.value("proposal", hm::json(nullptr)),
                                .origin = args.at("origin"),
                                .latency_ms = args.value("latency_ms", 0.0),
                                .model_name = args.at("model_name"),
                                .simulated = args.value("simulated", false)});
  }
  if (op == "perception") {
    return hm::pfn::perception({.observation_type = args.at("observation_type"),
                                .zone = args.at("zone"),
                                .confidence = args.at("confidence"),
                                .attributes = args.at("attributes"),
                                .source_device_id = opt_string(args, "source_device_id"),
                                .simulated = args.value("simulated", false)});
  }
  if (op == "check_llm_request") return {{"problem", hm::pfn::check_llm_request(line.at("request"))}};
  throw std::runtime_error("unknown op " + op);
}

}  // namespace

int main(int argc, char** argv) {
  const auto contracts = argc > 1 ? std::filesystem::path(argv[1]) : hm::default_contracts_dir();
  hm::ActionMap map(contracts);
  std::string text;
  while (std::getline(std::cin, text)) {
    if (text.empty()) continue;
    try {
      std::cout << hm::dump_compact(handle(map, hm::json::parse(text))) << '\n';
    } catch (const std::exception& error) {
      std::cout << hm::dump_compact({{"error", error.what()}}) << '\n';
    }
  }
  return 0;
}
