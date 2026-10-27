// homemind-perception unit tests: ports of services/runtime tests/test_perception.py with doubles for the
// context model, whisper and the parser. Contract validation and Python parity: native/tests/test_perception_*.py.
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "doubles.hpp"
#include "hm/audio/wav.hpp"
#include "hm/pfn.hpp"
#include "runner.hpp"

namespace fs = std::filesystem;
using namespace hm::perception;
using hm::json;

namespace {

constexpr int SR = 16000;

std::vector<float> tone(double seconds, float amplitude = 0.3f, double freq = 220.0) {
  std::vector<float> out(static_cast<std::size_t>(seconds * SR));
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<float>(amplitude * std::sin(2.0 * 3.141592653589793 * freq * static_cast<double>(i) / SR));
  }
  return out;
}

std::vector<float> silence(double seconds, float amplitude = 0.0005f, unsigned seed = 0) {
  std::mt19937 engine(seed);
  std::normal_distribution<float> normal(0.0f, 1.0f);
  std::vector<float> out(static_cast<std::size_t>(seconds * SR));
  for (float& value : out) value = amplitude * normal(engine);
  return out;
}

std::vector<float> cat(std::initializer_list<std::vector<float>> parts) {
  std::vector<float> out;
  for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

hm::RuntimeConfig default_config() { return hm::RuntimeConfig::load({}); }

Settings settings_with(const json& extra = json::object()) { return resolve_settings(extra, default_config()); }

const hm::ActionMap& action_map() {
  static const hm::ActionMap map(hm::default_contracts_dir());
  return map;
}

json fused_event(const json& overrides = json::object()) {
  json event = {
      {"schema_version", "1.0"}, {"event_id", "ctx-1"}, {"trace_id", "trace-1"},
      {"timestamp", "2026-10-22T00:00:00.000Z"}, {"source", "pi5.context-fusion"},
      {"source_node_id", "NODE-RPI5-02"}, {"event_type", "ACTIVITY_CONTEXT"}, {"context_type", "ZOOM_SPEAKER"},
      {"presence_zone", "DESK"}, {"activity", "MEETING"}, {"quiet_required", true}, {"confidence", 0.87},
      {"evidence", {"audio:ZOOM_SPEAKER:0.91", "audio_model:" + std::string(80, 'x'), "margin:0.300", "margin:0.300"}},
      {"transition", "ACTIVATED"}, {"valid_until", "2026-10-22T00:00:10.000Z"},
      {"model_version", "context-fusion-rules-v1"}};
  for (const auto& [key, value] : overrides.items()) event[key] = value;
  return event;
}

fs::path temp_dir() {
  char pattern[] = "/tmp/hm-perception-test-XXXXXX";
  const char* made = ::mkdtemp(pattern);
  if (!made) throw std::runtime_error("mkdtemp failed");
  return made;
}

std::vector<json> on_topic(const std::vector<std::pair<std::string, json>>& messages, const std::string& topic) {
  std::vector<json> out;
  for (const auto& [t, doc] : messages) {
    if (t == topic) out.push_back(doc);
  }
  return out;
}

// run_with owns what the hooks return; these forwarders keep the doubles alive for the assertions.
struct ForwardStt final : SpeechToText {
  explicit ForwardStt(std::shared_ptr<doubles::FakeTranscriber> target) : target(std::move(target)) {}
  hm::stt::SttResult transcribe(const std::vector<float>& samples) override { return target->transcribe(samples); }
  std::shared_ptr<doubles::FakeTranscriber> target;
};
struct ForwardDecider final : VoiceDecider {
  explicit ForwardDecider(std::shared_ptr<doubles::KeywordDecider> target) : target(std::move(target)) {}
  hm::VoiceDecision handle(const std::string& text, const json& context, const std::string& version,
                           std::optional<double> confidence, std::optional<std::string> utterance_id,
                           std::optional<std::string> trace_id) override {
    return target->handle(text, context, version, confidence, std::move(utterance_id), std::move(trace_id));
  }
  std::shared_ptr<doubles::KeywordDecider> target;
};

struct FileRun {
  CapturePublisher publisher;
  RunSummary summary;
  std::shared_ptr<doubles::FakeTranscriber> transcriber;
  std::shared_ptr<doubles::KeywordDecider> decider;
};

void run_file(FileRun& run, const fs::path& wav, const json& extra, std::vector<std::string> texts, bool stt_fail = false) {
  json settings_json = extra;
  settings_json["wav_input"] = wav.string();
  settings_json["wav_fast"] = true;
  const hm::RuntimeConfig config = default_config();
  const Settings settings = resolve_settings(settings_json, config);
  Hooks hooks;
  hooks.context = [](const Settings& s, bool) { return std::make_unique<doubles::ScriptedContextEngine>(s.presence_zone); };
  run.transcriber = std::make_shared<doubles::FakeTranscriber>(texts, stt_fail);
  run.decider = std::make_shared<doubles::KeywordDecider>(action_map(), config.perception_node_id,
                                                          config.llm_request_ttl_ms, true);
  hooks.transcriber = [&run](const Settings&) { return std::make_unique<ForwardStt>(run.transcriber); };
  hooks.parser = [&run](const hm::ActionMap&, const Settings&, const hm::RuntimeConfig&, bool) {
    return std::make_unique<ForwardDecider>(run.decider);
  };
  run.summary = run_with(config, settings, run.publisher, hooks);
}

}  // namespace

