// hm_context unit tests (PC x86_64 Docker). Cross-language parity lives in native/tests/test_context_*.py.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "hm/context/activity_head.hpp"
#include "hm/context/audio_features.hpp"
#include "hm/context/fusion.hpp"
#include "hm/context/pyfmt.hpp"
#include "hm/context_pipeline.hpp"

using hm::json;
namespace ctx = hm::ctx;

namespace {

// HM_CONTEXT_REPO_ROOT overrides the configure-time repo root (isolated builds use a symlinked source tree).
const std::filesystem::path kRepo = std::getenv("HM_CONTEXT_REPO_ROOT") ? std::getenv("HM_CONTEXT_REPO_ROOT") : HM_TEST_REPO_ROOT;
const std::filesystem::path kPolicy = kRepo / "services/edge-ai/context-fusion/config/audio-policy-v6-ensemble-50-50.json";
const std::filesystem::path kYamnet = kRepo / "native/.deps/models/yamnet/yamnet-embeddings.onnx";
const std::filesystem::path kHead = kRepo / "native/.deps/models/activity-head/public-balanced-v6-ensemble-50-50.json";

std::string iso(int second) {  // like test_stream_fusion.iso(): BASE 2026-09-03T10:00Z + seconds
  return ctx::iso_python(ctx::parse_iso("2026-09-03T10:00:00Z") + static_cast<ctx::Micros>(second) * 1000000);
}

json feature(const std::string& type, int second, json payload, int ttl = 10) {
  std::string id = type + "-" + std::to_string(second);
  for (auto& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return {{"schema_version", "1.0"}, {"event_id", id},           {"trace_id", "stream-test"},
          {"timestamp", iso(second)}, {"valid_until", iso(second + ttl)}, {"source", "pc.scenario"},
          {"source_node_id", "PC-SIMULATION"}, {"event_type", type}, {"simulated", true}, {"payload", payload}};
}

json audio(int second, const std::string& context = "IN_PERSON_MEETING") {
  return feature("AUDIO_CONTEXT", second,
                 {{"context_type", context}, {"confidence", 0.9}, {"speech_ratio", context != "QUIET" ? 0.6 : 0.0},
                  {"duration_seconds", 4 + second}, {"model_version", "test-audio-v1"}},
                 5);
}

bool has(const json& list, const std::string& item) {
  for (const auto& value : list)
    if (value == item) return true;
  return false;
}

}  // namespace

TEST(PyFormat, RoundAndFixedMatchPython) {
  EXPECT_EQ(ctx::py_round(0.125, 2), 0.12);
  EXPECT_EQ(ctx::py_round(2.675, 2), 2.67);
  EXPECT_EQ(ctx::py_round(0.9930885, 6), 0.993089);
  EXPECT_EQ(ctx::py_fixed(0.125, 2), "0.12");
  EXPECT_EQ(ctx::py_fixed(0.375, 2), "0.38");
  EXPECT_EQ(ctx::py_fixed(-0.001, 2), "-0.00");
  EXPECT_EQ(ctx::py_fixed(2.5, 0), "2");
  EXPECT_EQ(ctx::py_repr(0.1), "0.1");
  EXPECT_EQ(ctx::py_repr(3.0), "3.0");
  EXPECT_EQ(ctx::py_repr(1e-05), "1e-05");
  EXPECT_EQ(ctx::py_repr(0.0001), "0.0001");
  EXPECT_EQ(ctx::py_repr(1e16), "1e+16");
  EXPECT_EQ(ctx::py_repr(123456789.0), "123456789.0");
  EXPECT_EQ(ctx::py_str(json(3)), "3");
  EXPECT_EQ(ctx::py_str(json(3.0)), "3.0");
}

TEST(PyFormat, IsoTimestamps) {
  const ctx::Micros t = ctx::parse_iso("2026-09-03T10:00:00Z");
  EXPECT_EQ(t, 1788429600LL * 1000000);
  EXPECT_EQ(ctx::iso_python(t), "2026-09-03T10:00:00+00:00");
  EXPECT_EQ(ctx::iso_python(t + 123456), "2026-09-03T10:00:00.123456+00:00");
  EXPECT_EQ(ctx::iso_millis_z(t + 123999), "2026-09-03T10:00:00.123Z");
  EXPECT_EQ(ctx::parse_iso("2026-09-03T19:00:00+09:00"), t);
  EXPECT_EQ(ctx::parse_iso("2026-09-03 10:00:00.5"), t + 500000);
  EXPECT_EQ(ctx::parse_iso("2026-09-03T10:00:00.1234567+00:00"), t + 123456);
  EXPECT_THROW(ctx::parse_iso("2026-02-30T10:00:00Z"), std::invalid_argument);
  EXPECT_THROW(ctx::parse_iso("not a date"), std::invalid_argument);
}

TEST(Vad, SilenceHasNoSpeech) {
  const std::vector<float> silence(64000, 0.0f);
  const auto f = ctx::vad_temporal_features(silence.data(), silence.size());
  EXPECT_EQ(f[0], 0.0f);
  EXPECT_EQ(f[1], 0.0f);
  EXPECT_FLOAT_EQ(f[5], 3.98f);  // one pause run over 398 frames
  EXPECT_FLOAT_EQ(f[7], 3.98f);
  EXPECT_NEAR(f[8], -1.199, 1e-3);
}

TEST(Vad, ToneBurstsAreVoiced) {
  std::vector<float> signal(64000, 0.0f);
  for (std::size_t i = 0; i < signal.size(); ++i)
    if ((i / 8000) % 2 == 0) signal[i] = 0.3f * static_cast<float>(std::sin(2 * M_PI * 220.0 * i / 16000.0));
  const auto f = ctx::vad_temporal_features(signal.data(), signal.size());
  EXPECT_GT(f[0], 0.4f);
  EXPECT_LT(f[0], 0.6f);
  EXPECT_EQ(f[1] * 398.0f, 4.0f);  // four speech runs
}

TEST(Fusion, AsyncMeetingActivatesAndQuietReleases) {  // test_stream_fusion.py
  ctx::FeatureStreamFusion fusion;
  for (const json& event : {feature("DEVICE_STATE", -1, {{"tv_on", false}, {"laptop_active", true}, {"headphones_connected", false}}, 20),
                            feature("PRESENCE", -1, {{"presence_zone", "DESK"}, {"confidence", 0.9}}, 20),
                            feature("TURN_TAKING", -1, {{"turns_per_minute", 5.0}, {"confidence", 0.8}, {"method", "SPEAKER_CHANGE"}}, 20)})
    EXPECT_EQ(fusion.ingest(event).reason, "buffered");
  std::vector<json> meeting;
  for (int second : {0, 2, 4}) meeting.push_back(*fusion.ingest(audio(second)).context_event);
  EXPECT_FALSE(meeting[0]["quiet_required"].get<bool>());
  EXPECT_FALSE(meeting[1]["quiet_required"].get<bool>());
  EXPECT_TRUE(meeting[2]["quiet_required"].get<bool>());
  EXPECT_EQ(meeting[2]["transition"], "ACTIVATED");
  EXPECT_EQ(meeting[2]["presence_zone"], "DESK");
  EXPECT_TRUE(has(meeting[2]["evidence"], "input:turn_taking:fresh"));
  EXPECT_TRUE(has(meeting[2]["evidence"], "input:simulated:audio_context,turn_taking,presence,device_state"));
  fusion.ingest(feature("PRESENCE", 5, {{"presence_zone", "COMMON"}, {"confidence", 0.9}}, 20));
  const json first = *fusion.ingest(audio(6, "QUIET")).context_event;
  const json second = *fusion.ingest(audio(8, "QUIET")).context_event;
  EXPECT_TRUE(first["quiet_required"].get<bool>());
  EXPECT_FALSE(second["quiet_required"].get<bool>());
  EXPECT_EQ(second["transition"], "DEACTIVATED");
  EXPECT_EQ(second["timestamp"], "2026-09-03T10:00:08.000Z");
}

TEST(Fusion, StaleLowConfidenceDuplicateAndOutOfOrder) {
  ctx::FeatureStreamFusion fusion;
  fusion.ingest(feature("DEVICE_STATE", -3, {{"tv_on", true}, {"laptop_active", false}, {"headphones_connected", false}}, 2));
  fusion.ingest(feature("PRESENCE", -1, {{"presence_zone", "BED"}, {"confidence", 0.2}}, 20));
  const auto result = fusion.ingest(audio(0));
  ASSERT_TRUE(result.input_status);
  EXPECT_EQ((*result.input_status)[3], (std::pair<std::string, std::string>{"DEVICE_STATE", "stale"}));
  EXPECT_EQ((*result.input_status)[2], (std::pair<std::string, std::string>{"PRESENCE", "low_confidence"}));
  EXPECT_EQ((*result.context_event)["presence_zone"], "UNKNOWN");
  EXPECT_EQ(fusion.ingest(audio(0)).reason, "duplicate");
  json older = audio(-5);
  older["event_id"] = "older";
  older["payload"]["duration_seconds"] = 1;
  EXPECT_EQ(fusion.ingest(older).reason, "out_of_order");
  json bad = audio(9);
  bad["source_node_id"] = "NODE-RPI5-02";  // pc.scenario must be PC-SIMULATION
  EXPECT_THROW(fusion.ingest(bad), ctx::ValidationError);
}

TEST(Fusion, PolicyLoadsAndRejectsUnaccepted) {
  const auto policy = ctx::ContextFusionPolicy::from_json(kPolicy);
  EXPECT_EQ(policy.quiet_audio_confidence, 0.5);
  EXPECT_EQ(policy.activation_updates, json(3));
  EXPECT_THROW(ctx::ContextFusionPolicy::from_mapping({{"engine", {{"unknown", 1}}}}), std::invalid_argument);
  EXPECT_THROW(ctx::ContextFusionPolicy::from_mapping({{"engine", {{"activation_updates", 0}}}}), std::invalid_argument);
}

TEST(Fusion, EngineRejectsOutOfOrderAndExpires) {
  ctx::ContextFusionEngine engine;
  ctx::ContextObservation observation;
  observation.audio_context = "IN_PERSON_MEETING";
  observation.audio_confidence = 0.9;
  observation.speech_ratio = 0.6;
  observation.trace_id = "t";
  observation.timestamp = ctx::parse_iso("2026-09-03T10:00:00Z");
  engine.update(observation);
  observation.timestamp += 11 * 1000000;
  const json expired = engine.update(observation);
  EXPECT_EQ(expired["evidence"][0], "state:expired");
  observation.timestamp -= 1;
  EXPECT_THROW(engine.update(observation), std::invalid_argument);
}

TEST(Models, YamnetHeadAndPipelineRunWhenExported) {
  if (!std::filesystem::exists(kYamnet) || !std::filesystem::exists(kHead))
    GTEST_SKIP() << "models not exported (run native/libs/hm_context/tools/export_*.py)";
  std::vector<float> tone(64000);
  for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = 0.2f * static_cast<float>(std::sin(2 * M_PI * 440.0 * i / 16000.0));
  ctx::YamnetModel model(kYamnet, 2);
  std::size_t frames = 0;
  EXPECT_EQ(model.embeddings(tone.data(), tone.size(), frames).size(), 8 * ctx::kEmbeddingDims);
  EXPECT_EQ(frames, 8u);
  ctx::ActivityHead head(kHead);
  EXPECT_EQ(head.n_features(), 3084u);
  const auto features = ctx::extract_features(model, tone.data(), tone.size(), head.feature_version());
  const auto probabilities = head.predict_proba(features.vector.data(), features.vector.size());
  double sum = 0;
  for (double p : probabilities) sum += p;
  EXPECT_NEAR(sum, 1.0, 1e-12);

  hm::ContextPipelineOptions options;
  options.yamnet_model = kYamnet;
  options.activity_head = kHead;
  options.fusion_policy = kPolicy;
  options.presence_zone = "DESK";
  hm::AudioContextPipeline pipeline(options);
  pipeline.warmup();
  EXPECT_EQ(pipeline.policy_version(), "audio-policy-v6.2.1-ensemble-50-50");
  EXPECT_EQ(pipeline.model_version(), "yamnet-temporal-cost-ensemble-v0.8.0-50-50");
  const auto update = pipeline.process(tone.data(), tone.size(), 1792600000000LL, 4.0);
  EXPECT_EQ(update.feature_event["timestamp"], "2026-10-21T16:26:40+00:00");
  EXPECT_EQ(update.context_event["presence_zone"], "DESK");
  EXPECT_NO_THROW(ctx::validate_context_event(update.context_event));
  EXPECT_NO_THROW(ctx::validate_feature_event(update.feature_event));
}
