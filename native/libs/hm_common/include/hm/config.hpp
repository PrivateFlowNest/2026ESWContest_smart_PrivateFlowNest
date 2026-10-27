// Runtime configuration (same JSON keys as services/runtime RuntimeConfig).
// Addresses and accounts live in the file; the MQTT password only as a file path.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "hm/common.hpp"

namespace hm {

struct BrokerConfig {
  std::string host = "127.0.0.1";
  int port = 1883;
  std::optional<std::string> username;
  std::optional<std::string> password_file;
  int keepalive = 30;

  // First line of password_file, trimmed. nullopt when no file is configured; throws if empty.
  std::optional<std::string> password() const;
};

struct RuntimeConfig {
  BrokerConfig broker;
  std::filesystem::path contracts_dir;
  std::string perception_node_id = "NODE-RPI5-02";
  std::string llm_node_id = "NODE-RPI5-01";
  std::string llm_base_url = "http://127.0.0.1:8080";
  std::string llm_model = "exaone-1.2b-homemind-v3";
  int llm_request_ttl_ms = 10000;
  bool simulated = true;
  // Every other top-level key (service-specific settings such as the perception "vad" block).
  json extra = json::object();

  // path empty -> defaults. Relative paths inside `extra` are left to each service
  // (resolve them with resolve_path against base_dir()).
  static RuntimeConfig load(const std::filesystem::path& path);
  std::filesystem::path base_dir() const { return base_dir_; }
  std::filesystem::path resolve_path(const std::string& value) const;

 private:
  std::filesystem::path base_dir_;
};

// Default contracts dir: $HM_CONTRACTS_DIR, else the build-time repository path, else ./contracts.
std::filesystem::path default_contracts_dir();

}  // namespace hm
