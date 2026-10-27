// homemind-clock-sync: sets this Pi 5's clock from core's (NODE-RPI4-01). homemind-clock-sync.timer runs it 20 s after
// boot and then every 10 minutes. The Pis keep no time over a power cycle (real Pi5-01 was 154 s behind on 2026-10-24)
// while a 7-inch voice press expires after 3 s on NODE-RPI5-02 and core checks command expiry: the nodes must agree
// with core. Core's clock need not be right; the house needs one time (deploy/sync-time.sh sets the real time).
//   homemind-clock-sync (--config FILE | --config-dir DIR) [--client-id ID] [--max-skew-ms 1000] [--dry-run]
//                       [--log-level LEVEL]
// --config-dir takes the node's rpi5-0X.json there. The state request (pfn/v1/core/state/request) carries the node ID
// (perception_node_id or llm_node_id of that file) as client_id: the broker ACL lets each node read only its own
// response topic. Setting the clock needs CAP_SYS_TIME (root). Exit 0: clock right or set, 1: no usable answer from
// core, 2: usage, 3: the clock could not be set.
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

#include "clock_offset.hpp"
#include "hm/config.hpp"
#include "hm/mqtt.hpp"

namespace {

int usage(int code) {
  std::cerr << "usage: homemind-clock-sync (--config FILE | --config-dir DIR) [--client-id ID] [--max-skew-ms MS]\n"
               "                           [--dry-run] [--log-level LEVEL]\n";
  return code;
}

std::int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The single rpi5-0X.json of this node.
std::filesystem::path node_config(const std::filesystem::path& dir) {
  for (const char* name : {"rpi5-02.json", "rpi5-01.json"}) {
    if (std::filesystem::exists(dir / name)) return dir / name;
  }
  throw std::runtime_error("no rpi5-01.json or rpi5-02.json in " + dir.string());
}

std::string node_id_of(const std::filesystem::path& path) {
  const hm::json document = hm::read_json_file(path);
  for (const char* key : {"perception_node_id", "llm_node_id"}) {
    if (document.contains(key) && document.at(key).is_string()) return document.at(key).get<std::string>();
  }
  throw std::runtime_error(path.string() + " names no perception_node_id or llm_node_id (use --client-id)");
}

struct Answer {
  std::int64_t generated_at_ms = 0;
  std::int64_t received_steady_ms = 0;
  std::int64_t received_wall_ms = 0;
  bool core_clock_valid = false;
};

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path;
  std::filesystem::path config_dir;
  std::string client_id;
  std::int64_t max_skew_ms = 1000;
  bool dry_run = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--config-dir" && i + 1 < argc) {
      config_dir = argv[++i];
    } else if (arg == "--client-id" && i + 1 < argc) {
      client_id = argv[++i];
    } else if (arg == "--max-skew-ms" && i + 1 < argc) {
      max_skew_ms = std::stoll(argv[++i]);
    } else if (arg == "--dry-run") {
      dry_run = true;
    } else if (arg == "--log-level" && i + 1 < argc) {
      hm::set_log_level(hm::parse_log_level(argv[++i]));
    } else if (arg == "--help" || arg == "-h") {
      return usage(0);
    } else {
      std::cerr << "unrecognized argument: " << arg << "\n";
      return usage(2);
    }
  }
  if (config_path.empty() == config_dir.empty() || max_skew_ms < 0) return usage(2);
  try {
    if (config_path.empty()) config_path = node_config(config_dir);
    if (client_id.empty()) client_id = node_id_of(config_path);
    const hm::RuntimeConfig config = hm::RuntimeConfig::load(config_path);

    std::mutex mutex;
    std::condition_variable answered;
    std::string request_id;
    std::optional<Answer> answer;
    hm::Bus bus(config.broker, client_id + "-clock");
    bus.subscribe("pfn/v1/core/state/response/" + client_id, [&](const std::string&, const hm::json& document) {
      const std::int64_t steady = steady_ms();
      const std::int64_t wall = hm::now_ms();
      std::lock_guard lock(mutex);
      if (document.value("request_id", std::string()) != request_id || !document.contains("generated_at_ms") ||
          !document.at("generated_at_ms").is_number_integer()) {
        return;  // another request of this node, or not a usable answer
      }
      const hm::json clock = document.value("clock", hm::json::object());
      answer = Answer{document.at("generated_at_ms").get<std::int64_t>(), steady, wall,
                      clock.is_object() && clock.value("valid", false)};
      answered.notify_all();
    });
    if (!bus.start(10000)) {
      hm::log(hm::LogLevel::error, "clock", "broker not reachable: clock left as it is");
      return 1;
    }
    for (int attempt = 1; attempt <= 3; ++attempt) {
      std::int64_t sent_steady = 0;
      {
        std::lock_guard lock(mutex);
        request_id = hm::new_id();
        answer.reset();
        sent_steady = steady_ms();
      }
      bus.publish("pfn/v1/core/state/request", {{"schema_version", "pfn.v1"},
                                                 {"event_id", hm::new_id()},
                                                 {"trace_id", hm::new_id()},
                                                 {"request_id", request_id},
                                                 {"client_id", client_id},
                                                 {"sections", hm::json::array({"system_health"})},
                                                 {"sent_at_ms", hm::now_ms()}});
      std::unique_lock lock(mutex);
      if (!answered.wait_for(lock, std::chrono::seconds(5), [&] { return answer.has_value(); })) continue;
      const hm::clock::Decision decision = hm::clock::decide(
          {answer->generated_at_ms, answer->received_steady_ms - sent_steady, answer->received_wall_ms}, max_skew_ms,
          2000);
      hm::log(hm::LogLevel::info, "clock",
              decision.reason + " (core clock " + (answer->core_clock_valid ? "synced" : "not synced") + ")");
      if (decision.outcome == hm::clock::Outcome::too_slow) continue;  // ask again
      if (decision.outcome != hm::clock::Outcome::set_clock || dry_run) {
        bus.stop();
        return decision.outcome == hm::clock::Outcome::implausible ? 1 : 0;
      }
      // The answer arrived received_steady_ms ago: apply the offset to the clock as it reads now.
      const std::int64_t target_ms = hm::now_ms() + decision.offset_ms;
      const timespec target{static_cast<time_t>(target_ms / 1000), static_cast<long>((target_ms % 1000) * 1000000)};
      if (clock_settime(CLOCK_REALTIME, &target) != 0) {
        hm::log(hm::LogLevel::error, "clock", "clock_settime failed (needs CAP_SYS_TIME / root)");
        bus.stop();
        return 3;
      }
      hm::log(hm::LogLevel::info, "clock", "set to core time (" + std::string(decision.offset_ms > 0 ? "+" : "") +
                                               std::to_string(decision.offset_ms) + " ms)");
      bus.stop();
      return 0;
    }
    hm::log(hm::LogLevel::error, "clock", "no usable answer from core after 3 requests: clock left as it is");
    bus.stop();
    return 1;
  } catch (const std::exception& error) {
    hm::log(hm::LogLevel::error, "clock", error.what());
    return 2;
  }
}
