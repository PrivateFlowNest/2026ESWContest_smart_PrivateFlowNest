// Consumers, wiring and the command line of homemind-perception (perception.py context_loop, voice_loop,
// run_with, run, main).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engines.hpp"
#include "hm/audio/ptt.hpp"
#include "hm/audio/ring.hpp"
#include "partial_hints.hpp"
#include "service.hpp"
#include "voice_listen.hpp"

namespace hm::perception {

// Test/PC-harness injection points (nullptr / empty = real implementations).
struct Hooks {
  std::function<std::unique_ptr<ContextEngine>(const Settings&, bool simulated)> context;
  std::function<std::unique_ptr<SpeechToText>(const Settings&)> transcriber;
  std::function<std::unique_ptr<VoiceDecider>(const hm::ActionMap&, const Settings&, const hm::RuntimeConfig&, bool)>
      parser;
  std::function<std::unique_ptr<PartialTranscriber>(const Settings&)> partial_transcriber;
  std::function<std::unique_ptr<PartialIntentModel>(const Settings&)> partial_intent;
  std::string program = "homemind-perception";
  std::string extra_usage;
};

// 4 s windows every 1 s hop; lossy (microphone) skips ahead when it falls behind.
void context_loop(PerceptionService& service, hm::audio::AudioRing& ring, int consumer, bool lossy);
// mode: push_to_talk | always | whole. whole collects every sample into *whole (file mode, one utterance).
// partial (optional): the open utterance's audio is streamed to it while it is captured; each utterance gets a
// trace id shared with its final decision (whole mode: written to *whole_trace).
void voice_loop(PerceptionService& service, hm::audio::AudioRing& ring, int consumer, const std::string& mode,
                hm::audio::PttControl* control, std::vector<float>* whole = nullptr,
                PartialHintWorker* partial = nullptr, std::string* whole_trace = nullptr);

// Runs file mode to completion or microphone mode until SIGINT/SIGTERM. Returns the summary.
// remote (optional): pfn/v1/ai/voice/listen handler; attached to the push-to-talk control while it exists.
RunSummary run_with(const hm::RuntimeConfig& config, const Settings& settings, Publisher& publisher, const Hooks& hooks,
                    RemoteListen* remote = nullptr);
int run(const hm::RuntimeConfig& config, const Hooks& hooks);
int run_cli(int argc, char** argv, const Hooks& hooks = {});

void request_stop();  // what SIGINT/SIGTERM do
bool stop_requested();

}  // namespace hm::perception