// ---- fusion -> perception -----------------------------------------------------------------------
TEST(Perception, FusionEventToPerception) {
  const json doc = context_event_to_perception(fused_event(), action_map(), "yamnet-temporal-cost-ensemble-v0.8.0-50-50",
                                               "audio-policy-v6.2.1-ensemble-50-50", 15000, std::nullopt, true);
  const json& attrs = doc["attributes"];
  EXPECT_EQ(doc["zone"], "desk");
  EXPECT_EQ(doc["source_node_id"], "NODE-RPI5-02");
  EXPECT_EQ(doc["simulated"], true);
  EXPECT_EQ(doc["observation_type"], "ACTIVITY_CONTEXT");
  EXPECT_EQ(doc["confidence"], 0.87);
  EXPECT_EQ(attrs["activity"], "MEETING");
  EXPECT_EQ(attrs["audio_context"], "ZOOM_SPEAKER");
  EXPECT_EQ(attrs["quiet_required"], true);
  EXPECT_EQ(attrs["transition"], "ACTIVATED");
  EXPECT_EQ(attrs["evidence"][0], "AUDIO_CONTEXT:ZOOM_SPEAKER");
  EXPECT_EQ(attrs["evidence"][1], "PRESENCE:DESK");
  EXPECT_EQ(attrs["evidence"][2], "policy:audio-policy-v6.2.1-ensemble-50-50");
  EXPECT_EQ(attrs["evidence"].size(), 6u);  // the duplicate margin collapses
  for (const auto& item : attrs["evidence"]) {
    EXPECT_GE(item.get<std::string>().size(), 1u);
    EXPECT_LE(item.get<std::string>().size(), 64u);
  }
  EXPECT_GT(attrs["valid_until_ms"].get<std::int64_t>(), hm::now_ms() + 14000);
  EXPECT_LE(attrs["valid_until_ms"].get<std::int64_t>(), hm::now_ms() + 15000);
}

TEST(Perception, UnknownPresenceAndTransitionOverride) {
  const json doc = context_event_to_perception(fused_event({{"presence_zone", "UNKNOWN"}, {"evidence", json::array()}}),
                                               action_map(), "m", std::nullopt, 15000, "UNCHANGED", false);
  EXPECT_EQ(doc["zone"], "unknown");
  EXPECT_EQ(doc["attributes"]["transition"], "UNCHANGED");
  EXPECT_FALSE(doc.contains("simulated"));
  EXPECT_EQ(doc["attributes"]["evidence"], json({"AUDIO_CONTEXT:ZOOM_SPEAKER", "PRESENCE:UNKNOWN"}));
}

