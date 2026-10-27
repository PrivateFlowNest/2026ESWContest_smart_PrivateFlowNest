// JSONL probe for the PC parity tests (native/tests/test_context_*.py): one JSON request per stdin line,
// one JSON answer per stdout line. Audio and feature arrays travel as raw little-endian float32 files.
// PC x86_64 Docker only; this is not a device test.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "hm/context/activity_head.hpp"
#include "hm/context/audio_features.hpp"
#include "hm/context/fusion.hpp"
#include "hm/context/pyfmt.hpp"
#include "hm/context_pipeline.hpp"

using hm::json;
namespace ctx = hm::ctx;

namespace {

template <typename T>
std::vector<T> read_raw(const std::string& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::runtime_error("cannot read " + path);
  const auto bytes = static_cast<std::size_t>(stream.tellg());
  std::vector<T> values(bytes / sizeof(T));
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
  return values;
}

template <typename T>
void write_raw(const std::string& path, const std::vector<T>& values) {
  std::ofstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot write " + path);
  stream.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
}

struct State {
  std::map<std::string, std::vector<float>> audio;  // cached input files
  std::map<std::pair<std::string, int>, std::unique_ptr<ctx::YamnetModel>> models;
  std::map<std::string, std::unique_ptr<ctx::ActivityHead>> heads;
  std::optional<ctx::FeatureStreamFusion> fusion;
  std::optional<ctx::ContextFusionEngine> engine;
  std::unique_ptr<hm::AudioContextPipeline> pipeline;

