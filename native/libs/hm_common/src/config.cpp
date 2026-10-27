#include "hm/config.hpp"

#include <cstdlib>
#include <set>
#include <stdexcept>

namespace hm {

namespace fs = std::filesystem;

std::optional<std::string> BrokerConfig::password() const {
  if (!password_file || password_file->empty()) return std::nullopt;
  std::string text = read_text_file(*password_file);
  std::string line = text.substr(0, text.find_first_of("\r\n"));
  const auto begin = line.find_first_not_of(" \t");
  const auto end = line.find_last_not_of(" \t");
  if (begin == std::string::npos) throw std::runtime_error("MQTT password file is empty: " + *password_file);
  return line.substr(begin, end - begin + 1);
}

fs::path default_contracts_dir() {
  if (const char* env = std::getenv("HM_CONTRACTS_DIR"); env && *env) return env;
#ifdef HM_CONTRACTS_DIR_DEFAULT
  if (fs::exists(HM_CONTRACTS_DIR_DEFAULT)) return HM_CONTRACTS_DIR_DEFAULT;
#endif
  return fs::current_path() / "contracts";
}

namespace {

fs::path default_base_dir(const fs::path& config_dir) {
  if (const char* env = std::getenv("HM_BASE_DIR"); env && *env) return env;
#ifdef HM_REPO_ROOT_DEFAULT
  if (fs::exists(fs::path(HM_REPO_ROOT_DEFAULT) / "contracts")) return HM_REPO_ROOT_DEFAULT;
#endif
  return config_dir.empty() ? fs::current_path() : config_dir;
}

std::optional<std::string> optional_string(const json& value) {
  if (value.is_null()) return std::nullopt;
  return value.get<std::string>();
}

}  // namespace

RuntimeConfig RuntimeConfig::load(const fs::path& path) {
  RuntimeConfig config;
  if (path.empty()) {
    config.base_dir_ = default_base_dir({});
    config.contracts_dir = default_contracts_dir();
    return config;
  }
  json raw = read_json_file(path);
  if (!raw.is_object()) throw std::runtime_error(path.string() + ": config must be a JSON object");
  const fs::path config_dir = fs::absolute(path).parent_path();
  // "base_dir" (optional): what relative paths in the file are relative to. Default: repository root on
  // the PC build, the config file's folder on the device.
  if (raw.contains("base_dir")) {
    fs::path base = raw["base_dir"].get<std::string>();
    config.base_dir_ = base.is_absolute() ? base : (config_dir / base).lexically_normal();
    raw.erase("base_dir");
  } else {
    config.base_dir_ = default_base_dir(config_dir);
  }
  if (raw.contains("broker")) {
    const json& broker = raw["broker"];
    config.broker.host = broker.value("host", config.broker.host);
    config.broker.port = broker.value("port", config.broker.port);
    if (broker.contains("username")) config.broker.username = optional_string(broker["username"]);
    if (broker.contains("password_file")) config.broker.password_file = optional_string(broker["password_file"]);
    if (broker.contains("password")) throw std::runtime_error("broker.password is not accepted; use password_file");
    config.broker.keepalive = broker.value("keepalive", config.broker.keepalive);
    if (config.broker.password_file) config.broker.password_file = config.resolve_path(*config.broker.password_file).string();
    raw.erase("broker");
  }
  if (raw.contains("contracts_dir") && !raw["contracts_dir"].is_null()) {
    config.contracts_dir = config.resolve_path(raw["contracts_dir"].get<std::string>());
  } else {
    config.contracts_dir = default_contracts_dir();
  }
  raw.erase("contracts_dir");
  config.perception_node_id = raw.value("perception_node_id", config.perception_node_id);
  config.llm_node_id = raw.value("llm_node_id", config.llm_node_id);
  config.llm_base_url = raw.value("llm_base_url", config.llm_base_url);
  config.llm_model = raw.value("llm_model", config.llm_model);
  config.llm_request_ttl_ms = raw.value("llm_request_ttl_ms", config.llm_request_ttl_ms);
  config.simulated = raw.value("simulated", config.simulated);
  for (const char* key : {"perception_node_id", "llm_node_id", "llm_base_url", "llm_model", "llm_request_ttl_ms",
                          "simulated"}) {
    raw.erase(key);
  }
  config.extra = std::move(raw);
  return config;
}

fs::path RuntimeConfig::resolve_path(const std::string& value) const {
  fs::path candidate(value);
  if (candidate.is_absolute()) return candidate;
  return (base_dir_ / candidate).lexically_normal();
}

}  // namespace hm
