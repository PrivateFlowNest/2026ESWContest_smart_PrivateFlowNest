// Strict structural and semantic validation for untrusted LLM proposals.
// Port of services/exaone-edge/src/homemind_exaone/validation.py: same accept/reject decisions and the same
// error strings in the same order (they are fed back to the model on retry).
#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "hm/pyjson.hpp"

namespace hm::guard {

using py::ojson;

class ProposalValidationError : public std::runtime_error {
 public:
  explicit ProposalValidationError(std::vector<std::string> errors);
  const std::vector<std::string>& errors() const { return errors_; }

 private:
  std::vector<std::string> errors_;
};

// Python TypeError raised while validating (set() of unhashable safety_flags items): "unhashable type: 'list'".
class PyTypeError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// <contracts>/rpi5-internal/task-proposal.schema.json and activity-context.schema.json
std::filesystem::path default_schema_path(const std::filesystem::path& contracts_dir = {});
std::filesystem::path default_activity_context_schema_path(const std::filesystem::path& contracts_dir = {});
// $HM_CAPABILITIES_PATH, else services/exaone-edge/config/device-capabilities.json of the build tree,
// else <$HM_BASE_DIR or cwd>/services/exaone-edge/config/device-capabilities.json.
std::filesystem::path default_capabilities_path();

// json.loads(path.read_text("utf-8")); throws std::runtime_error "Expected a JSON object in <path>".
ojson load_json_object(const std::filesystem::path& path);

ojson default_activity_context();

// Returns the proposal (a copy, key order kept) or throws ProposalValidationError / PyTypeError.
ojson validate_proposal(const ojson& proposal, const ojson& schema, const ojson& capabilities);
// Fills defaults, then validates against the activity-context schema. context: null or object.
ojson validate_activity_context(const ojson& context, const ojson& schema);

ojson safe_fallback(bool unavailable, const std::optional<std::string>& question = std::nullopt);

}  // namespace hm::guard
