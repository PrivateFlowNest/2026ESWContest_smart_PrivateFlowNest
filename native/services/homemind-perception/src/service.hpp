// NODE-RPI5-02 perception service state, shared by both consumers. Port of services/runtime perception.py
// PerceptionService: ACTIVITY_CONTEXT on every transition and every republish_seconds (audio time),
// valid_until_ms = publish time + perception_ttl_seconds, sanitized context handed to the voice parser
// (only while younger than the TTL), voice decisions published in order.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engines.hpp"
#include "hm/action_map.hpp"
#include "hm/common.hpp"
#include "settings.hpp"

namespace hm {
class Bus;
}

namespace hm::perception {

class Publisher {
 public:
  virtual ~Publisher() = default;
  virtual void publish(const std::string& topic, const json& document) = 0;
};

// "<topic> <compact json>" lines (Python StdoutPublisher). Shares the stdout lock with echo().
class StdoutPublisher final : public Publisher {
 public:
  void publish(const std::string& topic, const json& document) override;
};

// MQTT publisher with a small outbox. hm::Bus (libmosquitto) refuses QoS1 publishes while disconnected
// (MOSQ_ERR_NO_CONN) where the Python Bus (paho) queues them, so messages are held here and sent in order after
// the reconnect while still valid: perception until attributes.valid_until_ms, llm/request until expires_at_ms,
// anything else (core task requests) for hold_ms. Expired or overflowing messages are dropped with a log line.
class BusPublisher final : public Publisher {
 public:
  using Connected = std::function<bool()>;
  using Send = std::function<bool(const std::string& topic, const json& document)>;

  explicit BusPublisher(hm::Bus& bus, int hold_ms = 10000, std::size_t capacity = 256);
  BusPublisher(Connected connected, Send send, int hold_ms = 10000, std::size_t capacity = 256);
  ~BusPublisher() override;
  void publish(const std::string& topic, const json& document) override;
  std::size_t pending() const;
  std::size_t dropped() const;
  void flush();  // one flush pass (also done every 200 ms by the flusher thread)

 private:
  struct Item {
    std::string topic;
    json document;
    std::int64_t deadline_ms;
  };
  void flush_locked();

  Connected connected_;
  Send send_;
  int hold_ms_;
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Item> outbox_;
  std::size_t dropped_ = 0;
  bool holding_logged_ = false;
  bool stop_ = false;
  std::thread flusher_;
};

class CapturePublisher final : public Publisher {
 public:
  void publish(const std::string& topic, const json& document) override;
  std::vector<std::pair<std::string, json>> messages() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<std::string, json>> messages_;
};

// One line on stdout (console feedback, Python echo = print).
void echo_stdout(const std::string& line);

// Fused ACTIVITY_CONTEXT (rpi5-internal) -> pfn.v1 perception document (context_event_to_perception).
json context_event_to_perception(const json& event, const hm::ActionMap& action_map, const std::string& model_version,
                                 const std::optional<std::string>& policy_version, std::int64_t ttl_ms,
                                 const std::optional<std::string>& transition, bool simulated,
                                 const std::string& source_node_id = "NODE-RPI5-02");
// {presence_zone, activity, audio_context, quiet_required, confidence} (activity-context projection).
json sanitized_context(const json& event);

// Sound level telemetry (user 2026-10-25): an estimate without calibration, mean square -> dBFS + offset_db, clamped
// to 0..120 dB.
double estimated_sound_db(double mean_square, double offset_db);
inline constexpr const char* kMicrophoneDeviceId = "MIC-INDOOR-01";
std::string telemetry_topic(const std::string& node_id);  // pfn/v1/device/<node>/telemetry
// telemetry.schema.json document with MIC-INDOOR-01 sound_level_db (sensor-rules.json: number 0..120).
json sound_level_telemetry(const std::string& node_id, std::int64_t seq, double sound_db, std::int64_t sent_at_ms,
                           std::int64_t uptime_ms, const std::string& trace_id);

struct RunSummary {
  int windows = 0;
  json perceptions = json::array();
  json voice = json::array();
  json partial_intent;  // PartialHintWorker::summary() when partial_intent is enabled, else null
  json presence;        // live presence counters when presence_fifo is set, else null
};

class PerceptionService {
 public:
  PerceptionService(Settings settings, Publisher& publisher, const hm::ActionMap& action_map, ContextEngine* context,
                    SpeechToText* transcriber, VoiceDecider* parser, bool simulated,
                    std::function<void(const std::string&)> echo = echo_stdout,
                    std::string source_node_id = "NODE-RPI5-02");

  const Settings& settings() const { return settings_; }
  ContextEngine* context_engine() const { return context_; }
  SpeechToText* transcriber() const { return transcriber_; }
  VoiceDecider* parser() const { return parser_; }
  void set_base_time_ms(std::int64_t value) { base_time_ms_ = value; }
  void echo(const std::string& line) const { echo_(line); }
  std::int64_t timestamp_ms(double audio_seconds) const;

  // ---- context path
  void process_window(const float* window, std::size_t samples, std::int64_t end_sample);
  std::optional<json> on_context_event(const json& event, double audio_time);
  // as_of=false: the newest event (Python). as_of=true: the newest event at or before audio_time.
  std::optional<json> latest_context(std::optional<double> audio_time, bool as_of = false) const;

  // ---- voice path
  // trace_id: the utterance trace shared with its partial-intent hints (nullopt: the parser makes one).
  std::optional<json> on_utterance(const std::vector<float>& samples, std::optional<double> audio_time,
                                   std::optional<std::string> trace_id = std::nullopt);

  // File mode determinism: the voice path waits until the context path consumed audio up to the utterance
  // end before it reads the context (Python's --fast threads race here).
  void enable_context_sync() { sync_context_ = true; }
  void context_progress(std::int64_t consumed_sample);
  void context_finished();

  RunSummary summary() const;

  // ---- sound level (live microphone only): energy of the newest hop of every window, published every
  // settings.sound_level.interval_seconds (audio time) while microphone_allowed() says yes (privacy/state).
  void enable_sound_level(std::function<bool()> microphone_allowed);

 private:
  void wait_for_context(std::int64_t sample) const;
  void sound_level_window(const float* window, std::size_t samples, double audio_time);

  Settings settings_;
  Publisher& publisher_;
  const hm::ActionMap& action_map_;
  ContextEngine* context_;
  SpeechToText* transcriber_;
  VoiceDecider* parser_;
  bool simulated_;
  std::function<void(const std::string&)> echo_;
  std::string source_node_id_;
  std::int64_t base_time_ms_;

  mutable std::mutex mutex_;
  std::deque<std::pair<double, json>> history_;  // (audio_time, fused event), newest last
  std::optional<double> last_publish_audio_time_;
  RunSummary summary_;

  // Context thread only (process_window).
  std::function<bool()> microphone_allowed_;  // empty: sound level off
  double sound_energy_ = 0.0;
  std::size_t sound_hops_ = 0;
  std::optional<double> sound_since_;
  std::int64_t sound_seq_ = 0;
  std::string sound_trace_id_;

  bool sync_context_ = false;
  mutable std::mutex progress_mutex_;
  mutable std::condition_variable progress_cv_;
  std::int64_t context_consumed_ = 0;
  bool context_done_ = false;
};

}  // namespace hm::perception