  const std::vector<float>& input(const std::string& path) {
    auto it = audio.find(path);
    if (it == audio.end()) it = audio.emplace(path, read_raw<float>(path)).first;
    return it->second;
  }
  ctx::YamnetModel& model(const json& request) {
    const auto key = std::make_pair(request.at("model").get<std::string>(), request.value("threads", 2));
    auto it = models.find(key);
    if (it == models.end()) it = models.emplace(key, std::make_unique<ctx::YamnetModel>(key.first, key.second)).first;
    return *it->second;
  }
  ctx::ActivityHead& head(const std::string& path) {
    auto it = heads.find(path);
    if (it == heads.end()) it = heads.emplace(path, std::make_unique<ctx::ActivityHead>(path)).first;
    return *it->second;
  }
};

// (pointer, size) of request["input"][offset : offset + samples]
std::pair<const float*, std::size_t> window(State& state, const json& request) {
  const std::vector<float>& data = state.input(request.at("input").get<std::string>());
  const auto offset = request.value("offset", std::size_t{0});
  const auto samples = request.value("samples", data.size() - std::min(offset, data.size()));
  if (offset + samples > data.size()) throw std::out_of_range("window outside input");
  return {data.data() + offset, samples};
}

ctx::ContextFusionEngine engine_from(const json& request) {
  if (request.contains("policy")) return ctx::ContextFusionPolicy::from_json(request.at("policy").get<std::string>()).engine();
  if (request.contains("engine")) return ctx::ContextFusionPolicy::from_mapping(request.at("engine")).engine();
  return ctx::ContextFusionEngine();
}

json update_json(const hm::ContextUpdate& update) {
  return {{"raw_label", update.raw_label},        {"raw_confidence", update.raw_confidence},
          {"speech_ratio", update.speech_ratio},  {"feature_event", update.feature_event},
          {"context_event", update.context_event}, {"latency_ms", update.latency_ms}};
}

json stats(std::vector<double> values) {
  if (values.empty()) return json::object();
  std::sort(values.begin(), values.end());
  auto at = [&](double q) { return values[static_cast<std::size_t>(q * static_cast<double>(values.size() - 1) + 0.5)]; };
  const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  return {{"n", values.size()}, {"mean", mean}, {"p50", at(0.5)}, {"p95", at(0.95)}, {"max", values.back()},
          {"min", values.front()}};
}

json handle(State& state, const json& request) {
  const std::string op = request.at("op").get<std::string>();
  if (op == "vad") {
    const auto [data, size] = window(state, request);
    const auto features = ctx::vad_temporal_features(data, size);
    return {{"features", std::vector<float>(features.begin(), features.end())}};
  }
  if (op == "embed") {
    const auto [data, size] = window(state, request);
    std::size_t frames = 0;
    const auto embeddings = state.model(request).embeddings(data, size, frames);
    write_raw(request.at("output").get<std::string>(), embeddings);
    return {{"frames", frames}};
  }
  if (op == "features") {
    const auto [data, size] = window(state, request);
    const auto result = ctx::extract_features(state.model(request), data, size, request.at("feature_version").get<std::string>());
    write_raw(request.at("output").get<std::string>(), result.vector);
    return {{"frames", result.frames}, {"dims", result.vector.size()}};
  }
  if (op == "head") {
    ctx::ActivityHead& head = state.head(request.at("head").get<std::string>());
    const std::vector<float> rows = read_raw<float>(request.at("input").get<std::string>());
    const std::size_t n = head.n_features();
    if (rows.size() % n != 0) throw std::invalid_argument("input is not a whole number of feature rows");
    std::vector<double> probabilities;
    json labels = json::array(), confidences = json::array();
    for (std::size_t r = 0; r < rows.size() / n; ++r) {
      const ctx::Prediction prediction = head.predict(rows.data() + r * n, n);
      labels.push_back(prediction.label);
      confidences.push_back(prediction.confidence);
      for (const auto& item : prediction.class_probabilities) probabilities.push_back(item.second);
    }
    write_raw(request.at("output").get<std::string>(), probabilities);
    return {{"labels", labels}, {"confidences", confidences}, {"classes", head.classes()},
            {"model_version", head.model_version()}, {"feature_version", head.feature_version()}};
  }
  if (op == "fusion_new") {
    state.fusion.emplace(engine_from(request), request.value("minimum_turn_confidence", 0.5),
                         request.value("minimum_presence_confidence", 0.5), request.value("duplicate_window", 4096));
    return {{"ok", true}};
  }
  if (op == "fusion_ingest") return state.fusion.value().ingest(request.at("event")).debug_record();
  if (op == "engine_new") {
    state.engine.emplace(engine_from(request));
    return {{"ok", true}};
  }
  if (op == "engine_update")
    return state.engine.value().update(ctx::ContextObservation::from_mapping(request.at("observation")));
  if (op == "classify") {
    const auto result = ctx::classify(ctx::ContextObservation::from_mapping(request.at("observation")),
                                      request.value("quiet_audio_confidence", 0.0), request.value("meeting_tv_margin", 0.0));
    json scores = json::object();
    for (const auto& [label, score] : result.scores) scores[label] = score;
    return {{"context_type", result.context_type}, {"confidence", result.confidence}, {"evidence", result.evidence},
            {"scores", scores}};
  }
  if (op == "policy") return ctx::ContextFusionPolicy::from_json(request.at("path").get<std::string>(),
                                                                 request.value("require_accepted", true)).as_dict();
  if (op == "format") {
    json out = {{"round", json::array()}, {"fixed", json::array()}, {"repr", json::array()},
                {"parse_iso", json::array()}, {"iso", json::array()}, {"iso_ms", json::array()}};
    for (const auto& item : request.value("round", json::array()))
      out["round"].push_back(ctx::py_round(item[0].get<double>(), item[1].get<int>()));
    for (const auto& item : request.value("fixed", json::array()))
      out["fixed"].push_back(ctx::py_fixed(item[0].get<double>(), item[1].get<int>()));
    for (const auto& item : request.value("repr", json::array())) out["repr"].push_back(ctx::py_repr(item.get<double>()));
    for (const auto& item : request.value("parse_iso", json::array())) {
      try {
        out["parse_iso"].push_back(ctx::parse_iso(item.get<std::string>()));
      } catch (const std::invalid_argument&) {
        out["parse_iso"].push_back(nullptr);
      }
    }
    for (const auto& item : request.value("iso", json::array())) {
      out["iso"].push_back(ctx::iso_python(item.get<std::int64_t>()));
      out["iso_ms"].push_back(ctx::iso_millis_z(item.get<std::int64_t>()));
    }
    return out;
  }
  if (op == "pipeline_new") {
    hm::ContextPipelineOptions options;
    options.yamnet_model = request.at("yamnet_model").get<std::string>();
    options.activity_head = request.at("activity_head").get<std::string>();
    options.fusion_policy = request.at("fusion_policy").get<std::string>();
    options.presence_zone = request.value("presence_zone", std::string("UNKNOWN"));
    options.presence_confidence = request.value("presence_confidence", 0.9);
    options.simulated = request.value("simulated", false);
    options.feature_ttl_seconds = request.value("feature_ttl_seconds", 10);
    options.threads = request.value("threads", 2);
    state.pipeline = std::make_unique<hm::AudioContextPipeline>(options);
    if (request.value("warmup", true)) state.pipeline->warmup();
    return {{"model_version", state.pipeline->model_version()}, {"policy_version", state.pipeline->policy_version()}};
  }
  if (op == "pipeline_process") {
    const auto [data, size] = window(state, request);
    return update_json(state.pipeline->process(data, size, request.at("timestamp_ms").get<std::int64_t>(),
                                               request.at("elapsed_seconds").get<double>()));
  }
  if (op == "pipeline_run") {  // every window of a stream: {input, window, hop, base_timestamp_ms}
    const std::vector<float>& data = state.input(request.at("input").get<std::string>());
    const auto size = request.value("window", std::size_t{64000});
    const auto hop = request.value("hop", std::size_t{16000});
    const auto base = request.at("base_timestamp_ms").get<std::int64_t>();
    json updates = json::array();
    std::vector<double> latency;
    for (std::size_t start = 0; start + size <= data.size(); start += hop) {
      const std::size_t end = start + size;
      const auto update = state.pipeline->process(data.data() + start, size,
                                                  base + static_cast<std::int64_t>(end / 16), static_cast<double>(end) / 16000.0);
      latency.push_back(update.latency_ms);
      updates.push_back(update_json(update));
    }
    return {{"updates", updates}, {"latency_ms", stats(latency)}};
  }
  if (op == "bench") {  // per-stage latency on repeated windows of `input`
    ctx::YamnetModel& model = state.model(request);
    ctx::ActivityHead& head = state.head(request.at("head").get<std::string>());
    ctx::FeatureStreamFusion fusion(ctx::ContextFusionPolicy::from_json(request.at("policy").get<std::string>()).engine());
    const std::vector<float>& data = state.input(request.at("input").get<std::string>());
    const std::size_t size = 64000, hop = 16000;
    const int windows = request.value("windows", 60);
    std::vector<double> yamnet, vad, head_ms, fusion_ms, total;
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    for (int i = -request.value("warmup", 3); i < windows; ++i) {
      const std::size_t start = (static_cast<std::size_t>(std::max(i, 0)) * hop) % (data.size() - size + 1);
      const float* w = data.data() + start;
      const auto t0 = clock::now();
      std::size_t frames = 0;
      const auto embeddings = model.embeddings(w, size, frames);
      auto pooled = ctx::pool_embeddings(embeddings, frames, head.feature_version());
      const auto t1 = clock::now();
      const auto v = ctx::vad_temporal_features(w, size);
      pooled.insert(pooled.end(), v.begin(), v.end());
      const auto t2 = clock::now();
      const auto prediction = head.predict(pooled.data(), pooled.size());
      const auto t3 = clock::now();
      const auto event = ctx::prediction_to_feature_event(prediction, "trace-bench", std::clamp(double(v[0]), 0.0, 1.0),
                                                          static_cast<double>(i + 4), 1792600000000000LL + (i + 10) * 1000000LL, 10, true);
      fusion.ingest(event);
      const auto t4 = clock::now();
      if (i < 0) continue;
      yamnet.push_back(ms(t0, t1));
      vad.push_back(ms(t1, t2));
      head_ms.push_back(ms(t2, t3));
      fusion_ms.push_back(ms(t3, t4));
      total.push_back(ms(t0, t4));
    }
    return {{"threads", request.value("threads", 2)}, {"yamnet_ms", stats(yamnet)}, {"vad_ms", stats(vad)},
            {"head_ms", stats(head_ms)}, {"fusion_ms", stats(fusion_ms)}, {"total_ms", stats(total)}};
  }
  throw std::invalid_argument("unknown op " + op);
}

}  // namespace

int main() {
  State state;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    json answer;
    try {
      answer = handle(state, json::parse(line));
    } catch (const ctx::ValidationError& error) {
      answer = {{"error", {{"type", "ValidationError"}, {"message", error.what()}}}};
    } catch (const std::invalid_argument& error) {
      answer = {{"error", {{"type", "ValueError"}, {"message", error.what()}}}};
    } catch (const std::exception& error) {
      answer = {{"error", {{"type", "RuntimeError"}, {"message", error.what()}}}};
    }
    std::cout << answer.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
  }
  return 0;
}
