// NODE-RPI5-02 deterministic voice parser: final transcript -> core task request or LLM request.
// Port of services/runtime voice.py (same guard, same scenario keywords, same messages).
// INTERFACE CONTRACT between agent A (implements libs/hm_parser) and agent B (calls it from
// services/homemind-perception). Change only together with both sides.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hm/action_map.hpp"
#include "hm/common.hpp"

namespace hm {

struct VoiceDecision {
  std::string kind;      // task_request | llm_request | clarify | no_action | query | unsupported | invalid
  json proposal;         // guard proposal or null
  std::vector<std::pair<std::string, json>> messages;  // (topic, pfn.v1 document) to publish, in order
  std::string note;
};

struct VoiceParserOptions {
  std::filesystem::path capabilities_path;  // services/exaone-edge/config/device-capabilities.json (or packaged copy)
  std::string node_id = "NODE-RPI5-02";
  int llm_ttl_ms = 10000;
  bool simulated = false;
  // Added by agent A (backward compatible): where rpi5-internal/task-proposal.schema.json lives.
  // Empty -> hm::default_contracts_dir(); callers should pass RuntimeConfig::contracts_dir.
  std::filesystem::path contracts_dir;
};

// handle() throws std::invalid_argument where the Python parser raises (pfn.v1 schema check of the message it
// builds fails, e.g. a non-uuid trace_id/utterance_id or stt_confidence outside [0, 1]).

class VoiceParser {
 public:
  VoiceParser(const ActionMap& action_map, VoiceParserOptions options);
  ~VoiceParser();
  VoiceParser(const VoiceParser&) = delete;
  VoiceParser& operator=(const VoiceParser&) = delete;

  // context: null, or the sanitized activity-context object
  //   {presence_zone, activity, audio_context, quiet_required, confidence}
  VoiceDecision handle(const std::string& text, const json& context = nullptr,
                       const std::string& stt_model_version = "keyboard",
                       std::optional<double> stt_confidence = std::nullopt,
                       std::optional<std::string> utterance_id = std::nullopt,
                       std::optional<std::string> trace_id = std::nullopt);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace hm
