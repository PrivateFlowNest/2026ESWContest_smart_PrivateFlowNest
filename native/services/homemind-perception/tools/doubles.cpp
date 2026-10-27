#include "doubles.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <stdexcept>

#include "hm/common.hpp"
#include "hm/pfn.hpp"

namespace hm::perception::doubles {

ReplayContextEngine::ReplayContextEngine(const std::filesystem::path& path) {
  const std::string text = hm::read_text_file(path);
  std::size_t start = 0;
  bool header = true;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
    const json item = json::parse(line);
    if (header) {
      model_version_ = item.value("model_version", std::string("replay"));
      if (item.contains("policy_version") && item["policy_version"].is_string()) {
        policy_version_ = item["policy_version"].get<std::string>();
      }
      header = false;
      continue;
    }
    events_.emplace_back(item.at("t").get<double>(), item.at("event"));
  }
}

ReplayContextEngine::~ReplayContextEngine() {
  const json report = {{"consumed", next_}, {"elapsed_mismatches", mismatches_}, {"remaining", events_.size() - next_}};
  std::fprintf(stderr, "REPLAY %s\n", hm::dump_compact(report).c_str());
}

hm::ContextUpdate ReplayContextEngine::process(const float*, std::size_t, std::int64_t, double elapsed_seconds) {
  if (next_ >= events_.size()) throw std::runtime_error("context replay exhausted");
  const auto& [t, event] = events_[next_++];
  if (std::fabs(t - elapsed_seconds) > 1e-6) {
    ++mismatches_;
    hm::log(hm::LogLevel::warning, "replay",
            "window elapsed " + std::to_string(elapsed_seconds) + " != recorded " + std::to_string(t));
  }
  hm::ContextUpdate update;
  update.raw_label = event.value("context_type", std::string("UNKNOWN"));
  update.raw_confidence = event.value("confidence", 0.0);
  update.context_event = event;
  return update;
}

void ScriptedContextEngine::submit_presence(const json& event) {
  if (!event.is_object() || event.value("event_type", "") != "PRESENCE" || !event.contains("payload") ||
      !event["payload"].contains("presence_zone") || !event["payload"]["presence_zone"].is_string()) {
    throw std::invalid_argument("not a PRESENCE observation");
  }
  std::lock_guard lock(mutex_);
  ++presence_;
  if (event["payload"].value("confidence", 0.0) >= 0.5) zone_ = event["payload"]["presence_zone"].get<std::string>();
}

std::size_t ScriptedContextEngine::presence_submitted() const {
  std::lock_guard lock(mutex_);
  return presence_;
}

hm::ContextUpdate ScriptedContextEngine::process(const float* window, std::size_t samples, std::int64_t,
                                                 double elapsed_seconds) {
  std::string zone;
  {
    std::lock_guard lock(mutex_);
    zone = zone_;
  }
  double energy = 0.0;
  for (std::size_t i = 0; i < samples; ++i) energy += static_cast<double>(window[i]) * window[i];
  const bool loud = samples > 0 && std::sqrt(energy / static_cast<double>(samples)) > 0.05;
  std::string transition = "UNCHANGED";
  if (loud && (first_ || !quiet_)) {
    transition = "ACTIVATED";
  } else if (!loud && !first_ && quiet_) {
    transition = "DEACTIVATED";
  }
  quiet_ = loud;
  first_ = false;
  elapsed.push_back(elapsed_seconds);
  hm::ContextUpdate update;
  update.raw_label = loud ? "ZOOM_SPEAKER" : "QUIET";
  update.raw_confidence = loud ? 0.9 : 0.6;
  update.speech_ratio = loud ? 0.7 : 0.0;
  update.context_event = {
      {"schema_version", "1.0"},
      {"event_id", "ctx-" + hm::new_id()},
      {"trace_id", "trace-double"},
      {"timestamp", "2026-10-22T00:00:00.000Z"},
      {"source", "pi5.context-fusion"},
      {"source_node_id", "NODE-RPI5-02"},
      {"event_type", "ACTIVITY_CONTEXT"},
      {"context_type", loud ? "ZOOM_SPEAKER" : "QUIET"},
      {"presence_zone", zone},
      {"activity", loud ? "MEETING" : "UNKNOWN"},
      {"quiet_required", loud},
      {"confidence", loud ? 0.87 : 0.6},
      {"evidence", json::array({std::string("audio:") + (loud ? "ZOOM_SPEAKER:0.900" : "QUIET:0.600")})},
      {"transition", transition},
      {"valid_until", "2026-10-22T00:00:10.000Z"},
      {"model_version", "context-fusion-rules-v1"},
  };
  return update;
}

