// JSONL probe for native/tests/test_parser_parity.py: one VoiceParser.handle call per stdin line.
//   {"text": s, "context": c|null, "stt_model_version": s, "stt_confidence": x|null, "utterance_id": s|null,
//    "trace_id": s|null, "node_id": s, "llm_ttl_ms": n, "simulated": b}
//   -> {"kind": ..., "proposal": ..., "messages": [[topic, document], ...], "note": ...} | {"error": ...}
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <tuple>

#include "hm/config.hpp"
#include "hm/pyjson.hpp"
#include "hm/voice_parser.hpp"

namespace {

std::optional<std::string> optional_string(const hm::json& line, const char* key) {
  if (!line.contains(key) || line[key].is_null()) return std::nullopt;
  return line[key].get<std::string>();
}

}  // namespace

int main() {
  const auto contracts = hm::default_contracts_dir();
  hm::ActionMap action_map(contracts);
  std::map<std::tuple<std::string, int, bool>, std::unique_ptr<hm::VoiceParser>> parsers;
  std::string text;
  while (std::getline(std::cin, text)) {
    if (text.empty()) continue;
    hm::json answer;
    try {
      const hm::json line = hm::py::to_plain(hm::py::loads(text));
      const auto key = std::make_tuple(line.value("node_id", std::string("NODE-RPI5-02")), line.value("llm_ttl_ms", 10000),
                                       line.value("simulated", false));
      auto& parser = parsers[key];
      if (!parser) {
        hm::VoiceParserOptions options;
        options.node_id = std::get<0>(key);
        options.llm_ttl_ms = std::get<1>(key);
        options.simulated = std::get<2>(key);
        options.contracts_dir = contracts;
        parser = std::make_unique<hm::VoiceParser>(action_map, options);
      }
      std::optional<double> confidence;
      if (line.contains("stt_confidence") && !line["stt_confidence"].is_null()) confidence = line["stt_confidence"].get<double>();
      const hm::VoiceDecision decision =
          parser->handle(line.at("text").get<std::string>(), line.value("context", hm::json(nullptr)),
                         line.value("stt_model_version", std::string("keyboard")), confidence,
                         optional_string(line, "utterance_id"), optional_string(line, "trace_id"));
      hm::json messages = hm::json::array();
      for (const auto& [topic, document] : decision.messages) messages.push_back({topic, document});
      answer = {{"kind", decision.kind}, {"proposal", decision.proposal}, {"messages", messages}, {"note", decision.note}};
    } catch (const std::exception& error) {
      answer = {{"error", error.what()}};
    }
    std::cout << hm::py::dumps(answer, true, true) << '\n' << std::flush;
  }
  return 0;
}
