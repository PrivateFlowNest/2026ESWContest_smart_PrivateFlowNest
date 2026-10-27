#include "service.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <set>

#include "hm/audio/common.hpp"
#include "hm/mqtt.hpp"
#include "hm/pfn.hpp"

namespace hm::perception {

namespace {

std::mutex& stdout_mutex() {
  static std::mutex mutex;
  return mutex;
}

// Python str() of a JSON scalar (evidence items are strings in practice).
std::string py_str(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
  if (value.is_null()) return "None";
  return value.dump();
}

std::string text_of(const json& value) { return value.is_string() ? value.get<std::string>() : py_str(value); }

bool truthy(const json& value) {
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_null()) return false;
  if (value.is_number()) return value.get<double>() != 0.0;
  if (value.is_string()) return !value.get_ref<const std::string&>().empty();
  return !value.empty();
}

std::string format(const char* pattern, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* pattern, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, pattern);
  std::vsnprintf(buffer, sizeof buffer, pattern, args);
  va_end(args);
  return buffer;
}

std::string rstrip(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n')) text.pop_back();
  return text;
}

json value_or_null(const json& object, const char* key) {
  if (!object.is_object()) return nullptr;
  auto it = object.find(key);
  return it == object.end() ? json(nullptr) : *it;
}

}  // namespace

void echo_stdout(const std::string& line) {
  std::lock_guard lock(stdout_mutex());
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void StdoutPublisher::publish(const std::string& topic, const json& document) {
  echo_stdout(topic + " " + hm::dump_compact(document));
}

BusPublisher::BusPublisher(hm::Bus& bus, int hold_ms, std::size_t capacity)
    : BusPublisher([&bus] { return bus.connected(); },
                   [&bus](const std::string& topic, const json& document) { return bus.publish(topic, document, 1); },
                   hold_ms, capacity) {}

BusPublisher::BusPublisher(Connected connected, Send send, int hold_ms, std::size_t capacity)
    : connected_(std::move(connected)), send_(std::move(send)), hold_ms_(hold_ms), capacity_(capacity) {
  flusher_ = std::thread([this] {
    std::unique_lock lock(mutex_);
    while (!stop_) {
      wake_.wait_for(lock, std::chrono::milliseconds(200));
      if (!outbox_.empty()) flush_locked();
    }
  });
}

BusPublisher::~BusPublisher() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    if (!outbox_.empty()) {
      hm::log(hm::LogLevel::warning, "perception",
              std::to_string(outbox_.size()) + " message(s) not delivered (MQTT still down at shutdown)");
    }
  }
  wake_.notify_all();
  if (flusher_.joinable()) flusher_.join();
}

void BusPublisher::publish(const std::string& topic, const json& document) {
  std::lock_guard lock(mutex_);
  if (outbox_.empty() && connected_() && send_(topic, document)) return;
  std::int64_t deadline = hm::now_ms() + hold_ms_;
  if (topic == hm::pfn::kPerception && document.contains("attributes") &&
      document["attributes"].contains("valid_until_ms")) {
    deadline = document["attributes"]["valid_until_ms"].get<std::int64_t>();
  } else if (document.contains("expires_at_ms") && document["expires_at_ms"].is_number_integer()) {
    deadline = document["expires_at_ms"].get<std::int64_t>();
  }
  if (outbox_.size() >= capacity_) {
    ++dropped_;
    hm::log(hm::LogLevel::error, "perception", "MQTT outbox full; dropped the oldest " + outbox_.front().topic);
    outbox_.pop_front();
  }
  outbox_.push_back(Item{topic, document, deadline});
  if (!holding_logged_) {
    hm::log(hm::LogLevel::warning, "perception", "MQTT not connected: holding messages until the broker is back");
    holding_logged_ = true;
  }
}

void BusPublisher::flush_locked() {
  const std::int64_t now = hm::now_ms();
  while (!outbox_.empty()) {
    Item& item = outbox_.front();
    if (item.deadline_ms < now) {
      ++dropped_;
      hm::log(hm::LogLevel::warning, "perception", "dropped expired " + item.topic + " (MQTT was down)");
      outbox_.pop_front();
      continue;
    }
    if (!connected_() || !send_(item.topic, item.document)) return;
    outbox_.pop_front();
  }
  if (holding_logged_) {
    hm::log(hm::LogLevel::info, "perception", "MQTT back: held messages sent");
    holding_logged_ = false;
  }
}

void BusPublisher::flush() {
  std::lock_guard lock(mutex_);
  flush_locked();
}

std::size_t BusPublisher::pending() const {
  std::lock_guard lock(mutex_);
  return outbox_.size();
}

std::size_t BusPublisher::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