TEST(Perception, PublishesTransitionsAndRepublishesEvery10s) {
  CapturePublisher publisher;
  PerceptionService service(settings_with(), publisher, action_map(), nullptr, nullptr, nullptr, true,
                            [](const std::string&) {});
  const std::vector<std::pair<int, std::string>> timeline = {{1, "UNCHANGED"}, {2, "UNCHANGED"}, {3, "ACTIVATED"},
                                                             {4, "UNCHANGED"}, {12, "UNCHANGED"}, {13, "UNCHANGED"},
                                                             {14, "DEACTIVATED"}, {15, "UNCHANGED"}, {24, "UNCHANGED"}};
  for (const auto& [t, transition] : timeline) {
    const bool quiet = transition == "ACTIVATED" || (3 <= t && t < 14);
    service.on_context_event(fused_event({{"transition", transition}, {"quiet_required", quiet}}), t);
  }
  std::vector<std::string> transitions;
  for (const auto& [topic, doc] : publisher.messages()) {
    EXPECT_EQ(topic, "pfn/v1/ai/perception");
    transitions.push_back(doc["attributes"]["transition"]);
  }
  EXPECT_EQ(transitions, (std::vector<std::string>{"UNCHANGED", "ACTIVATED", "UNCHANGED", "DEACTIVATED", "UNCHANGED"}));
  std::vector<double> times;
  for (const auto& record : service.summary().perceptions) times.push_back(record["t"]);
  EXPECT_EQ(times, (std::vector<double>{1.0, 3.0, 13.0, 14.0, 24.0}));
}

TEST(Perception, LatestContextHonoursTtlAndAsOf) {
  CapturePublisher publisher;
  PerceptionService service(settings_with(), publisher, action_map(), nullptr, nullptr, nullptr, true,
                            [](const std::string&) {});
  EXPECT_FALSE(service.latest_context(1.0).has_value());
  service.on_context_event(fused_event({{"transition", "UNCHANGED"}, {"context_type", "QUIET"}}), 4.0);
  service.on_context_event(fused_event({{"transition", "ACTIVATED"}}), 5.0);
  const auto latest = service.latest_context(4.5);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(*latest, json({{"presence_zone", "DESK"}, {"activity", "MEETING"}, {"audio_context", "ZOOM_SPEAKER"},
                           {"quiet_required", true}, {"confidence", 0.87}}));
  EXPECT_EQ((*service.latest_context(4.5, true))["audio_context"], "QUIET");
  EXPECT_FALSE(service.latest_context(3.0, true).has_value());
  EXPECT_TRUE(service.latest_context(20.0).has_value());   // 15 s after the last event
  EXPECT_FALSE(service.latest_context(20.5).has_value());  // older than perception_ttl_seconds
}

TEST(Perception, UtteranceDecisionsEchoAndWhisperFailure) {
  CapturePublisher publisher;
  std::vector<std::string> lines;
  doubles::FakeTranscriber transcriber({"불 켜줘", "", "아 좀 덥네"});
  doubles::KeywordDecider decider(action_map(), "NODE-RPI5-02", 10000, true);
  PerceptionService service(settings_with(), publisher, action_map(), nullptr, &transcriber, &decider, true,
                            [&](const std::string& line) { lines.push_back(line); });
  service.on_context_event(fused_event({{"transition", "UNCHANGED"}}), 1.0);
  const auto first = service.on_utterance(tone(1.0), 2.0);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ((*first)["decision"], "task_request");
  EXPECT_EQ((*first)["device"], "LIGHT");
  EXPECT_EQ((*first)["context"], "ZOOM_SPEAKER");
  EXPECT_EQ((*first)["chars"], 4);
  const auto empty = service.on_utterance(tone(0.5), 3.0);
  EXPECT_EQ((*empty)["decision"], "empty");
  const auto vague = service.on_utterance(tone(0.8), 30.0);  // context expired -> llm/request without context
  EXPECT_EQ((*vague)["decision"], "llm_request");
  EXPECT_TRUE((*vague)["context"].is_null());
  transcriber.failing = true;
  EXPECT_FALSE(service.on_utterance(tone(0.8), 31.0).has_value());
  const auto messages = publisher.messages();
  const auto tasks = on_topic(messages, "pfn/v1/core/task/request");
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0]["target_device"], "LED-LIVING-01");
  const auto llm = on_topic(messages, "pfn/v1/ai/llm/request");
  ASSERT_EQ(llm.size(), 1u);
  EXPECT_FALSE(llm[0].contains("context"));
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "[voice] 1.0s, STT 0 ms → task_request LIGHT TURN_ON");
  EXPECT_EQ(lines[1], "[voice] 0.5s, STT 0 ms → 인식된 문장 없음");
  for (const auto& line : lines) EXPECT_EQ(line.find("켜줘"), std::string::npos);  // no transcript unless debug
  EXPECT_EQ(service.summary().voice.size(), 3u);
}

