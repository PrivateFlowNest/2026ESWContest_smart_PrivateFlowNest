// NODE-RPI5-01 LLM node: pfn/v1/ai/llm/request -> EXAONE -> core task request (origin llm) + llm/result.
// Port of services/runtime/src/homemind_runtime/llm_node.py (LlmNode.handle).
// EXAONE only proposes; every task it sends needs UI approval in core (task-policy llm_needs_approval).
// The transcript is never logged and never forwarded to core.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "hm/action_map.hpp"
#include "hm/common.hpp"
#include "hm/exaone_client.hpp"

namespace hm {

struct LlmNodeOptions {
  std::string node_id = "NODE-RPI5-01";
  bool simulated = false;
};

class LlmNode {
 public:
  LlmNode(guard::ExaoneClient& client, const ActionMap& action_map, LlmNodeOptions options);

  // Messages to publish, in order. Throws std::invalid_argument where the Python node raises (request fails
  // llm-request.schema.json, empty transcript, invalid context); the bus then logs and drops the message.
  std::vector<std::pair<std::string, json>> handle(const json& request);

 private:
  guard::ExaoneClient& client_;
  const ActionMap& action_map_;
  LlmNodeOptions options_;
};

}  // namespace hm