void CapturePublisher::publish(const std::string& topic, const json& document) {
  std::lock_guard lock(mutex_);
  messages_.emplace_back(topic, document);
}

std::vector<std::pair<std::string, json>> CapturePublisher::messages() const {
  std::lock_guard lock(mutex_);
  return messages_;
}

json context_event_to_perception(const json& event, const hm::ActionMap& action_map, const std::string& model_version,
                                 const std::optional<std::string>& policy_version, std::int64_t ttl_ms,
                                 const std::optional<std::string>& transition, bool simulated,
                                 const std::string& source_node_id) {
  std::vector<std::string> raw = {"AUDIO_CONTEXT:" + py_str(event.at("context_type")),
                                  "PRESENCE:" + py_str(event.at("presence_zone"))};
  if (policy_version && !policy_version->empty()) raw.push_back("policy:" + *policy_version);
  if (event.contains("evidence") && event["evidence"].is_array()) {
    for (const auto& item : event["evidence"]) {
      std::string text = py_str(item);
      if (!text.empty()) raw.push_back(std::move(text));
    }
  }
  json evidence = json::array();
  std::set<std::string> seen;
  for (const auto& item : raw) {  // list(dict.fromkeys(_clip(item) for item in evidence))[:16]
    std::string clipped = hm::audio::utf8_prefix(item, 64);
    if (seen.insert(clipped).second && evidence.size() < 16) evidence.push_back(std::move(clipped));
  }
  const json attributes = {
      {"activity", event.at("activity")},
      {"audio_context", event.at("context_type")},
      {"quiet_required", truthy(event.at("quiet_required"))},
      {"transition", transition ? json(*transition) : event.at("transition")},
      {"evidence", evidence},
      {"valid_until_ms", hm::now_ms() + ttl_ms},
      {"model_version", hm::audio::utf8_prefix(model_version, 64)},
  };
  return hm::pfn::perception({.observation_type = "ACTIVITY_CONTEXT",
                              .zone = action_map.zone(text_of(event.at("presence_zone"))),
                              .confidence = event.at("confidence").get<double>(),
                              .attributes = attributes,
                              .source_device_id = std::nullopt,
                              .trace_id = std::nullopt,
                              .simulated = simulated,
                              .source_node_id = source_node_id});
}

json sanitized_context(const json& event) {
  return {
      {"presence_zone", event.at("presence_zone")},
      {"activity", event.at("activity")},
      {"audio_context", event.at("context_type")},
      {"quiet_required", truthy(event.at("quiet_required"))},
      {"confidence", event.at("confidence").get<double>()},
  };
}

PerceptionService::PerceptionService(Settings settings, Publisher& publisher, const hm::ActionMap& action_map,
                                     ContextEngine* context, SpeechToText* transcriber, VoiceDecider* parser,
                                     bool simulated, std::function<void(const std::string&)> echo,
                                     std::string source_node_id)
    : settings_(std::move(settings)),
      publisher_(publisher),
      action_map_(action_map),
      context_(context),
      transcriber_(transcriber),
      parser_(parser),
      simulated_(simulated),
      echo_(echo ? std::move(echo) : std::function<void(const std::string&)>(echo_stdout)),
      source_node_id_(std::move(source_node_id)),
      base_time_ms_(hm::now_ms()) {}

std::int64_t PerceptionService::timestamp_ms(double audio_seconds) const {
  return base_time_ms_ + std::llround(audio_seconds * 1000.0);
}

double estimated_sound_db(double mean_square, double offset_db) {
  const double dbfs = 10.0 * std::log10(std::max(mean_square, 1e-12));
  return std::clamp(dbfs + offset_db, 0.0, 120.0);
}

std::string telemetry_topic(const std::string& node_id) { return "pfn/v1/device/" + node_id + "/telemetry"; }

json sound_level_telemetry(const std::string& node_id, std::int64_t seq, double sound_db, std::int64_t sent_at_ms,
                           std::int64_t uptime_ms, const std::string& trace_id) {
  return {{"schema_version", "pfn.v1"},
          {"event_id", hm::new_id()},
          {"trace_id", trace_id},
          {"node_id", node_id},
          {"seq", seq},
          {"sent_at_ms", sent_at_ms},
          {"uptime_ms", uptime_ms},
          {"sensor_ready", true},
          {"readings", {{"sound_level_db", hm::round_to(sound_db, 1)}}},
          {"reading_sources", {{"sound_level_db", kMicrophoneDeviceId}}}};
}

void PerceptionService::enable_sound_level(std::function<bool()> microphone_allowed) {
  microphone_allowed_ = std::move(microphone_allowed);
  sound_trace_id_ = hm::new_id();
}

