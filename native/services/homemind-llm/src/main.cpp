// homemind-llm: NODE-RPI5-01 EXAONE node (port of `homemind-runtime llm`, services/runtime cli.run_llm).
//   homemind-llm --config runtime.json [--log-level INFO]   MQTT service (needs llama-server at llm_base_url)
//   homemind-llm --config runtime.json --once-stdin         one llm/request JSON per stdin line ->
//                                                           {"messages": [[topic, document], ...]} | {"error": ...}
// Config: the services/runtime keys (broker, llm_base_url, llm_model, llm_node_id, simulated, contracts_dir) plus
// optional "capabilities_path" (device-capabilities.json; relative to the config's base_dir). The MQTT password
// is read from broker.password_file only. The transcript is never logged.
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "hm/config.hpp"
#include "hm/exaone_client.hpp"
#include "hm/heartbeat.hpp"
#include "hm/mqtt.hpp"
#include "hm/pfn.hpp"
#include "hm/validation.hpp"
#include "llm_node.hpp"

namespace {

int usage(int code) {
  std::cerr << "usage: homemind-llm [--config FILE] [--log-level DEBUG|INFO|WARNING|ERROR] [--once-stdin]\n";
  return code;
}

// The first request after llama-server starts reads the whole prompt (rules and examples, about 2,000 tokens): on the
// Pi 5 that took longer than the 30 s request timeout (real Pi5-01 2026-10-25, every first request failed). One
// request at startup leaves that prefix in llama-server's prompt cache, so real requests read only their own part.
// Its answer is dropped. Requests that arrive meanwhile wait in the queue (an expired one is answered "expired").
void warm_up(const hm::guard::ExaoneClientOptions& base, const std::atomic<bool>& stopping) {
  auto options = base;
  options.timeout_seconds = 120.0;
  options.retries = 0;
  hm::guard::ExaoneClient client(options);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  for (;;) {
    if (stopping) return;
    try {
      if (client.health().value("status", std::string()) == "ok") break;
    } catch (const std::exception&) {  // still loading the model, or not started yet
    }
    if (std::chrono::steady_clock::now() > deadline) {
      hm::log(hm::LogLevel::warning, "llm", "EXAONE not ready after 10 minutes; no warm-up");
      return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  const auto started = std::chrono::steady_clock::now();
  try {
    client.interpret("안녕");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    hm::log(hm::LogLevel::info, "llm", "warm-up done in " + std::to_string(ms.count()) + " ms (prompt prefix cached)");
  } catch (const std::exception& error) {
    hm::log(hm::LogLevel::warning, "llm", std::string("warm-up failed: ") + error.what());
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  bool once_stdin = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--log-level" && i + 1 < argc) {
      hm::set_log_level(hm::parse_log_level(argv[++i]));
    } else if (arg == "--once-stdin") {
      once_stdin = true;
    } else if (arg == "--help" || arg == "-h") {
      return usage(0);
    } else {
      std::cerr << "unrecognized argument: " << arg << "\n";
      return usage(2);
    }
  }
  try {
    const hm::RuntimeConfig config = hm::RuntimeConfig::load(config_path);
    const auto capabilities_path = config.extra.contains("capabilities_path") && config.extra["capabilities_path"].is_string()
                                       ? config.resolve_path(config.extra["capabilities_path"].get<std::string>())
                                       : hm::guard::default_capabilities_path();
    hm::guard::ExaoneClientOptions options;
    options.base_url = config.llm_base_url;
    options.model = config.llm_model;
    options.capabilities = hm::guard::load_json_object(capabilities_path);
    options.contracts_dir = config.contracts_dir;
    options.timeout_seconds = 30.0;  // same as run_llm
    hm::guard::ExaoneClient client(options);
    const hm::ActionMap action_map(config.contracts_dir);
    hm::LlmNode node(client, action_map, {.node_id = config.llm_node_id, .simulated = config.simulated});

    if (once_stdin) {
      std::string line;
      while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        hm::json answer;
        try {
          hm::json messages = hm::json::array();
          for (auto& [topic, document] : node.handle(hm::json::parse(line))) messages.push_back({topic, document});
          answer = {{"messages", messages}};
        } catch (const std::exception& error) {
          answer = {{"error", error.what()}};
        }
        std::cout << hm::dump_compact(answer) << '\n' << std::flush;
      }
      return 0;
    }

    // Requests are handled on a worker thread: a model call (up to 2 x 30 s) must not block the MQTT network
    // thread, or the broker keepalive could expire. Same results as the Python node, which handles them inline.
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<hm::json> queue;
    bool stopping = false;
    std::atomic<bool> shutting_down = false;
    constexpr std::size_t kMaxQueued = 8;
    hm::Bus bus(config.broker, config.llm_node_id + "-llm");
    std::thread worker([&] {
      warm_up(options, shutting_down);
      for (;;) {
        hm::json request;
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, [&] { return stopping || !queue.empty(); });
          if (stopping) return;
          request = std::move(queue.front());
          queue.pop_front();
        }
        try {
          for (const auto& [topic, message] : node.handle(request)) bus.publish(topic, message);
        } catch (const std::exception& error) {  // one bad request must not stop the node
          hm::log(hm::LogLevel::error, "llm", std::string("request failed: ") + error.what());
        }
      }
    });
    bus.subscribe(hm::pfn::kLlmRequest, [&](const std::string&, const hm::json& document) {
      std::lock_guard lock(mutex);
      if (queue.size() >= kMaxQueued) {
        hm::log(hm::LogLevel::warning, "llm", "request queue full; dropping a request");
        return;
      }
      queue.push_back(document);
      ready.notify_one();
    });
    bus.start();
    // NODE-RPI5-01 online on the 7-inch UI and the phone (core marks a node offline after 15 s without one).
    hm::HeartbeatLoop heartbeat([&](const hm::json& document) { bus.publish(hm::kHeartbeatTopic, document, 0); },
                                [&] { return bus.connected(); }, config.llm_node_id, "pi5");
    hm::log(hm::LogLevel::info, "llm", "LLM node ready (" + config.llm_base_url + ")");
    hm::wait_for_shutdown_signal();
    shutting_down = true;
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    ready.notify_all();
    worker.join();
    heartbeat.stop();  // one "offline" heartbeat while still connected
    bus.stop();
    return 0;
  } catch (const std::exception& error) {
    hm::log(hm::LogLevel::error, "llm", error.what());
    return 1;
  }
}
