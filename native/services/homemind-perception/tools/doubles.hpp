// Test doubles for homemind-perception (gtests and the PC harness only; never in the shipped binary).
#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engines.hpp"

namespace hm::perception::doubles {

// Replays fused ACTIVITY_CONTEXT events recorded from the Python reference, one per window.
// File: JSONL; first line {"model_version":..., "policy_version":...}, then {"t": elapsed_seconds, "event": {...}}.
class ReplayContextEngine final : public ContextEngine {
 public:
  explicit ReplayContextEngine(const std::filesystem::path& path);
  ~ReplayContextEngine() override;  // prints "REPLAY {...}" (consumed, elapsed mismatches, remaining) to stderr
  hm::ContextUpdate process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                            double elapsed_seconds) override;
  std::string model_version() const override { return model_version_; }
  std::optional<std::string> policy_version() const override { return policy_version_; }

 private:
  std::vector<std::pair<double, json>> events_;
  std::size_t next_ = 0;
  int mismatches_ = 0;
  std::string model_version_;
  std::optional<std::string> policy_version_;
};

// Deterministic stand-in for YAMNet + fusion: loud windows (RMS > 0.05) are a meeting (quiet required),
// quiet windows are QUIET; ACTIVATED / DEACTIVATED on changes, UNCHANGED otherwise. Live presence: the zone of
// the newest submitted PRESENCE (confidence >= 0.5) replaces the configured zone (no TTL, test double).
class ScriptedContextEngine final : public ContextEngine {
 public:
  explicit ScriptedContextEngine(std::string presence_zone = "UNKNOWN") : zone_(std::move(presence_zone)) {}
  hm::ContextUpdate process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                            double elapsed_seconds) override;
  std::string model_version() const override { return "scripted-context-double"; }
  std::optional<std::string> policy_version() const override { return "scripted-policy"; }
  void submit_presence(const json& event) override;  // throws unless event_type PRESENCE with a payload zone
  std::vector<double> elapsed;  // every window's elapsed_seconds
  std::size_t presence_submitted() const;

 private:
  mutable std::mutex mutex_;
  std::string zone_;
  std::size_t presence_ = 0;
  bool quiet_ = false;
  bool first_ = true;
};

// Wraps a context engine: writes every window's fusion input and output as JSONL (PC parity tests):
//   {"t": elapsed, "timestamp_ms": .., "presence": [...], "feature": {...}, "context": {...}}
class TracingContextEngine final : public ContextEngine {
 public:
  TracingContextEngine(std::unique_ptr<ContextEngine> inner, const std::filesystem::path& path);
  void warmup() override { inner_->warmup(); }
  hm::ContextUpdate process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                            double elapsed_seconds) override;
  std::string model_version() const override { return inner_->model_version(); }
  std::optional<std::string> policy_version() const override { return inner_->policy_version(); }
  void submit_presence(const json& event) override { inner_->submit_presence(event); }

 private:
  std::unique_ptr<ContextEngine> inner_;
  std::ofstream out_;
};

// Submits scripted PRESENCE observations on the audio clock (deterministic --fast runs): JSONL items
//   {"at": s, "zone": "DESK", "confidence": 0.9, "ttl": 15, "offset": 0, "simulated": true, "repeat_previous": false}
// are stamped timestamp = stream start + (at + offset) s, valid_until = timestamp + ttl, and submitted right before
// the first window whose elapsed time is >= at. repeat_previous re-sends the previous event (same event_id).
class PresenceReplayEngine final : public ContextEngine {
 public:
  PresenceReplayEngine(std::unique_ptr<ContextEngine> inner, const std::filesystem::path& script);
  void warmup() override { inner_->warmup(); }
  hm::ContextUpdate process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                            double elapsed_seconds) override;
  std::string model_version() const override { return inner_->model_version(); }
  std::optional<std::string> policy_version() const override { return inner_->policy_version(); }
  void submit_presence(const json& event) override { inner_->submit_presence(event); }

 private:
  std::unique_ptr<ContextEngine> inner_;
  std::vector<json> items_;
  std::size_t next_ = 0;
  json previous_;
};

// Partial transcripts double: every accept() call of an utterance returns the next scripted text (an empty string
// = no new partial). Utterance k uses script k (the last script repeats).
class ScriptedPartialTranscriber final : public PartialTranscriber {
 public:
  explicit ScriptedPartialTranscriber(std::vector<std::vector<std::string>> scripts) : scripts_(std::move(scripts)) {}
  std::unique_ptr<PartialSession> start() override;
  std::string model_version() const override { return "scripted-partials"; }
  std::size_t sessions() const { return started_; }

 private:
  std::vector<std::vector<std::string>> scripts_;
  std::atomic<std::size_t> started_{0};
};

// Returns the configured texts in order (the last one repeats); fail=true throws like a down whisper.
class FakeTranscriber final : public SpeechToText {
 public:
  explicit FakeTranscriber(std::vector<std::string> texts, bool fail = false) : failing(fail), texts_(std::move(texts)) {}
  hm::stt::SttResult transcribe(const std::vector<float>& samples) override;
  std::atomic<bool> failing;
  std::vector<std::size_t> seen() const;  // sample counts of the utterances

 private:
  std::vector<std::string> texts_;
  mutable std::mutex mutex_;
  std::vector<std::size_t> seen_;
  std::size_t next_ = 0;
};

// A few fixed phrases -> core task requests (via the real ActionMap), everything else -> llm/request.
// Stand-in until libs/hm_parser is available; not the real parser rules.
class KeywordDecider final : public VoiceDecider {
 public:
  KeywordDecider(const hm::ActionMap& action_map, std::string node_id, int llm_ttl_ms, bool simulated)
      : action_map_(action_map), node_id_(std::move(node_id)), llm_ttl_ms_(llm_ttl_ms), simulated_(simulated) {}
  hm::VoiceDecision handle(const std::string& text, const json& context, const std::string& stt_model_version,
                           std::optional<double> stt_confidence, std::optional<std::string> utterance_id,
                           std::optional<std::string> trace_id) override;
  std::vector<json> contexts;  // context seen per call

 private:
  const hm::ActionMap& action_map_;
  std::string node_id_;
  int llm_ttl_ms_;
  bool simulated_;
};

}  // namespace hm::perception::doubles