void PerceptionService::sound_level_window(const float* window, std::size_t samples, double audio_time) {
  const auto hop = static_cast<std::size_t>(std::nearbyint(settings_.hop_seconds * kSampleRate));
  const std::size_t count = std::min(hop, samples);
  if (count == 0) return;
  double energy = 0.0;
  for (std::size_t index = samples - count; index < samples; ++index) energy += double(window[index]) * window[index];
  sound_energy_ += energy / static_cast<double>(count);
  ++sound_hops_;
  if (!sound_since_) sound_since_ = audio_time;
  if (audio_time - *sound_since_ < settings_.sound_level.interval_seconds) return;
  const double sound_db = estimated_sound_db(sound_energy_ / static_cast<double>(sound_hops_), settings_.sound_level.offset_db);
  sound_energy_ = 0.0;
  sound_hops_ = 0;
  sound_since_ = audio_time;
  if (!microphone_allowed_()) return;  // privacy: nothing leaves the node, not even the number
  publisher_.publish(telemetry_topic(source_node_id_),
                     sound_level_telemetry(source_node_id_, sound_seq_++, sound_db, hm::now_ms(),
                                           std::llround(audio_time * 1000.0), sound_trace_id_));
  hm::log(hm::LogLevel::debug, "perception", format("sound level t=%.1fs %.1f dB (estimate)", audio_time, sound_db));
}

void PerceptionService::process_window(const float* window, std::size_t samples, std::int64_t end_sample) {
  const double audio_time = static_cast<double>(end_sample) / kSampleRate;
  if (microphone_allowed_) sound_level_window(window, samples, audio_time);
  if (!context_) return;  // context path disabled (built without libs/hm_context); the sound level still runs
  const hm::ContextUpdate update = context_->process(window, samples, timestamp_ms(audio_time), audio_time);
  {
    std::lock_guard lock(mutex_);
    ++summary_.windows;
  }
  const json& event = update.context_event;
  hm::log(hm::LogLevel::debug, "perception",
          format("window t=%.1fs raw=%s(%.2f) speech=%.2f fused=%s/%s %s %.0fms", audio_time, update.raw_label.c_str(),
                 update.raw_confidence, update.speech_ratio, text_of(value_or_null(event, "context_type")).c_str(),
                 truthy(value_or_null(event, "quiet_required")) ? "True" : "False",
                 text_of(value_or_null(event, "transition")).c_str(), update.latency_ms));
  on_context_event(event, audio_time);
}

std::optional<json> PerceptionService::on_context_event(const json& event, double audio_time) {
  const std::string transition = text_of(event.at("transition"));
  bool due = false;
  {
    std::lock_guard lock(mutex_);
    history_.emplace_back(audio_time, event);
    // Keep what the TTL can still reach (plus slack for an utterance that waits on STT).
    const double horizon = settings_.perception_ttl_seconds + 120.0;
    while (history_.size() > 2 && (history_.size() > 512 || audio_time - history_.front().first > horizon)) {
      history_.pop_front();
    }
    due = transition != "UNCHANGED" || !last_publish_audio_time_ ||
          audio_time - *last_publish_audio_time_ >= settings_.republish_seconds;
    if (due) last_publish_audio_time_ = audio_time;
  }
  if (!due) return std::nullopt;
  std::string model_version = context_ ? context_->model_version() : "";
  if (model_version.empty()) model_version = text_of(value_or_null(event, "model_version"));
  const std::optional<std::string> policy = context_ ? context_->policy_version() : std::nullopt;
  const auto ttl_ms = static_cast<std::int64_t>(settings_.perception_ttl_seconds * 1000.0);  // int() truncation
  json document = context_event_to_perception(event, action_map_, model_version, policy, ttl_ms, std::nullopt,
                                              simulated_, source_node_id_);
  publisher_.publish(hm::pfn::kPerception, document);
  const json& attributes = document["attributes"];
  json record = {{"t", hm::round_to(audio_time, 1)},
                 {"activity", attributes["activity"]},
                 {"audio_context", attributes["audio_context"]},
                 {"quiet_required", attributes["quiet_required"]},
                 {"transition", attributes["transition"]},
                 {"confidence", document["confidence"]},
                 {"zone", document["zone"]}};
  hm::log(hm::LogLevel::info, "perception",
          format("perception t=%.1fs %s/%s quiet=%s %s conf=%.2f zone=%s", audio_time,
                 text_of(record["activity"]).c_str(), text_of(record["audio_context"]).c_str(),
                 record["quiet_required"].get<bool>() ? "True" : "False", text_of(record["transition"]).c_str(),
                 record["confidence"].get<double>(), text_of(record["zone"]).c_str()));
  {
    std::lock_guard lock(mutex_);
    summary_.perceptions.push_back(std::move(record));
  }
  return document;
}

