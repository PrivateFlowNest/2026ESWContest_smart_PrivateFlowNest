// OpenAI-compatible llama.cpp (llama-server) client with fail-closed proposal handling.
// Port of services/exaone-edge/src/homemind_exaone/client.py ExaoneClient: identical request body bytes
// (json.dumps(payload, ensure_ascii=False)), same parsing, guard/candidate merge, retries, fallback and origins.
#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hm/http.hpp"
#include "hm/pyjson.hpp"

namespace hm::guard {

using py::ojson;

extern const char* const kSystemPrompt;

struct InterpretationResult {
  ojson proposal;
  bool accepted = false;
  std::string origin;                       // guard | model | model_guard | guard_fallback | fallback
  std::optional<bool> model_output_valid;   // None for guard answers
  int attempts = 0;
  double latency_ms = 0.0;
  std::string model;
  std::optional<std::string> raw_content;
  std::vector<std::string> errors;

  ojson as_dict(bool include_raw = false) const;
};

// (method, url, body, content_type, timeout_ms) -> response; status 0 = transport error.
using HttpTransport = std::function<http::Response(const std::string& method, const std::string& url,
                                                   const std::string& body, const std::string& content_type,
                                                   int timeout_ms)>;

struct ExaoneClientOptions {
  std::string base_url = "http://127.0.0.1:8080";
  std::string model = "exaone-4.0-1.2b";
  ojson schema = nullptr;        // null/{} -> <contracts>/rpi5-internal/task-proposal.schema.json
  ojson capabilities = nullptr;  // null/{} -> default_capabilities_path()
  ojson activity_context_schema = nullptr;  // null/{} -> <contracts>/rpi5-internal/activity-context.schema.json
  std::filesystem::path contracts_dir;      // empty -> hm::default_contracts_dir()
  double timeout_seconds = 120.0;
  int max_tokens = 256;
  int retries = 1;
  bool enable_guardrails = true;
  bool include_few_shots = true;
  HttpTransport transport;  // empty -> hm::http::request
};

class ExaoneClient {
 public:
  explicit ExaoneClient(ExaoneClientOptions options);

  const std::string& model() const { return options_.model; }
  const std::string& base_url() const { return base_url_; }
  const ojson& schema() const { return options_.schema; }
  const ojson& capabilities() const { return options_.capabilities; }

  // ExaoneClient._payload (context: the validated activity context; feedback: empty = none).
  ojson payload(const std::string& utterance, const ojson& context, const std::vector<std::string>& feedback,
                const ojson& deterministic_candidate) const;
  ojson health();
  // Throws std::invalid_argument for an empty utterance and ProposalValidationError for an invalid context,
  // like the Python client.
  InterpretationResult interpret(const std::string& utterance, const ojson& context = nullptr);

 private:
  ojson request_json(const std::string& path, const ojson* payload);

  ExaoneClientOptions options_;
  std::string base_url_;
  std::mutex request_mutex_;
};

}  // namespace hm::guard
