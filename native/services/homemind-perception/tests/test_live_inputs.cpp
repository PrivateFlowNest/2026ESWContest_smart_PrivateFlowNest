// homemind-perception live inputs (agent F): remote push-to-talk (pfn/v1/ai/voice/listen), camera presence from
// homemind-vision (presence_fifo -> context engine) and partial-intent hints (partial transcripts -> gate -> hint).
// Python/contract parity and MQTT/FIFO end to end: native/tests/test_perception_live.py, test_perception_partial.py.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <numbers>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "doubles.hpp"
#include "hm/audio/wav.hpp"
#include "presence_feed.hpp"
#include "runner.hpp"
#include "voice_listen.hpp"

namespace fs = std::filesystem;
using namespace hm::perception;
using hm::json;

namespace {

constexpr int SR = 16000;
const std::string kRepo = HM_TEST_REPO_ROOT;

std::vector<float> tone(double seconds, float amplitude = 0.3f, double freq = 220.0) {
  std::vector<float> out(static_cast<std::size_t>(seconds * SR));
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) / SR));
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

fs::path temp_dir() {
  char pattern[] = "/tmp/hm-live-test-XXXXXX";
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

json listen_example() {
  return hm::read_json_file(fs::path(hm::default_contracts_dir()) / "pfn.v1/examples/voice-listen-start.json");
}

json presence(const std::string& zone, double confidence = 0.9) {
  const auto now = hm::now_ms();
  auto iso = [](std::int64_t ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char text[96];
    std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(ms % 1000));
    return std::string(text);
  };
  return {{"schema_version", "1.0"}, {"event_id", hm::new_id()}, {"trace_id", hm::new_id()},
          {"timestamp", iso(now)}, {"valid_until", iso(now + 15000)}, {"source", "pi5.vision-presence"},
          {"source_node_id", "NODE-RPI5-02"}, {"event_type", "PRESENCE"}, {"simulated", true},
          {"payload", {{"presence_zone", zone}, {"confidence", confidence}}}};
}

}  // namespace

// ---- pfn/v1/ai/voice/listen ---------------------------------------------------------------------------------
TEST(VoiceListen, ContractExampleIsValid) { EXPECT_EQ(validate_voice_listen(listen_example()), ""); }

TEST(VoiceListen, SchemaRules) {
  auto with = [](const std::function<void(json&)>& change) {
    json document = listen_example();
    change(document);
    return validate_voice_listen(document);
  };
  for (const char* key : {"schema_version", "event_id", "trace_id", "source_node_id", "target_node_id", "client_id",
                          "action", "sent_at_ms", "expires_at_ms"}) {
    EXPECT_NE(with([&](json& d) { d.erase(key); }), "") << key;
  }
  EXPECT_NE(with([](json& d) { d["extra"] = 1; }), "");
  EXPECT_NE(with([](json& d) { d["text"] = "불 켜줘"; }), "");  // no transcript may ride along
  EXPECT_NE(with([](json& d) { d["schema_version"] = "pfn.v2"; }), "");
  EXPECT_NE(with([](json& d) { d["event_id"] = "not-a-uuid"; }), "");
  EXPECT_NE(with([](json& d) { d["trace_id"] = 7; }), "");
  EXPECT_NE(with([](json& d) { d["source_node_id"] = "NODE-RPI5-02"; }), "");
  EXPECT_NE(with([](json& d) { d["target_node_id"] = "NODE-RPI5-01"; }), "");
  EXPECT_NE(with([](json& d) { d["client_id"] = ""; }), "");
  EXPECT_NE(with([](json& d) { d["client_id"] = std::string(65, 'a'); }), "");
  std::string korean;
  for (int i = 0; i < 64; ++i) korean += "가";  // 64 characters, 192 bytes
  EXPECT_EQ(with([&](json& d) { d["client_id"] = korean; }), "");
  EXPECT_NE(with([](json& d) { d["action"] = "stop"; }), "");
  EXPECT_EQ(with([](json& d) { d["action"] = "cancel"; }), "");
  EXPECT_NE(with([](json& d) { d["sent_at_ms"] = -1; }), "");
  EXPECT_NE(with([](json& d) { d["sent_at_ms"] = 1.5; }), "");
  EXPECT_NE(with([](json& d) { d["expires_at_ms"] = "1792560003000"; }), "");
  EXPECT_EQ(with([](json& d) { d["expires_at_ms"] = 1792560003000.0; }), "");  // jsonschema: 5.0 is an integer
  EXPECT_NE(with([](json& d) { d["simulated"] = "yes"; }), "");
  EXPECT_EQ(with([](json& d) { d["simulated"] = true; }), "");
  EXPECT_NE(validate_voice_listen(json::array()), "");
  // Problems name the field, never the value.
  EXPECT_EQ(with([](json& d) { d["client_id"] = std::string(65, 'q'); }).find("qqq"), std::string::npos);
}