std::optional<json> PerceptionService::latest_context(std::optional<double> audio_time, bool as_of) const {
  std::lock_guard lock(mutex_);
  if (history_.empty()) return std::nullopt;
  const std::pair<double, json>* chosen = &history_.back();
  if (as_of && audio_time) {
    chosen = nullptr;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
      if (it->first <= *audio_time + 1e-9) {
        chosen = &*it;
        break;
      }
    }
    if (!chosen) return std::nullopt;
  }
  if (audio_time && *audio_time - chosen->first > settings_.perception_ttl_seconds) return std::nullopt;
  return sanitized_context(chosen->second);
}

void PerceptionService::context_progress(std::int64_t consumed_sample) {
  {
    std::lock_guard lock(progress_mutex_);
    context_consumed_ = std::max(context_consumed_, consumed_sample);
  }
  progress_cv_.notify_all();
}

void PerceptionService::context_finished() {
  {
    std::lock_guard lock(progress_mutex_);
    context_done_ = true;
  }
  progress_cv_.notify_all();
}

void PerceptionService::wait_for_context(std::int64_t sample) const {
  std::unique_lock lock(progress_mutex_);
  progress_cv_.wait(lock, [&] { return context_done_ || context_consumed_ >= sample; });
}

std::optional<json> PerceptionService::on_utterance(const std::vector<float>& samples, std::optional<double> audio_time,
                                                    std::optional<std::string> trace_id) {
  if (!transcriber_ || !parser_) return std::nullopt;
  const double seconds = static_cast<double>(samples.size()) / kSampleRate;
  hm::stt::SttResult result;
  try {
    result = transcriber_->transcribe(samples);
  } catch (const std::exception& error) {  // one failed STT call must not stop the node
    hm::log(hm::LogLevel::error, "perception", format("whisper failed for a %.1fs utterance: ", seconds) + error.what());
    return std::nullopt;
  }
  if (settings_.debug_transcripts) {
    hm::log(hm::LogLevel::info, "perception", "transcript (debug_transcripts) '" + result.text + "'");
  }
  json record = {{"utterance_seconds", hm::round_to(seconds, 2)},
                 {"stt_ms", static_cast<std::int64_t>(std::nearbyint(result.latency_ms))},
                 {"chars", hm::audio::utf8_length(result.text)}};
  if (result.text.empty()) {
    record["decision"] = "empty";
    {
      std::lock_guard lock(mutex_);
      summary_.voice.push_back(record);
    }
    echo_(format("[voice] %.1fs, STT %.0f ms → 인식된 문장 없음", seconds, result.latency_ms));
    return record;
  }
  if (sync_context_ && audio_time) wait_for_context(std::llround(*audio_time * kSampleRate));
  const std::optional<json> context = latest_context(audio_time, sync_context_);
  hm::VoiceDecision decision;
  try {
    decision = parser_->handle(result.text, context ? *context : json(nullptr), result.model_version, std::nullopt,
                               hm::new_id(), std::move(trace_id));
  } catch (const std::exception& error) {
    hm::log(hm::LogLevel::error, "perception", std::string("voice parser produced an invalid message: ") + error.what());
    return std::nullopt;
  }
  json topics = json::array();
  for (const auto& [topic, message] : decision.messages) {
    publisher_.publish(topic, message);
    topics.push_back(topic);
  }
  const json device = value_or_null(decision.proposal, "device");
  const json action = value_or_null(decision.proposal, "action");
  record["decision"] = decision.kind;
  record["device"] = device;
  record["action"] = action;
  record["topics"] = topics;
  record["context"] = context ? (*context)["audio_context"] : json(nullptr);
  {
    std::lock_guard lock(mutex_);
    summary_.voice.push_back(record);
  }
  hm::log(hm::LogLevel::info, "perception",
          format("voice %.1fs stt=%.0fms chars=%d decision=%s %s/%s", seconds, result.latency_ms,
                 static_cast<int>(hm::audio::utf8_length(result.text)), decision.kind.c_str(), py_str(device).c_str(),
                 py_str(action).c_str()));
  const std::string shown = settings_.debug_transcripts ? " \"" + result.text + "\"" : "";
  echo_(rstrip(format("[voice] %.1fs, STT %.0f ms", seconds, result.latency_ms) + shown + " → " + decision.kind + " " +
               (truthy(device) ? text_of(device) : "") + " " + (truthy(action) ? text_of(action) : "") + " " +
               decision.note));
  return record;
}

RunSummary PerceptionService::summary() const {
  std::lock_guard lock(mutex_);
  return summary_;
}

}  // namespace hm::perception