// ---- MQTT outbox ----------------------------------------------------------------------------------
TEST(BusPublisher, HoldsMessagesWhileDisconnectedAndDropsExpiredOnes) {
  std::atomic<bool> up{true};
  std::mutex sent_mutex;
  std::vector<std::string> sent;
  BusPublisher publisher([&] { return up.load(); },
                         [&](const std::string& topic, const json& document) {
                           if (!up) return false;
                           std::lock_guard lock(sent_mutex);
                           sent.push_back(topic + ":" + document.value("n", std::string()));
                           return true;
                         },
                         /*hold_ms=*/300);
  publisher.publish("pfn/v1/core/task/request", {{"n", "a"}});
  up = false;
  const std::int64_t now = hm::now_ms();
  publisher.publish("pfn/v1/core/task/request", {{"n", "b"}});                             // held 300 ms
  publisher.publish("pfn/v1/ai/perception", {{"n", "c"}, {"attributes", {{"valid_until_ms", now + 60000}}}});
  publisher.publish("pfn/v1/ai/llm/request", {{"n", "d"}, {"expires_at_ms", now - 1}});  // already expired
  EXPECT_EQ(publisher.pending(), 3u);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // task request b expires while down
  up = true;
  publisher.publish("pfn/v1/ai/perception", {{"n", "e"}, {"attributes", {{"valid_until_ms", now + 60000}}}});
  publisher.flush();
  EXPECT_EQ(publisher.pending(), 0u);
  EXPECT_EQ(publisher.dropped(), 2u);
  std::lock_guard lock(sent_mutex);
  EXPECT_EQ(sent, (std::vector<std::string>{"pfn/v1/core/task/request:a", "pfn/v1/ai/perception:c",
                                            "pfn/v1/ai/perception:e"}));  // order kept
}

// ---- settings -------------------------------------------------------------------------------------
TEST(Settings, ExampleConfigLoadsWithoutSecrets) {
  const fs::path path = fs::path(HM_TEST_REPO_ROOT) / "services/runtime/config/runtime.pc.example.json";
  const auto config = hm::RuntimeConfig::load(path);
  EXPECT_FALSE(config.broker.password_file.has_value());
  EXPECT_EQ(config.broker.host, "127.0.0.1");
  const Settings settings = resolve_settings(config.extra, config);
  EXPECT_EQ(settings.listen_mode, "push_to_talk");
  EXPECT_EQ(settings.presence_zone, "UNKNOWN");
  EXPECT_EQ(settings.vad.end_silence_ms, 700);
  EXPECT_TRUE(settings.whisper_model.is_absolute());
  EXPECT_EQ(settings.whisper_model.filename(), "ggml-whisper-small-homemind-lora-v4-q5_0.bin");
  std::ifstream stream(path);
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  for (std::size_t pos; (pos = text.find("password_file")) != std::string::npos;) text.erase(pos, 13);
  EXPECT_EQ(text.find("password"), std::string::npos);
}