TEST(VoiceListen, StartCancelExpiryDuplicatesAndInactive) {
  RemoteListen remote("NODE-RPI5-02");
  hm::audio::PttControl control;
  json start = listen_example();
  const std::int64_t expires = start["expires_at_ms"];
  EXPECT_EQ(remote.handle(start, expires - 1000), "ignored:inactive");  // no push-to-talk control attached
  remote.attach(&control);
  start["event_id"] = hm::new_id();
  EXPECT_EQ(remote.handle(start, expires), "start");  // expires_at_ms itself is still valid
  EXPECT_EQ(control.poll(), hm::audio::PttCommand::start);
  EXPECT_EQ(remote.handle(start, expires - 10), "ignored:duplicate");  // QoS 1 redelivery
  start["event_id"] = hm::new_id();
  EXPECT_EQ(remote.handle(start, expires + 1), "ignored:expired");  // a stale press is never replayed
  json cancel = listen_example();
  cancel["event_id"] = hm::new_id();
  cancel["action"] = "cancel";
  EXPECT_EQ(remote.handle(cancel, expires - 5), "cancel");
  EXPECT_EQ(control.poll(), hm::audio::PttCommand::cancel);
  EXPECT_FALSE(control.poll().has_value());
  json broken = listen_example();
  broken["action"] = "toggle";
  EXPECT_EQ(remote.handle(broken, expires - 5), "ignored:invalid");
  RemoteListen elsewhere("NODE-TEST");
  elsewhere.attach(&control);
  EXPECT_EQ(elsewhere.handle(listen_example(), expires - 5), "ignored:wrong_target");
  remote.attach(nullptr);
  cancel["event_id"] = hm::new_id();
  EXPECT_EQ(remote.handle(cancel, expires - 5), "ignored:inactive");
  const auto counts = remote.counts();
  EXPECT_EQ(counts.start, 1u);
  EXPECT_EQ(counts.cancel, 1u);
  EXPECT_EQ(counts.duplicate, 1u);
  EXPECT_EQ(counts.expired, 1u);
  EXPECT_EQ(counts.invalid, 1u);
  EXPECT_EQ(counts.inactive, 2u);
  EXPECT_FALSE(control.poll().has_value());
}

TEST(VoiceListen, ProtocolTopicName) {
  const json protocol = hm::read_json_file(fs::path(hm::default_contracts_dir()) / "pfn.v1/protocol.json");
  EXPECT_EQ(protocol["topics"]["ai_voice_listen"], kVoiceListenTopic);
  EXPECT_EQ(protocol["topic_policies"]["ai_voice_listen"]["subscribers"], json::array({"NODE-RPI5-02"}));
}

TEST(PushToTalk, CancelDiscardsTheUtteranceAndIdleCancelIsIgnored) {
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
  // A one-block lossless ring: push(k + 1) returns only after the voice loop read block k.
  hm::audio::AudioRing ring(1600, false);
  const int consumer = ring.add_consumer("voice");
  std::thread thread([&] { voice_loop(service, ring, consumer, "push_to_talk", &control); });
  auto push = [&](const std::vector<float>& audio) {
    for (std::size_t offset = 0; offset < audio.size(); offset += 1600) ring.push(audio.data() + offset, 1600);
  };
  control.push(hm::audio::PttCommand::cancel);  // idle: ignored
  control.push(hm::audio::PttCommand::start);
  push(silence(0.5));
  push(tone(0.6));                               // speech has started
  control.push(hm::audio::PttCommand::cancel);  // the remote UI cancels mid-utterance
  push(tone(0.8));
  push(silence(1.5));
  control.push(hm::audio::PttCommand::start);   // a new press works normally afterwards
  push(silence(0.4));
  push(tone(1.0));
  push(silence(1.2));
  ring.close();
  thread.join();
  ASSERT_EQ(transcriber.seen().size(), 1u);  // only the second press reached whisper
  EXPECT_LT(transcriber.seen()[0], static_cast<std::size_t>(2.8 * SR));
  std::lock_guard lock(lines_mutex);
  EXPECT_NE(std::find(lines.begin(), lines.end(), "듣기를 취소했습니다. Enter를 누르고 말하세요"), lines.end());
  EXPECT_EQ(std::count_if(lines.begin(), lines.end(), [](const std::string& l) { return l.rfind("듣는 중", 0) == 0; }), 2);
  EXPECT_EQ(on_topic(publisher.messages(), "pfn/v1/core/task/request").size(), 1u);
}