TracingContextEngine::TracingContextEngine(std::unique_ptr<ContextEngine> inner, const std::filesystem::path& path)
    : inner_(std::move(inner)), out_(path) {
  if (!out_) throw std::runtime_error("cannot write the fusion trace " + path.string());
}

hm::ContextUpdate TracingContextEngine::process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                                                double elapsed_seconds) {
  hm::ContextUpdate update = inner_->process(window, samples, timestamp_ms, elapsed_seconds);
  const json line = {{"t", elapsed_seconds}, {"timestamp_ms", timestamp_ms}, {"presence", update.presence_events},
                     {"feature", update.feature_event}, {"context", update.context_event}};
  out_ << hm::dump_compact(line) << '\n';
  out_.flush();
  return update;
}

namespace {
// UTC "YYYY-MM-DDTHH:MM:SS.mmmZ" (homemind-vision's presence timestamps).
std::string iso_ms(std::int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char text[96];
  std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(epoch_ms % 1000));
  return text;
}
}  // namespace

PresenceReplayEngine::PresenceReplayEngine(std::unique_ptr<ContextEngine> inner, const std::filesystem::path& script)
    : inner_(std::move(inner)) {
  const std::string text = hm::read_text_file(script);
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.find_first_not_of(" \r\t") != std::string::npos) items_.push_back(json::parse(line));
  }
}

hm::ContextUpdate PresenceReplayEngine::process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                                                double elapsed_seconds) {
  const std::int64_t base_ms = timestamp_ms - std::llround(elapsed_seconds * 1000.0);
  while (next_ < items_.size() && items_[next_].at("at").get<double>() <= elapsed_seconds + 1e-9) {
    const json& item = items_[next_++];
    json event;
    if (item.value("repeat_previous", false) && previous_.is_object()) {
      event = previous_;
    } else {
      const double at = item.at("at").get<double>() + item.value("offset", 0.0);
      const std::int64_t stamp = base_ms + std::llround(at * 1000.0);
      const std::int64_t ttl_ms = std::llround(item.value("ttl", 15.0) * 1000.0);
      event = {{"schema_version", "1.0"},
               {"event_id", hm::new_id()},
               {"trace_id", hm::new_id()},
               {"timestamp", iso_ms(stamp)},
               {"valid_until", iso_ms(stamp + ttl_ms)},
               {"source", "pi5.vision-presence"},
               {"source_node_id", "NODE-RPI5-02"},
               {"event_type", "PRESENCE"},
               {"simulated", item.value("simulated", true)},
               {"payload", {{"presence_zone", item.value("zone", "UNKNOWN")}, {"confidence", item.value("confidence", 0.9)}}}};
      if (item.contains("raw")) event = item["raw"];
    }
    previous_ = event;
    try {
      inner_->submit_presence(event);
    } catch (const std::exception& error) {
      hm::log(hm::LogLevel::info, "replay", std::string("presence item rejected: ") + error.what());
    }
  }
  return inner_->process(window, samples, timestamp_ms, elapsed_seconds);
}

namespace {
class ScriptedPartialSession final : public PartialSession {
 public:
  explicit ScriptedPartialSession(std::vector<std::string> script) : script_(std::move(script)) {}
  std::vector<std::string> accept(const float*, std::size_t) override {
    if (next_ >= script_.size()) return {};
    std::string text = script_[next_++];
    if (text.empty()) return {};
    return {text};
  }
  std::vector<std::string> flush() override { return {}; }

