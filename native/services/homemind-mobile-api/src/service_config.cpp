#include "service_config.hpp"

#include <stdexcept>

namespace hm::mobile {

namespace {

template <typename T>
T value_of(const json& extra, const char* key, T fallback) {
  if (!extra.contains(key) || extra[key].is_null()) return fallback;
  try {
    return extra[key].get<T>();
  } catch (const json::exception&) {
    throw std::runtime_error(std::string("config key '") + key + "' has the wrong type");
  }
}

}  // namespace

ServiceConfig ServiceConfig::load(const std::filesystem::path& path) {
  if (path.empty()) throw std::runtime_error("--config is required");
  ServiceConfig config;
  config.runtime = RuntimeConfig::load(path);
  const json& extra = config.runtime.extra;

  const std::string registry = value_of<std::string>(extra, "device_registry", "");
  config.registry_path = registry.empty() ? config.runtime.contracts_dir / "pfn.v1" / "device-registry.json"
                                          : config.runtime.resolve_path(registry);

  config.bridge.enabled = value_of<bool>(extra, "mqtt_enabled", true);
  config.bridge.client_id = value_of<std::string>(extra, "mqtt_client_id", "homemind-mobile-api");
  if (config.bridge.client_id.empty() || config.bridge.client_id.size() > 64 ||
      config.bridge.client_id.find_first_of("+#/") != std::string::npos) {
    throw std::runtime_error("mqtt_client_id must be 1..64 characters without + # /");
  }
  config.bridge.source_node_id = value_of<std::string>(extra, "source_node_id", "NODE-RPI4-01");
  if (!allowed_source_node(config.bridge.source_node_id)) {
    throw std::runtime_error("source_node_id must be NODE-RPI4-01 or NODE-RPI5-01 (task-policy origin ui)");
  }

  config.listen_host = value_of<std::string>(extra, "listen_host", config.listen_host);
  config.listen_port = value_of<int>(extra, "listen_port", config.listen_port);
  if (config.listen_port < 1 || config.listen_port > 65535) throw std::runtime_error("listen_port out of range");
  config.http_threads = value_of<int>(extra, "http_threads", config.http_threads);
  if (config.http_threads < 1 || config.http_threads > 64) throw std::runtime_error("http_threads must be 1..64");

  const std::string data_dir = value_of<std::string>(extra, "data_dir", "");
  if (data_dir.empty()) throw std::runtime_error("data_dir must be set (tokens and pairing state live there)");
  config.auth.data_dir = config.runtime.resolve_path(data_dir);
  const std::string secret = value_of<std::string>(extra, "pairing_secret_file", "");
  config.auth.secret_file = secret.empty() ? config.auth.data_dir / "pairing-secret" : config.runtime.resolve_path(secret);
  if (extra.contains("pairing_secret")) throw std::runtime_error("pairing_secret is not accepted; use pairing_secret_file");
  config.auth.public_base_url = value_of<std::string>(extra, "public_base_url", "");
  config.auth.device_id = value_of<std::string>(extra, "device_id", config.bridge.source_node_id);
  config.auth.device_name = value_of<std::string>(extra, "device_name", "HomeMind " + config.auth.device_id);
  config.auth.code_ttl_s = value_of<int>(extra, "pairing_code_ttl_s", config.auth.code_ttl_s);
  if (config.auth.code_ttl_s < kMinCodeTtlS || config.auth.code_ttl_s > kMaxCodeTtlS) {
    throw std::runtime_error("pairing_code_ttl_s must be 30..3600");
  }
  config.auth.max_failures_per_client =
      value_of<int>(extra, "pairing_max_failures_per_client", config.auth.max_failures_per_client);
  config.auth.max_failures_global = value_of<int>(extra, "pairing_max_failures_global", config.auth.max_failures_global);
  config.auth.failure_window_s = value_of<int>(extra, "pairing_failure_window_s", config.auth.failure_window_s);
  if (config.auth.max_failures_per_client < 1 || config.auth.max_failures_global < 1 ||
      config.auth.failure_window_s < 1) {
    throw std::runtime_error("pairing failure limits must be positive");
  }
  config.tls.enabled = value_of<bool>(extra, "tls_enabled", false);
  const std::string cert = value_of<std::string>(extra, "tls_cert_file", "");
  const std::string key = value_of<std::string>(extra, "tls_key_file", "");
  config.tls.cert_file = cert.empty() ? config.auth.data_dir / "tls-cert.pem" : config.runtime.resolve_path(cert);
  config.tls.key_file = key.empty() ? config.auth.data_dir / "tls-key.pem" : config.runtime.resolve_path(key);
  if (extra.contains("tls_key") || extra.contains("tls_private_key")) {
    throw std::runtime_error("TLS keys are not accepted in the config; use tls_key_file");
  }
  if (config.tls.enabled && !config.auth.public_base_url.empty() &&
      config.auth.public_base_url.rfind("https://", 0) != 0) {
    throw std::runtime_error("tls_enabled needs an https:// public_base_url");
  }
  return config;
}

}  // namespace hm::mobile