TEST(PushToTalk, RemotePressTraceBecomesTheUtteranceTrace) {
  for (const std::string press : {"5f0e0000-0000-4000-8000-0000000000a0", "5F0E0000-0000-4000-8000-0000000000A0"}) {
    CapturePublisher publisher;
    doubles::FakeTranscriber transcriber({"불 켜줘"});
    doubles::KeywordDecider decider(action_map(), "NODE-RPI5-02", 10000, true);
    PerceptionService service(settings_with(), publisher, action_map(), nullptr, &transcriber, &decider, true,
                              [](const std::string&) {});
    hm::audio::PttControl control;
    hm::audio::AudioRing ring(30 * SR, false);
    const int consumer = ring.add_consumer("voice");
    control.push(hm::audio::PttCommand::start, press);
    std::thread thread([&] { voice_loop(service, ring, consumer, "push_to_talk", &control); });
    const auto audio = cat({silence(0.8), tone(1.0), silence(1.2)});
    for (std::size_t offset = 0; offset < audio.size(); offset += 1600) ring.push(audio.data() + offset, 1600);
    ring.close();
    thread.join();
    const auto tasks = on_topic(publisher.messages(), "pfn/v1/core/task/request");
    ASSERT_EQ(tasks.size(), 1u);
    if (press == "5f0e0000-0000-4000-8000-0000000000a0") {
      EXPECT_EQ(tasks[0]["trace_id"], press);  // UI press -> final decision, one trace
    } else {
      EXPECT_NE(tasks[0]["trace_id"], press);  // not canonical text: a fresh trace instead
    }
  }
}

// ---- settings ----------------------------------------------------------------------------------------------------
TEST(LiveSettings, DefaultsAndValidation) {
  const Settings defaults = settings_with();
  EXPECT_FALSE(defaults.presence_fifo.has_value());
  EXPECT_FALSE(defaults.partial_intent.enabled);
  EXPECT_EQ(defaults.partial_intent.vosk_library, "libvosk.so");
  EXPECT_EQ(defaults.partial_intent.vosk_model.filename(), "vosk-model-small-ko-0.22");
  EXPECT_TRUE(defaults.partial_intent.vosk_model.is_absolute());
  EXPECT_EQ(defaults.partial_intent.onnx_model.filename(), "partial-intent-v2.onnx");
  EXPECT_EQ(defaults.partial_intent.chunk_ms, 100);
  EXPECT_EQ(defaults.partial_intent.confirmation_updates, 2);
  EXPECT_EQ(defaults.values["partial_intent"]["enabled"], false);

  const Settings live = settings_with({{"presence_fifo", "/run/homemind/presence"}, {"presence_zone", "DESK"},
                                       {"partial_intent", {{"enabled", true}, {"vosk_library", "lib/libvosk.so"}}},
                                       {"wav_voice", "ptt"}});
  EXPECT_EQ(live.presence_fifo, fs::path("/run/homemind/presence"));
  EXPECT_EQ(live.presence_zone, "UNKNOWN");  // the fixed override is never injected next to live presence
  EXPECT_TRUE(live.partial_intent.enabled);
  EXPECT_TRUE(fs::path(live.partial_intent.vosk_library).is_absolute());
  EXPECT_EQ(live.wav_voice, "ptt");
  EXPECT_THROW(settings_with({{"partial_intent", {{"enabled", "yes"}}}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"partial_intent", {{"chunk_ms", 5}}}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"partial_intent", {{"confirmation_updates", 0}}}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"partial_intent", {{"max_queue_seconds", 0}}}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"partial_intent", "on"}}), std::invalid_argument);
  EXPECT_THROW(settings_with({{"presence_fifo", 3}}), std::invalid_argument);
}