 private:
  std::vector<std::string> script_;
  std::size_t next_ = 0;
};
}  // namespace

std::unique_ptr<PartialSession> ScriptedPartialTranscriber::start() {
  const std::size_t index = started_++;
  if (scripts_.empty()) return std::make_unique<ScriptedPartialSession>(std::vector<std::string>{});
  return std::make_unique<ScriptedPartialSession>(scripts_[std::min(index, scripts_.size() - 1)]);
}

hm::stt::SttResult FakeTranscriber::transcribe(const std::vector<float>& samples) {
  std::lock_guard lock(mutex_);
  seen_.push_back(samples.size());
  if (failing) throw std::runtime_error("whisper-server http://127.0.0.1:9/inference: connection refused (double)");
  hm::stt::SttResult result;
  result.text = texts_.empty() ? "" : texts_[std::min(next_, texts_.size() - 1)];
  ++next_;
  result.raw_length = result.text.size();
  result.model_version = "fake-whisper";
  result.backend = "fake";
  return result;
}

std::vector<std::size_t> FakeTranscriber::seen() const {
  std::lock_guard lock(mutex_);
  return seen_;
}

hm::VoiceDecision KeywordDecider::handle(const std::string& text, const json& context,
                                         const std::string& stt_model_version, std::optional<double> stt_confidence,
                                         std::optional<std::string> utterance_id, std::optional<std::string> trace_id) {
  contexts.push_back(context);
  struct Rule {
    const char* phrase;
    const char* device;
    const char* action;
  };
  static const Rule rules[] = {{"불 켜", "LIGHT", "TURN_ON"},
                               {"불 꺼", "LIGHT", "TURN_OFF"},
                               {"현관문 열어", "ENTRY_DOOR", "OPEN"},
                               {"조용히", "ALL_QUIET_DEVICES", "SET_QUIET_MODE"}};
  for (const auto& rule : rules) {
    if (text.find(rule.phrase) == std::string::npos) continue;
    const json proposal = {{"intent", "DEVICE_CONTROL"}, {"device", rule.device}, {"action", rule.action},
                           {"parameters", json::object()}, {"ambiguous", false}, {"safety_flags", json::array()},
                           {"clarification_question", nullptr}};
    const hm::Resolution resolution = action_map_.resolve(proposal);
    hm::VoiceDecision decision;
    decision.kind = "task_request";
    decision.proposal = proposal;
    hm::pfn::TaskRequestArgs args;
    args.source_node_id = node_id_;
    args.confidence = stt_confidence.value_or(0.9);
    args.trace_id = trace_id;
    if (resolution.kind == "device_control") {
      args.intent = "device_control";
      args.target_device = *resolution.device_id;
      args.parameters = hm::control_parameters(resolution, "voice_parser", proposal);
      args.safety_level = action_map_.is_door_open(resolution) ? "caution" : "normal";
    } else {
      args.intent = resolution.intent.value_or("quiet_mode_on");
      args.target_device = "HOME-ENV-01";
      args.parameters = hm::scenario_parameters("voice_parser", proposal);
    }
    decision.messages.emplace_back(hm::pfn::kTaskRequest, hm::pfn::task_request(args));
    return decision;
  }
  hm::pfn::LlmRequestArgs args;
  args.text = text;
  args.utterance_id = utterance_id.value_or(hm::new_id());
  args.stt_model_version = stt_model_version;
  args.stt_confidence = stt_confidence;
  args.context = context;
  args.ttl_ms = llm_ttl_ms_;
  args.simulated = simulated_;
  args.source_node_id = node_id_;
  args.trace_id = trace_id;
  hm::VoiceDecision decision;
  decision.kind = "llm_request";
  decision.proposal = nullptr;
  decision.messages.emplace_back(hm::pfn::kLlmRequest, hm::pfn::llm_request(args));
  decision.note = "parser has no rule";
  return decision;
}

}  // namespace hm::perception::doubles
