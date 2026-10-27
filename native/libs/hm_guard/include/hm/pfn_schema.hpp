// pfn.v1 JSON Schema checks that the Python runtime performs with jsonschema (Draft 2020-12 + FormatChecker)
// on llm/request (LlmNode input), task/request and llm/result (builder outputs). Same accept/reject decisions
// for these schemas; the message text is our own (Python raises ValueError("<schema>: ...")).
#pragma once

#include <string>

#include "hm/common.hpp"

namespace hm::guard {

// jsonschema "uuid" format: uuid.UUID(value) accepts it and value[8|13|18|23] == "-".
bool jsonschema_uuid(const std::string& value);

// "" when the document satisfies the schema, else the first problem found.
std::string validate_llm_request(const json& request);   // contracts/pfn.v1/llm-request.schema.json
std::string validate_task_request(const json& request);  // task-request (+ device-control for device_control)
std::string validate_llm_result(const json& result);     // contracts/pfn.v1/llm-result.schema.json

}  // namespace hm::guard