// ---- presence FIFO ---------------------------------------------------------------------------------------------
TEST(PresenceFeed, LinesAndCounters) {
  std::vector<json> seen;
  PresenceFeed feed([&](const json& event) {
    if (event.value("event_type", "") != "PRESENCE") throw std::invalid_argument("not presence");
    seen.push_back(event);
  });
  const std::string valid = hm::dump_compact(presence("DESK"));
  const std::string text = valid + "\n\nnot json\n[1,2]\n{\"event_type\":\"AUDIO_CONTEXT\"}\n" + valid.substr(0, 10);
  feed.feed(text.data(), text.size());
  const std::string rest = valid.substr(10) + "\r\n";
  feed.feed(rest.data(), rest.size());  // a line split across reads
  const std::string longline = std::string(PresenceFeed::kMaxLine + 100, 'x') + "\n" + valid + "\n";
  feed.feed(longline.data(), longline.size());
  const auto counts = feed.counts();
  EXPECT_EQ(seen.size(), 3u);
  EXPECT_EQ(counts.accepted, 3u);
  EXPECT_EQ(counts.not_json, 2u);
  EXPECT_EQ(counts.rejected, 1u);
  EXPECT_EQ(counts.oversized, 1u);
}

TEST(PresenceFeed, ReadsAFifoWhileWritersComeAndGo) {
  const fs::path dir = temp_dir();
  const fs::path fifo = dir / "presence";
  std::mutex mutex;
  std::vector<std::string> zones;
  PresenceFeed feed([&](const json& event) {
    std::lock_guard lock(mutex);
    zones.push_back(event["payload"]["presence_zone"]);
  });
  feed.start(fifo);
  struct stat info {};
  ASSERT_EQ(::stat(fifo.c_str(), &info), 0);
  EXPECT_TRUE(S_ISFIFO(info.st_mode));
  for (const char* zone : {"DESK", "BED", "UNKNOWN"}) {  // homemind-vision: open, one line, close
    const int fd = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
    ASSERT_GE(fd, 0);
    const std::string line = hm::dump_compact(presence(zone)) + "\n";
    ASSERT_EQ(::write(fd, line.data(), line.size()), static_cast<ssize_t>(line.size()));
    ::close(fd);
  }
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard lock(mutex);
      if (zones.size() == 3) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  feed.stop();
  EXPECT_EQ(zones, (std::vector<std::string>{"DESK", "BED", "UNKNOWN"}));
  std::ofstream(dir / "plain") << "x";
  PresenceFeed other([](const json&) {});
  EXPECT_THROW(other.start(dir / "plain"), std::runtime_error);
  fs::remove_all(dir);
}

// The context engine sees the vision zone: presence written before the run reaches the first window.
TEST(PresenceFeed, FileRunUsesLivePresence) {
  const fs::path dir = temp_dir();
  const fs::path fifo = dir / "presence";
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  const int keep = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK);  // holds the line in the pipe until the run reads it
  ASSERT_GE(keep, 0);
  const std::string line = hm::dump_compact(presence("DESK")) + "\n";
  ASSERT_EQ(::write(keep, line.data(), line.size()), static_cast<ssize_t>(line.size()));
  const auto audio = cat({silence(2.0), tone(5.0), silence(2.0)});
  hm::audio::write_pcm16_wav(dir / "scene.wav", audio.data(), audio.size());

  struct WaitForPresence final : ContextEngine {  // first window waits (<= 2 s) for the FIFO line: no race
    doubles::ScriptedContextEngine inner{"UNKNOWN"};
    hm::ContextUpdate process(const float* w, std::size_t n, std::int64_t ts, double t) override {
      for (int i = 0; i < 100 && inner.presence_submitted() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      return inner.process(w, n, ts, t);
    }
    std::string model_version() const override { return inner.model_version(); }
    std::optional<std::string> policy_version() const override { return inner.policy_version(); }
    void submit_presence(const json& event) override { inner.submit_presence(event); }
  };
  const hm::RuntimeConfig config = default_config();
  const Settings settings = resolve_settings({{"wav_input", (dir / "scene.wav").string()}, {"wav_fast", true},
                                              {"wav_voice", "off"}, {"presence_fifo", fifo.string()},
                                              {"presence_zone", "BED"}},
                                             config);
  Hooks hooks;
  hooks.context = [](const Settings& s, bool) {
    EXPECT_EQ(s.presence_zone, "UNKNOWN");
    return std::make_unique<WaitForPresence>();
  };
  CapturePublisher publisher;
  const RunSummary summary = run_with(config, settings, publisher, hooks);
  ::close(keep);
  const auto perceptions = on_topic(publisher.messages(), "pfn/v1/ai/perception");
  ASSERT_FALSE(perceptions.empty());
  for (const auto& doc : perceptions) EXPECT_EQ(doc["zone"], "desk") << hm::dump_compact(doc);
  ASSERT_TRUE(summary.presence.is_object());
  EXPECT_EQ(summary.presence["accepted"], 1);
  fs::remove_all(dir);
}

