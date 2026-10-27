// homemind-mobile-api: phone app HTTP API on NODE-RPI4-01, next to homemind-core (C++ port of the Python
// Mobile API in team/homemind-application-2026-10-21/backend). Talks to core only over pfn.v1 MQTT.
//   homemind-mobile-api --config FILE [--log-level LEVEL]          serve (HTTP/1.1, bearer tokens)
//   homemind-mobile-api --config FILE --print-pairing-qr [--base-url URL] [--ttl SECONDS] [--json]
//   homemind-mobile-api --config FILE --list-tokens
//   homemind-mobile-api --config FILE --revoke-token ID | --revoke-all-tokens
//   homemind-mobile-api --version
// The pairing commands are run by a person on the device; they print the code to the terminal only.
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "api.hpp"
#include "hm/mqtt.hpp"
#include "pairing_relay.hpp"
#include "phones_relay.hpp"
#include "service_config.hpp"
#include "terminal_qr.hpp"
#include "tls.hpp"

namespace {

using namespace hm::mobile;

int usage(int code) {
  (code ? std::cerr : std::cout)
      << "usage: homemind-mobile-api --config FILE [--log-level LEVEL]\n"
         "       homemind-mobile-api --config FILE --print-pairing-qr [--base-url URL] [--ttl SECONDS] [--json]\n"
         "       homemind-mobile-api --config FILE --list-tokens\n"
         "       homemind-mobile-api --config FILE --revoke-token ID | --revoke-all-tokens\n"
         "       homemind-mobile-api --version\n";
  return code;
}

class BusPublisher : public Publisher {
 public:
  hm::Bus* bus = nullptr;
  bool connected() const override { return bus && bus->connected(); }
  bool publish(const std::string& topic, const hm::json& document) override {
    return bus && bus->publish(topic, document, 1);
  }
};

std::string text_of(const hm::json& task, const char* key) {
  return task.contains(key) && task[key].is_string() ? task[key].get<std::string>() : std::string();
}

// Every task core announces feeds the phone's approval list; GET /api/v1/events tells the phone what changed.
void observe_tasks(const std::string& topic, const hm::json& document, ApprovalBook& approvals, EventHub& events) {
  if (!document.is_object()) return;
  const std::int64_t now = hm::now_ms();
  bool changed = false;
  if (topic == kTaskUpdate) {
    const hm::json task = document.value("task", hm::json::object());
    if (!task.is_object() || text_of(task, "task_id").empty()) return;
    changed = approvals.observe(task, now);
    events.publish({{"type", "task"},
                    {"task_id", text_of(task, "task_id")},
                    {"intent", text_of(task, "intent")},
                    {"state", text_of(task, "state")}});
  } else if (document.contains("tasks")) {
    changed = approvals.sync(document["tasks"], now);
  }
  if (changed) events.publish({{"type", "approvals"}, {"pending", approvals.pending().size()}});
}

// GET /api/v1/events: server-sent events for one paired phone. "task" and "approvals" events as JSON, a comment
// every 15 s so idle proxies and the phone notice a dead connection. Not OS push: the app must be running.
// `still_paired` is checked before every write: a phone unpaired on the 7-inch UI loses its stream within 15 s.
void serve_events(EventHub& events, const httplib::Request& req, httplib::Response& res,
                  std::function<bool()> still_paired) {
  std::uint64_t after = events.latest();
  if (req.has_param("after")) {
    try {
      after = std::min<std::uint64_t>(std::stoull(req.get_param_value("after")), after);
    } catch (const std::exception&) {
    }
  }
  res.set_header("Cache-Control", "no-store");
  res.set_chunked_content_provider("text/event-stream", [&events, after, still_paired](std::size_t,
                                                                                    httplib::DataSink& sink) mutable {
    if (events.closed()) return false;
    std::string chunk;
    for (const auto& event : events.wait_after(after, std::chrono::seconds(15))) {
      after = event["seq"].get<std::uint64_t>();
      chunk += "id: " + std::to_string(after) + "\nevent: " + event["type"].get<std::string>() +
               "\ndata: " + hm::dump_compact(event) + "\n\n";
    }
    if (events.closed() || !still_paired()) return false;
    if (chunk.empty()) chunk = ": keep-alive\n\n";
    return sink.write(chunk.data(), chunk.size());
  });
}

// The pairing settings this run uses: with tls_enabled, the certificate (made on first use) and its fingerprint.
AuthConfig auth_with_tls(const ServiceConfig& config) {
  AuthConfig auth = config.auth;
  if (config.tls.enabled) {
    auth.tls_sha256 = ensure_tls_identity(config.tls.cert_file, config.tls.key_file, auth.device_id,
                                          url_host(auth.public_base_url))
                          .sha256;
  }
  return auth;
}

int print_pairing_qr(const ServiceConfig& config, const std::string& base_url_override, std::optional<int> ttl,
                     bool json_only) {
  const AuthConfig auth = auth_with_tls(config);
  TokenStore tokens(auth.data_dir);
  PairingService pairing(auth, tokens);
  pairing.prepare();
  const std::string base_url = base_url_override.empty() ? auth.public_base_url : base_url_override;
  const PairingOffer offer = pairing.make_offer(hm::now_ms() / 1000, base_url, ttl.value_or(config.auth.code_ttl_s));
  const std::string payload = hm::dump_compact(offer.qr);
  if (json_only) {
    std::cout << payload << "\n";
    return 0;
  }
  std::cout << "HomeMind pairing (one-time, valid until " << utc_iso_seconds(offer.expires_at_s) << " UTC)\n"
            << "  device:       " << config.auth.device_name << " (" << config.auth.device_id << ")\n"
            << "  base URL:     " << offer.qr["base_url"].get<std::string>() << "\n"
            << "  pairing code: " << offer.code << "   <- compare with the code the app shows\n"
            << (offer.qr.contains("tls_sha256")
                    ? "  HTTPS cert:   sha256 " + offer.qr["tls_sha256"].get<std::string>() + " (pinned by the app)\n"
                    : std::string())
            << "Scan with the HomeMind app (QR drawn locally; never paste the payload into an online generator):\n"
            << render_terminal_qr(payload) << "QR payload (for --json users):\n"
            << payload << "\n";
  return 0;
}

int list_tokens(const ServiceConfig& config) {
  TokenStore tokens(config.auth.data_dir);
  const auto records = tokens.list();
  if (records.empty()) std::cout << "no paired phones\n";
  for (const auto& record : records) {
    std::cout << record.id << "  " << record.created_at << "  " << record.device_name << "\n";
  }
  return 0;
}

int serve(const ServiceConfig& config) {
  const Registry registry(config.registry_path);
  HardwareStore store(registry);
  const AuthConfig auth = auth_with_tls(config);
  TokenStore tokens(auth.data_dir);
  PairingService pairing(auth, tokens);
  pairing.prepare();

  ApprovalBook approvals;
  EventHub events;
  BusPublisher publisher;
  // 7-inch UI "phone connect": one-time offers on pfn/v1/mobile/pairing/offer (broker ACL: only NODE-RPI5-01 reads).
  PairingRelay relay(pairing, auth, config.bridge.source_node_id, config.runtime.simulated,
                     [&publisher](const std::string& topic, const hm::json& document) {
                       return publisher.publish(topic, document);
                     });
  // 7-inch UI phone list and unpair on pfn/v1/mobile/phones/state (broker ACL: only NODE-RPI5-01 reads).
  PhonesRelay phones(tokens, config.bridge.source_node_id, config.runtime.simulated,
                     [&publisher](const std::string& topic, const hm::json& document) {
                       return publisher.publish(topic, document);
                     });
  CoreBridge bridge(
      config.bridge, &publisher,
      [&store](const TrackedTask& task) {
        if (task.state == "completed") store.confirm_command(task.device_id, task.command);
        store.task_changed(task.device_id, task.task_id, task.state, task.failure_field("code"));
      },
      [&store](const std::map<std::string, std::string>& statuses) { store.set_node_status(statuses); });
  std::unique_ptr<hm::Bus> bus;
  if (config.bridge.enabled) {
    bus = std::make_unique<hm::Bus>(config.runtime.broker, config.bridge.client_id);
    publisher.bus = bus.get();
    auto forward = [&bridge, &approvals, &events](const std::string& topic, const hm::json& document) {
      bridge.on_message(topic, document);
      observe_tasks(topic, document, approvals, events);
    };
    bus->subscribe(kTaskUpdate, forward);
    bus->subscribe(bridge.state_response_topic(), forward);
    bus->subscribe(kPairingRequest,
                   [&relay](const std::string&, const hm::json& document) { relay.on_request(document, hm::now_ms()); });
    bus->subscribe(kPhonesRequest,
                   [&phones](const std::string&, const hm::json& document) { phones.on_request(document, hm::now_ms()); });
    bus->on_connected([&bridge] { bridge.request_snapshot(); });
    bus->start(3000);
  } else {
    hm::log(hm::LogLevel::info, "mobile-api", "MQTT disabled: commands are refused (503)");
  }

  Api api(registry, store, bridge, tokens, pairing, approvals);
  api.on_paired([&phones] { phones.notify_paired(hm::now_ms()); });
  std::unique_ptr<httplib::Server> listener;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  if (config.tls.enabled) {
    listener = std::make_unique<httplib::SSLServer>(config.tls.cert_file.c_str(), config.tls.key_file.c_str());
  }
#endif
  if (!listener) listener = std::make_unique<httplib::Server>();
  if (!listener->is_valid()) {
    hm::log(hm::LogLevel::error, "mobile-api",
            "HTTPS certificate or key cannot be used: " + config.tls.cert_file.string());
    if (bus) bus->stop();
    return 1;
  }
  httplib::Server& server = *listener;
  // `http_threads` workers, growing to 4x while idle keep-alive connections (phones) hold some; at most 64 queued.
  const auto threads = static_cast<std::size_t>(config.http_threads);
  server.new_task_queue = [threads] { return new httplib::ThreadPool(threads, threads * 4, 64); };
  server.set_payload_max_length(64 * 1024);
  server.set_read_timeout(10);
  server.set_write_timeout(10);
  server.set_keep_alive_timeout(5);
  auto handler = [&api](const httplib::Request& req, httplib::Response& res) {
    HttpRequest request{req.method,
                        req.path,
                        req.get_header_value("Authorization"),
                        req.get_header_value("Content-Type"),
                        req.body,
                        req.remote_addr};
    const HttpResponse response = api.handle(request);
    res.status = response.status;
    for (const auto& [name, value] : response.headers) res.set_header(name, value);
    res.set_content(hm::dump_compact(response.body), "application/json");
    hm::log(hm::LogLevel::debug, "http",
            req.remote_addr + " " + req.method + " " + req.path + " -> " + std::to_string(response.status));
  };
  // Registered before ".*": cpp-httplib takes the first matching route.
  server.Get("/api/v1/events", [&api, &events](const httplib::Request& req, httplib::Response& res) {
    if (!api.authorized(req.get_header_value("Authorization"))) {
      res.status = 401;
      res.set_header("WWW-Authenticate", "Bearer");
      res.set_content(hm::dump_compact({{"detail", "missing or invalid token"}}), "application/json");
      return;
    }
    hm::log(hm::LogLevel::debug, "http", req.remote_addr + " GET /api/v1/events (stream)");
    serve_events(events, req, res, [&api, authorization = req.get_header_value("Authorization")] {
      return api.authorized(authorization);
    });
  });
  for (const char* pattern : {".*"}) {
    server.Get(pattern, handler);
    server.Post(pattern, handler);
    server.Put(pattern, handler);
    server.Patch(pattern, handler);
    server.Delete(pattern, handler);
    server.Options(pattern, handler);
  }
  // Errors cpp-httplib produces itself (malformed request, 413, ...) also answer FastAPI-style JSON.
  server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty()) {
      res.set_content(hm::dump_compact({{"detail", httplib::status_message(res.status)}}), "application/json");
    }
  });
  server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
    res.status = 500;
    res.set_content(hm::dump_compact({{"detail", "Internal Server Error"}}), "application/json");
  });

  if (!server.bind_to_port(config.listen_host, config.listen_port)) {
    hm::log(hm::LogLevel::error, "mobile-api",
            "cannot listen on " + config.listen_host + ":" + std::to_string(config.listen_port));
    if (bus) bus->stop();
    return 1;
  }
  hm::log(hm::LogLevel::info, "mobile-api",
          std::string(kVersion) + " listening on " + config.listen_host + ":" + std::to_string(config.listen_port) +
              (config.tls.enabled ? " HTTPS (cert sha256 " + auth.tls_sha256.substr(0, 16) + "...)" : " HTTP") +
              " (bearer token required; registry " + config.registry_path.string() + ")");
  std::thread http([&server] { server.listen_after_bind(); });
  hm::wait_for_shutdown_signal();
  hm::log(hm::LogLevel::info, "mobile-api", "stopping");
  events.close();  // ends open event streams so server.stop() does not wait for them
  server.stop();
  http.join();
  if (bus) bus->stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path, base_url, revoke_id;
  std::optional<int> ttl;
  bool print_qr = false, json_only = false, list = false, revoke_all = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(std::string(name) + " needs a value");
      return argv[++i];
    };
    try {
      if (arg == "--config") {
        config_path = next("--config");
      } else if (arg == "--log-level") {
        hm::set_log_level(hm::parse_log_level(next("--log-level")));
      } else if (arg == "--print-pairing-qr") {
        print_qr = true;
      } else if (arg == "--base-url") {
        base_url = next("--base-url");
      } else if (arg == "--ttl") {
        ttl = std::stoi(next("--ttl"));
      } else if (arg == "--json") {
        json_only = true;
      } else if (arg == "--list-tokens") {
        list = true;
      } else if (arg == "--revoke-token") {
        revoke_id = next("--revoke-token");
      } else if (arg == "--revoke-all-tokens") {
        revoke_all = true;
      } else if (arg == "--version") {
        std::cout << kServiceName << " " << kVersion << (tls_supported() ? " +tls" : "") << "\n";
        return 0;
      } else if (arg == "--help" || arg == "-h") {
        return usage(0);
      } else {
        std::cerr << "unrecognized argument: " << arg << "\n";
        return usage(2);
      }
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return usage(2);
    }
  }
  if (int(print_qr) + int(list) + int(!revoke_id.empty()) + int(revoke_all) > 1) {
    std::cerr << "choose one of --print-pairing-qr, --list-tokens, --revoke-token, --revoke-all-tokens\n";
    return 2;
  }
  try {
    const ServiceConfig config = ServiceConfig::load(config_path);
    if (print_qr) return print_pairing_qr(config, base_url, ttl, json_only);
    if (list) return list_tokens(config);
    if (!revoke_id.empty()) {
      TokenStore tokens(config.auth.data_dir);
      if (!tokens.revoke(revoke_id)) {
        std::cerr << "no token with id " << revoke_id << "\n";
        return 1;
      }
      std::cout << "revoked " << revoke_id << "\n";
      return 0;
    }
    if (revoke_all) {
      TokenStore tokens(config.auth.data_dir);
      std::cout << "revoked " << tokens.revoke_all() << " token(s)\n";
      return 0;
    }
    return serve(config);
  } catch (const std::exception& error) {
    hm::log(hm::LogLevel::error, "mobile-api", error.what());
    return 1;
  }
}
