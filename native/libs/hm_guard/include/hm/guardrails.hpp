// Deterministic guard for cases that must not depend on LLM judgment.
// Port of services/exaone-edge/src/homemind_exaone/guardrails.py (preflight_proposal), same rule order,
// same proposals (insertion order included) and questions.
#pragma once

#include <string_view>

#include "hm/pyjson.hpp"

namespace hm::guard {

using py::ojson;

// Returns a safe deterministic proposal, or null (Python None) when EXAONE should decide.
// capabilities: device-capabilities.json document. context: null or an object (only quiet_required is read).
ojson preflight_proposal(std::string_view utterance, const ojson& capabilities, const ojson& context = nullptr);

}  // namespace hm::guard