// ---- partial-intent hints --------------------------------------------------------------------------------------
namespace {

std::unique_ptr<PartialIntentModel> real_partial_model() {
  const fs::path model = fs::path(kRepo) / "services/edge-ai/partial-intent/artifacts/partial-intent-v2.onnx";
  if (!partial_intent_available() || !fs::is_regular_file(model)) return nullptr;
  try {
    return make_partial_intent_model(settings_with({{"partial_intent", {{"onnx_model", model.string()}}}}));
  } catch (const std::exception&) {
    return nullptr;  // built without ONNX Runtime
  }
}

// Every accept() sleeps: a Vosk that is slower than real time.
class SlowPartials final : public PartialTranscriber {
 public:
  struct Session final : PartialSession {
    std::vector<std::string> accept(const float*, std::size_t) override {
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      return {};
    }
    std::vector<std::string> flush() override { return {}; }
  };
  std::unique_ptr<PartialSession> start() override { return std::make_unique<Session>(); }
  std::string model_version() const override { return "slow"; }
};

const std::vector<std::string> kColder = {"방 안이", "방 안이 점점 추워지는", "방 안이 점점 추워지는 것 같아",
                                          "방 안이 점점 추워지는 것 같아 보일러"};

}  // namespace

TEST(PartialHints, ConfirmedIntentBecomesOneHintWithoutText) {
  auto model = real_partial_model();
  if (!model) GTEST_SKIP() << "partial-intent model or ONNX Runtime missing";
  doubles::ScriptedPartialTranscriber partials({kColder});
  CapturePublisher publisher;
  PartialHintWorker::Options options;
  options.drain_at_end = true;
  options.simulated = true;
  {
    PartialHintWorker worker(partials, *model, publisher, action_map(), options);
    const std::string trace = hm::new_id();
    worker.begin(trace);
    const auto audio = silence(0.6);
    for (std::size_t offset = 0; offset < audio.size(); offset += 1600) worker.audio(audio.data() + offset, 1600);
    worker.end();
    worker.finish();
    const json summary = worker.summary();
    EXPECT_EQ(summary["utterances"], 1);
    EXPECT_EQ(summary["chunks"], 6);
    EXPECT_EQ(summary["partials"], 4);
    const auto hints = on_topic(publisher.messages(), "pfn/v1/ai/perception");
    ASSERT_EQ(hints.size(), 1u);  // confirmed once, then suppressed for the rest of the utterance
    EXPECT_EQ(summary["hints"], 1);
    const json& hint = hints[0];
    EXPECT_EQ(hint["observation_type"], "PARTIAL_INTENT_HINT");
    EXPECT_EQ(hint["trace_id"], trace);
    EXPECT_EQ(hint["source_device_id"], "MIC-INDOOR-01");
    EXPECT_EQ(hint["simulated"], true);
    EXPECT_EQ(hint["attributes"]["execution_allowed"], false);
    EXPECT_EQ(hint["attributes"]["requires_confirmation"], true);
    const std::string dumped = hm::dump_compact(hint);
    for (const auto& text : kColder) EXPECT_EQ(dumped.find(text), std::string::npos);
  }
}

TEST(PartialHints, MicrophoneDropsLateAudioAndOverload) {
  auto model = real_partial_model();
  if (!model) GTEST_SKIP() << "partial-intent model or ONNX Runtime missing";
  SlowPartials slow;
  CapturePublisher publisher;
  PartialHintWorker::Options options;
  options.max_queue_seconds = 0.5;
  PartialHintWorker worker(slow, *model, publisher, action_map(), options);
  const auto block = silence(0.1);
  worker.begin(hm::new_id());
  for (int i = 0; i < 4; ++i) worker.audio(block.data(), block.size());
  worker.end();  // utterance over: what Vosk has not reached is dropped
  worker.begin(hm::new_id());
  for (int i = 0; i < 20; ++i) worker.audio(block.data(), block.size());  // 2 s at once: over the 0.5 s queue
  worker.end();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const json summary = worker.summary();
  EXPECT_EQ(summary["overloaded_utterances"], 1);
  EXPECT_LT(summary["chunks"].get<int>(), 24);
  EXPECT_GT(summary["dropped_seconds"].get<double>(), 1.0);
}