TEST(Settings, ValidationAndNormalisation) {
  const Settings s = settings_with({{"presence_zone", "desk"}, {"wav_input", "a.wav"}, {"wav_max_seconds", 0},
                                    {"unknown_key", 1}, {"audio_device", 2}, {"whisper_server_url", ""}});
  EXPECT_EQ(s.presence_zone, "DESK");
  ASSERT_EQ(s.wav_input.size(), 1u);
  EXPECT_FALSE(s.wav_max_seconds.has_value());  // Python: 0 -> None
  EXPECT_EQ(s.audio_device, "2");
  EXPECT_FALSE(s.whisper_server_url.has_value());
  EXPECT_THROW(settings_with({{"presence_zone", "KITCHEN"}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"listen_mode", "sometimes"}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"publish", "kafka"}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"vad", {{"bogus", 1}}}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"window_seconds", "4"}}), std::invalid_argument);
}

// ---- file mode (scripted context + fake whisper + keyword decider) --------------------------------
TEST(FileMode, RunsBothPaths) {
  const fs::path dir = temp_dir();
  const auto audio = cat({silence(5.0), tone(14.0, 0.3f), silence(8.0)});
  hm::audio::write_pcm16_wav(dir / "scene.wav", audio.data(), audio.size());
  FileRun run;
  run_file(run, dir / "scene.wav", {{"wav_voice", "whole"}, {"max_whole_file_seconds", 60}}, {"불 켜줘"});
  const auto messages = run.publisher.messages();
  const auto perceptions = on_topic(messages, "pfn/v1/ai/perception");
  std::vector<std::string> transitions;
  for (const auto& doc : perceptions) {
    transitions.push_back(doc["attributes"]["transition"]);
    EXPECT_EQ(doc["simulated"], true);  // file mode is always simulated
  }
  EXPECT_NE(std::find(transitions.begin(), transitions.end(), "ACTIVATED"), transitions.end());
  EXPECT_NE(std::find(transitions.begin(), transitions.end(), "DEACTIVATED"), transitions.end());
  EXPECT_EQ(run.summary.windows, 24);  // 27 s, 4 s window, 1 s hop
  const auto tasks = on_topic(messages, "pfn/v1/core/task/request");
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0]["target_device"], "LED-LIVING-01");
  EXPECT_EQ(run.summary.voice[0]["decision"], "task_request");
  ASSERT_EQ(run.transcriber->seen().size(), 1u);
  EXPECT_EQ(run.transcriber->seen()[0], audio.size());
  EXPECT_EQ(messages.back().first, "pfn/v1/core/task/request");  // whole-file utterance after the context path
  fs::remove_all(dir);
}

TEST(FileMode, VadVoicePathAndDeterministicContext) {
  const fs::path dir = temp_dir();
  const auto audio = cat({silence(1.0), tone(1.0), silence(1.5), tone(1.0), silence(1.5)});
  hm::audio::write_pcm16_wav(dir / "two.wav", audio.data(), audio.size());
  FileRun run;
  run_file(run, dir / "two.wav", {{"wav_voice", "vad"}}, {"현관문 열어줘"});
  const auto tasks = on_topic(run.publisher.messages(), "pfn/v1/core/task/request");
  ASSERT_EQ(tasks.size(), 2u);
  for (const auto& task : tasks) EXPECT_EQ(task["safety_level"], "caution");
  // Utterances end at ~2.6 s and ~5.1 s: the first sees no window yet (first window ends at 4 s),
  // the second sees the 5 s window, never a later one.
  ASSERT_EQ(run.decider->contexts.size(), 2u);
  EXPECT_TRUE(run.decider->contexts[0].is_null());
  EXPECT_FALSE(run.decider->contexts[1].is_null());
  fs::remove_all(dir);
}

TEST(FileMode, ShortFileGetsOnePaddedWindow) {
  const fs::path dir = temp_dir();
  const auto audio = tone(2.5);
  hm::audio::write_pcm16_wav(dir / "short.wav", audio.data(), audio.size());
  FileRun run;
  run_file(run, dir / "short.wav", {{"wav_voice", "off"}}, {});
  EXPECT_EQ(run.summary.windows, 1);
  EXPECT_EQ(on_topic(run.publisher.messages(), "pfn/v1/ai/perception").size(), 1u);
  fs::remove_all(dir);
}

