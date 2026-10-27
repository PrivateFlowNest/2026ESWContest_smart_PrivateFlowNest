// PC harness: homemind-perception plus test doubles, for native/tests/test_perception_*.py. Never shipped.
//   --context-replay FILE   fused events recorded from the Python reference (one per window)
//   --context-scripted      loud = meeting / quiet double instead of YAMNet + fusion
//   --stt-fake-text TEXT    fake whisper returning TEXT (repeat for a sequence; the last one repeats)
//   --stt-fake-fail         fake whisper that always fails (whisper down)
//   --decider-keywords      keyword decider instead of libs/hm_parser (automatic when hm_parser is missing)
//   --presence-replay FILE  scripted PRESENCE observations on the audio clock (doubles.hpp PresenceReplayEngine)
//   --fusion-trace FILE     JSONL of every window's fusion input/output (presence events, feature, fused event)
//   --partial-script FILE   JSON [[texts of utterance 1], ...]: scripted partial transcripts instead of Vosk
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "doubles.hpp"
#include "runner.hpp"

int main(int argc, char** argv) {
  using namespace hm::perception;
  std::vector<char*> rest = {argv[0]};
  std::string replay;
  std::string presence_replay;
  std::string fusion_trace;
  std::string partial_script;
  bool scripted = false;
  bool fake_fail = false;
  bool keywords = false;
  std::vector<std::string> texts;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--context-replay" && i + 1 < argc) {
      replay = argv[++i];
    } else if (arg == "--context-scripted") {
      scripted = true;
    } else if (arg == "--stt-fake-text" && i + 1 < argc) {
      texts.emplace_back(argv[++i]);
    } else if (arg == "--stt-fake-fail") {
      fake_fail = true;
    } else if (arg == "--decider-keywords") {
      keywords = true;
    } else if (arg == "--presence-replay" && i + 1 < argc) {
      presence_replay = argv[++i];
    } else if (arg == "--fusion-trace" && i + 1 < argc) {
      fusion_trace = argv[++i];
    } else if (arg == "--partial-script" && i + 1 < argc) {
      partial_script = argv[++i];
    } else {
      rest.push_back(argv[i]);
    }
  }
  Hooks hooks;
  hooks.program = "homemind-perception-harness";
  hooks.extra_usage =
      " [--context-replay FILE | --context-scripted] [--stt-fake-text TEXT ...] [--stt-fake-fail] [--decider-keywords]"
      " [--presence-replay FILE] [--fusion-trace FILE] [--partial-script FILE]";
  std::function<std::unique_ptr<ContextEngine>(const Settings&, bool)> base;
  if (!replay.empty()) {
    base = [replay](const Settings&, bool) { return std::make_unique<doubles::ReplayContextEngine>(replay); };
  } else if (scripted) {
    base = [](const Settings& settings, bool) {
      return std::make_unique<doubles::ScriptedContextEngine>(settings.presence_zone);
    };
  }
  if (base || !presence_replay.empty() || !fusion_trace.empty()) {
    hooks.context = [base, presence_replay, fusion_trace](const Settings& settings, bool simulated) {
      std::unique_ptr<ContextEngine> engine = base ? base(settings, simulated) : make_context_engine(settings, simulated);
      if (!presence_replay.empty()) {
        engine = std::make_unique<doubles::PresenceReplayEngine>(std::move(engine), presence_replay);
      }
      if (!fusion_trace.empty()) engine = std::make_unique<doubles::TracingContextEngine>(std::move(engine), fusion_trace);
      return engine;
    };
  }
  if (!texts.empty() || fake_fail) {
    hooks.transcriber = [texts, fake_fail](const Settings&) {
      return std::make_unique<doubles::FakeTranscriber>(texts, fake_fail);
    };
  }
  if (keywords || !voice_parser_available()) {
    if (!keywords) hm::log(hm::LogLevel::warning, "harness", "libs/hm_parser not linked: using the keyword decider double");
    hooks.parser = [](const hm::ActionMap& map, const Settings&, const hm::RuntimeConfig& config, bool simulated) {
      return std::make_unique<doubles::KeywordDecider>(map, config.perception_node_id, config.llm_request_ttl_ms, simulated);
    };
  }
  if (!partial_script.empty()) {
    const hm::json scripts = hm::read_json_file(partial_script);
    hooks.partial_transcriber = [scripts](const Settings&) {
      return std::make_unique<doubles::ScriptedPartialTranscriber>(
          scripts.get<std::vector<std::vector<std::string>>>());
    };
  }
  return run_cli(static_cast<int>(rest.size()), rest.data(), hooks);
}