// Voice loop: partials stream while each utterance is captured; the hint carries the same trace as the task request.
TEST(PartialHints, VoiceLoopSharesTheUtteranceTrace) {
  auto model = real_partial_model();
  if (!model) GTEST_SKIP() << "partial-intent model or ONNX Runtime missing";
  doubles::ScriptedPartialTranscriber partials({kColder, {"", "불을", "불을 켜"}});
  CapturePublisher publisher;
  doubles::FakeTranscriber transcriber({"불 켜줘"});
  doubles::KeywordDecider decider(action_map(), "NODE-RPI5-02", 10000, true);
  PerceptionService service(settings_with(), publisher, action_map(), nullptr, &transcriber, &decider, true,
                            [](const std::string&) {});
  PartialHintWorker::Options options;
  options.drain_at_end = true;
  options.simulated = true;
  PartialHintWorker worker(partials, *model, publisher, action_map(), options);
  hm::audio::AudioRing ring(30 * SR, false);
  const int consumer = ring.add_consumer("voice");
  std::thread thread([&] { voice_loop(service, ring, consumer, "always", nullptr, nullptr, &worker); });
  const auto audio = cat({silence(1.0), tone(1.2), silence(1.5), tone(1.0), silence(1.5)});
  for (std::size_t offset = 0; offset < audio.size(); offset += 1600) {
    ring.push(audio.data() + offset, std::min<std::size_t>(1600, audio.size() - offset));
  }
  ring.close();
  thread.join();
  worker.finish();
  EXPECT_EQ(partials.sessions(), 2u);
  const auto messages = publisher.messages();
  const auto tasks = on_topic(messages, "pfn/v1/core/task/request");
  ASSERT_EQ(tasks.size(), 2u);
  std::vector<json> hints;
  for (const auto& doc : on_topic(messages, "pfn/v1/ai/perception")) {
    if (doc["observation_type"] == "PARTIAL_INTENT_HINT") hints.push_back(doc);
  }
  ASSERT_FALSE(hints.empty());
  EXPECT_EQ(hints[0]["trace_id"], tasks[0]["trace_id"]);  // hint -> final decision, one trace
  EXPECT_NE(tasks[0]["trace_id"], tasks[1]["trace_id"]);
  const json summary = worker.summary();
  EXPECT_EQ(summary["utterances"], 2);
  EXPECT_GT(summary["chunks"].get<int>(), 15);  // ~1.5 s + ~1.3 s of utterance audio in 100 ms blocks
}

TEST(PartialHints, DisabledByDefaultAndMissingVoskOnlyDisablesHints) {
  const fs::path dir = temp_dir();
  const auto audio = cat({silence(1.0), tone(1.0), silence(1.0)});
  hm::audio::write_pcm16_wav(dir / "a.wav", audio.data(), audio.size());
  const hm::RuntimeConfig config = default_config();
  for (const bool enabled : {false, true}) {
    const Settings settings = resolve_settings(
        {{"wav_input", (dir / "a.wav").string()}, {"wav_fast", true}, {"wav_voice", "whole"},
         {"partial_intent", {{"enabled", enabled}, {"vosk_library", "/nonexistent/libvosk.so"}}}},
        config);
    Hooks hooks;
    hooks.context = [](const Settings& s, bool) { return std::make_unique<doubles::ScriptedContextEngine>(s.presence_zone); };
    hooks.transcriber = [](const Settings&) { return std::make_unique<doubles::FakeTranscriber>(std::vector<std::string>{"불 켜줘"}); };
    hooks.parser = [](const hm::ActionMap& map, const Settings&, const hm::RuntimeConfig& c, bool simulated) {
      return std::make_unique<doubles::KeywordDecider>(map, c.perception_node_id, c.llm_request_ttl_ms, simulated);
    };
    CapturePublisher publisher;
    const RunSummary summary = run_with(config, settings, publisher, hooks);
    EXPECT_TRUE(summary.partial_intent.is_null());  // never started (off, or libvosk missing -> logged)
    EXPECT_EQ(on_topic(publisher.messages(), "pfn/v1/core/task/request").size(), 1u);  // voice path unaffected
  }
  fs::remove_all(dir);
}