TEST(FileMode, WhisperDownKeepsContextPathRunning) {
  const fs::path dir = temp_dir();
  const auto audio = cat({silence(1.0), tone(1.0), silence(1.5), tone(1.0), silence(6.5)});
  hm::audio::write_pcm16_wav(dir / "down.wav", audio.data(), audio.size());
  FileRun run;
  run_file(run, dir / "down.wav", {{"wav_voice", "vad"}}, {"불 켜줘"}, /*stt_fail=*/true);
  EXPECT_EQ(run.transcriber->seen().size(), 2u);
  EXPECT_TRUE(on_topic(run.publisher.messages(), "pfn/v1/core/task/request").empty());
  EXPECT_EQ(run.summary.windows, 8);
  EXPECT_GE(on_topic(run.publisher.messages(), "pfn/v1/ai/perception").size(), 2u);
  fs::remove_all(dir);
}

// ---- push-to-talk ---------------------------------------------------------------------------------
TEST(PushToTalk, RecordsOnlyAfterPress) {
  CapturePublisher publisher;
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  doubles::FakeTranscriber transcriber({"불 켜줘"});
  doubles::KeywordDecider decider(action_map(), "NODE-RPI5-02", 10000, true);
  PerceptionService service(settings_with(), publisher, action_map(), nullptr, &transcriber, &decider, true,
                            [&](const std::string& line) {
                              std::lock_guard lock(lines_mutex);
                              lines.push_back(line);
                            });
  hm::audio::PttControl control;
  auto feed = [&](const std::vector<float>& audio, bool press_first) {
    hm::audio::AudioRing ring(30 * SR, false);
    const int consumer = ring.add_consumer("voice");
    std::thread thread([&] { voice_loop(service, ring, consumer, "push_to_talk", &control); });
    if (press_first) control.press();
    for (std::size_t offset = 0; offset < audio.size(); offset += 1600) {
      ring.push(audio.data() + offset, std::min<std::size_t>(1600, audio.size() - offset));
    }
    ring.close();
    thread.join();
  };
  feed(cat({silence(0.5), tone(1.5), silence(1.0)}), false);  // speaker audio, no press -> ignored
  EXPECT_TRUE(transcriber.seen().empty());
  feed(cat({silence(0.8), tone(1.0), silence(1.2)}), true);
  EXPECT_EQ(transcriber.seen().size(), 1u);
  const auto tasks = on_topic(publisher.messages(), "pfn/v1/core/task/request");
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0]["target_device"], "LED-LIVING-01");
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines[0].rfind("듣는 중", 0), 0u);
  EXPECT_EQ(lines.back(), "Enter를 누르고 말하세요");
}

TEST(PushToTalk, TimeoutWithoutSpeechAndStartStopCommands) {
  CapturePublisher publisher;
  std::vector<std::string> lines;
  doubles::FakeTranscriber transcriber({"불 꺼줘"});
  doubles::KeywordDecider decider(action_map(), "NODE-RPI5-02", 10000, true);
  PerceptionService service(settings_with({{"ptt_timeout_seconds", 1.0}}), publisher, action_map(), nullptr,
                            &transcriber, &decider, true, [&](const std::string& line) { lines.push_back(line); });
  hm::audio::PttControl control;
  hm::audio::AudioRing ring(30 * SR, false);
  const int consumer = ring.add_consumer("voice");
  control.push(hm::audio::PttCommand::stop);   // ignored while idle
  control.push(hm::audio::PttCommand::start);  // opens
  control.push(hm::audio::PttCommand::start);  // ignored while listening
  std::thread thread([&] { voice_loop(service, ring, consumer, "push_to_talk", &control); });
  const auto quiet = silence(2.0);
  for (std::size_t offset = 0; offset < quiet.size(); offset += 1600) ring.push(quiet.data() + offset, 1600);
  ring.close();
  thread.join();
  EXPECT_TRUE(transcriber.seen().empty());
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1], "음성이 감지되지 않았습니다. Enter를 누르고 다시 말하세요");
}

