// homemind-voice-text: NODE-RPI5-02 keyboard fallback (port of `homemind-runtime voice-text`, cli.run_voice_text).
// Each stdin line goes through the deterministic voice parser; task/llm requests are published on MQTT with the
// latest ACTIVITY_CONTEXT (pfn/v1/ai/perception) as context.
//   homemind-voice-text --config runtime.json [--log-level INFO] [--dry-run]
// --dry-run: no MQTT; each message is printed as "PUBLISH <topic> <json>" instead (no context then).
// Config: the services/runtime keys (broker, perception_node_id, llm_request_ttl_ms, simulated, contracts_dir) plus
// optional "capabilities_path". A line the Python version would crash on is logged and skipped.
#include <iostream>
#include <memory>
#include <string>

#include "context_tracker.hpp"
#include "hm/config.hpp"
#include "hm/mqtt.hpp"
#include "hm/pfn.hpp"
#include "hm/text.hpp"
#include "hm/validation.hpp"
#include "hm/voice_parser.hpp"

namespace {

int usage(int code) {
  std::cerr << "usage: homemind-voice-text [--config FILE] [--log-level LEVEL] [--dry-run]\n";
  return code;
}

std::string text_or_empty(const hm::json& proposal, const char* key) {
  if (!proposal.is_object()) return {};
  const hm::json value = proposal.value(key, hm::json(nullptr));
  return value.is_string() ? value.get<std::string>() : std::string();
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  bool dry_run = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--log-level" && i + 1 < argc) {
      hm::set_log_level(hm::parse_log_level(argv[++i]));
    } else if (arg == "--dry-run") {
      dry_run = true;
    } else if (arg == "--help" || arg == "-h") {
      return usage(0);
    } else {
      std::cerr << "unrecognized argument: " << arg << "\n";
      return usage(2);
    }
  }
  try {
    const hm::RuntimeConfig config = hm::RuntimeConfig::load(config_path);
    const hm::ActionMap action_map(config.contracts_dir);
    hm::VoiceParserOptions options;
    options.capabilities_path = config.extra.contains("capabilities_path") && config.extra["capabilities_path"].is_string()
                                    ? config.resolve_path(config.extra["capabilities_path"].get<std::string>())
                                    : hm::guard::default_capabilities_path();
    options.node_id = config.perception_node_id;
    options.llm_ttl_ms = config.llm_request_ttl_ms;
    options.simulated = config.simulated;
    options.contracts_dir = config.contracts_dir;
    hm::VoiceParser parser(action_map, options);
    hm::ContextTracker tracker;

    std::unique_ptr<hm::Bus> bus;
    if (!dry_run) {
      bus = std::make_unique<hm::Bus>(config.broker, config.perception_node_id + "-voice-text");
      bus->subscribe(hm::pfn::kPerception, [&](const std::string& topic, const hm::json& document) {
        tracker.update(topic, document);
      });
      bus->start();
    }
    std::cout << "문장을 입력하세요 (빈 줄 무시, Ctrl+C 종료)" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
      hm::VoiceDecision decision;
      try {
        decision = parser.handle(line, tracker.current());
      } catch (const std::exception& error) {
        hm::log(hm::LogLevel::error, "voice-text", std::string("line rejected: ") + error.what());
        continue;
      }
      for (const auto& [topic, message] : decision.messages) {
        if (bus) {
          bus->publish(topic, message);
        } else {
          std::cout << "PUBLISH " << topic << ' ' << hm::dump_compact(message) << '\n';
        }
      }
      std::cout << hm::text::strip("→ " + decision.kind + " " + text_or_empty(decision.proposal, "device") + " " +
                                   text_or_empty(decision.proposal, "action") + " " + decision.note)
                << std::endl;
    }
    if (bus) bus->stop();
    return 0;
  } catch (const std::exception& error) {
    hm::log(hm::LogLevel::error, "voice-text", error.what());
    return 1;
  }
}
