// Configuration file of homemind-mobile-api (device: /data/share/homemind/config/mobile-api.json).
// hm::RuntimeConfig keys (base_dir, broker{host, port, username, password_file, keepalive}, contracts_dir) plus:
//   device_registry          default <contracts_dir>/pfn.v1/device-registry.json (on RPI4-01: core's copy)
//   mqtt_enabled             default true (false: commands answer 503, like HOMEMIND_MQTT_ENABLED=false)
//   mqtt_client_id           default "homemind-mobile-api" (also the state/response topic suffix)
//   source_node_id           NODE-RPI4-01 | NODE-RPI5-01 (task-policy origin "ui"), default NODE-RPI4-01
//   listen_host, listen_port default 127.0.0.1, 8000
//   http_threads             default 4
//   data_dir                 required: tokens.json, pairing-used.json, default pairing secret (mode 0700)
//   pairing_secret_file      default <data_dir>/pairing-secret
//   public_base_url          URL the phone uses (QR base_url); --print-pairing-qr --base-url overrides
//   device_id, device_name   shown in the QR / claim answer (default source_node_id, "HomeMind <device_id>")
//   pairing_code_ttl_s       default 600 (30..3600)
//   pairing_max_failures_per_client / pairing_max_failures_global / pairing_failure_window_s   default 5 / 20 / 300
//   tls_enabled              default false: HTTPS with a self-signed certificate made on first start (tls.hpp);
//                            public_base_url must then be https://
//   tls_cert_file, tls_key_file   default <data_dir>/tls-cert.pem, <data_dir>/tls-key.pem (key 0600, never copied)
// Relative paths are resolved against base_dir. No secret value is ever read from this file.
#pragma once

#include <filesystem>
#include <string>

#include "auth.hpp"
#include "core_bridge.hpp"
#include "hm/config.hpp"

namespace hm::mobile {

struct ServiceConfig {
  RuntimeConfig runtime;
  std::filesystem::path registry_path;
  BridgeConfig bridge;
  std::string listen_host = "127.0.0.1";
  int listen_port = 8000;
  int http_threads = 4;
  AuthConfig auth;
  struct Tls {
    bool enabled = false;
    std::filesystem::path cert_file;
    std::filesystem::path key_file;
  } tls;

  static ServiceConfig load(const std::filesystem::path& path);
};

}  // namespace hm::mobile