// Sound level telemetry (user 2026-10-25): estimate without calibration, live microphone only, privacy gated.
TEST(SoundLevel, EstimateTelemetryAndPrivacy) {
  EXPECT_NEAR(estimated_sound_db(1e-6, 95.0), 35.0, 1e-9);  // -60 dBFS: a quiet room
  EXPECT_NEAR(estimated_sound_db(0.5, 95.0), 92.0, 0.02);   // full-scale sine, -3 dBFS
  EXPECT_EQ(estimated_sound_db(0.0, 95.0), 0.0);
  EXPECT_EQ(estimated_sound_db(10.0, 150.0), 120.0);

  const json telemetry = sound_level_telemetry("NODE-RPI5-02", 3, 44.44, 1000, 2000, "trace");
  EXPECT_EQ(telemetry["schema_version"], "pfn.v1");
  EXPECT_EQ(telemetry["event_id"].get<std::string>().size(), 36u);  // hm::new_id(): UUID v4 text
  EXPECT_EQ(telemetry["node_id"], "NODE-RPI5-02");
  EXPECT_EQ(telemetry["seq"], 3);
  EXPECT_EQ(telemetry["sensor_ready"], true);
  EXPECT_EQ(telemetry["readings"], (json{{"sound_level_db", 44.4}}));
  EXPECT_EQ(telemetry["reading_sources"], (json{{"sound_level_db", "MIC-INDOOR-01"}}));
  EXPECT_EQ(telemetry_topic("NODE-RPI5-02"), "pfn/v1/device/NODE-RPI5-02/telemetry");

  CapturePublisher publisher;
  bool allowed = true;
  PerceptionService service(settings_with(), publisher, action_map(), nullptr, nullptr, nullptr, false,
                            [](const std::string&) {}, "NODE-RPI5-02");
  service.process_window(std::vector<float>(64000, 0.5f).data(), 64000, 64000);  // not enabled: nothing
  EXPECT_TRUE(publisher.messages().empty());
  service.enable_sound_level([&] { return allowed; });
  const std::vector<float> quiet(64000, 0.001f);  // -60 dBFS
  std::int64_t end = 64000;
  auto windows = [&](int count) {
    for (int i = 0; i < count; ++i, end += 16000) service.process_window(quiet.data(), quiet.size(), end);
  };
  windows(6);  // t = 4..9 s: published at 9 s (5 s after the first window)
  const auto first = publisher.messages();
  ASSERT_EQ(first.size(), 1u);
  const auto& [topic, document] = first[0];
  EXPECT_EQ(topic, "pfn/v1/device/NODE-RPI5-02/telemetry");
  EXPECT_EQ(document["readings"]["sound_level_db"], 35.0);
  EXPECT_EQ(document["seq"], 0);
  EXPECT_EQ(document["uptime_ms"], 9000);
  allowed = false;  // privacy/state microphone_allowed false: not even the number leaves the node
  windows(10);
  EXPECT_EQ(publisher.messages().size(), 1u);
  allowed = true;
  windows(5);
  ASSERT_EQ(publisher.messages().size(), 2u);
  EXPECT_EQ(publisher.messages()[1].second["seq"], 1);
}

TEST(SoundLevel, Settings) {
  const Settings defaults = settings_with();
  EXPECT_FALSE(defaults.sound_level.enabled);  // off until the broker ACL and the UI display ship
  EXPECT_EQ(defaults.sound_level.interval_seconds, 5.0);
  EXPECT_EQ(defaults.sound_level.offset_db, 95.0);
  EXPECT_EQ(defaults.values["sound_level"]["offset_db"], 95.0);
  const Settings changed = settings_with({{"sound_level", {{"enabled", true}, {"offset_db", 100}}}});
  EXPECT_TRUE(changed.sound_level.enabled);
  EXPECT_EQ(changed.sound_level.offset_db, 100.0);
  EXPECT_EQ(changed.sound_level.interval_seconds, 5.0);
  EXPECT_THROW(settings_with({{"sound_level", "on"}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"sound_level", {{"enabled", 1}}}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"sound_level", {{"interval_seconds", 0.5}}}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"sound_level", {{"offset_db", -1}}}}), std::invalid_argument);
}
