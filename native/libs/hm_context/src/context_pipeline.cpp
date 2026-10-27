// AudioContextPipeline (contract: include/hm/context_pipeline.hpp), port of services/runtime audio/context.py.
#include "hm/context_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "hm/context/activity_head.hpp"
#include "hm/context/audio_features.hpp"
#include "hm/context/fusion.hpp"

namespace hm {

struct AudioContextPipeline::Impl {
  ContextPipelineOptions options;
  ctx::YamnetModel yamnet;
  ctx::ActivityHead head;
  ctx::FeatureStreamFusion fusion;
  std::string policy_version;
  std::string trace_id = "trace-" + new_id();
  // Live presence (submit_presence): sorted by timestamp, oldest first.
  static constexpr std::size_t kMaxPendingPresence = 64;
  mutable std::mutex presence_mutex;
  std::deque<std::pair<ctx::Micros, json>> pending_presence;
  std::size_t dropped_presence = 0;

  explicit Impl(const ContextPipelineOptions& o)
      : options(o),
        yamnet(o.yamnet_model, o.threads),
        head(o.activity_head),
        fusion(ctx::ContextFusionPolicy::from_json(o.fusion_policy).engine()) {
    if (head.head() != "activity") throw std::invalid_argument(o.activity_head.string() + " is not an activity head");
    ctx::embedding_feature_dimensions(head.feature_version());  // throws for unknown feature versions
    const json policy = read_json_file(o.fusion_policy);
    policy_version = policy.contains("config_version") ? ctx::py_str(policy.at("config_version"))
                                                       : o.fusion_policy.stem().string();
    if (options.feature_ttl_seconds < 1) throw std::invalid_argument("feature_ttl_seconds must be positive");
    // The presence override must be a contract zone (Python would fail on the first window instead).
    static const char* zones[] = {"BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"};
    if (std::none_of(std::begin(zones), std::end(zones), [&](const char* z) { return options.presence_zone == z; }))
      throw std::invalid_argument("unsupported presence_zone: " + options.presence_zone);
  }

  // Configured override (no camera in this demo) -> marked as a PC scenario input, like Python.
  json presence_event(ctx::Micros timestamp) const {
    return {
        {"schema_version", "1.0"},
        {"event_id", "presence-" + new_id()},
        {"trace_id", trace_id},
        {"timestamp", ctx::iso_python(timestamp)},
        {"valid_until", ctx::iso_python(timestamp + static_cast<ctx::Micros>(options.feature_ttl_seconds) * 1000000)},
        {"source", "pc.scenario"},
        {"source_node_id", "PC-SIMULATION"},
        {"event_type", "PRESENCE"},
        {"simulated", true},
        {"payload", {{"presence_zone", options.presence_zone}, {"confidence", options.presence_confidence}}},
    };
  }
};

AudioContextPipeline::AudioContextPipeline(const ContextPipelineOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

AudioContextPipeline::~AudioContextPipeline() = default;

void AudioContextPipeline::warmup() {
  const std::vector<float> silence(4 * ctx::kSampleRate, 0.0f);
  std::size_t frames = 0;
  impl_->yamnet.embeddings(silence.data(), silence.size(), frames);
}

ContextUpdate AudioContextPipeline::process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                                            double elapsed_seconds) {
  const auto started = std::chrono::steady_clock::now();
  const ctx::ExtractedFeatures features =
      ctx::extract_features(impl_->yamnet, window, samples, impl_->head.feature_version());
  const ctx::Prediction prediction = impl_->head.predict(features.vector.data(), features.vector.size());
  // vad_temporal_features(window)[0] clipped to [0, 1] (same function as inside the feature vector).
  const double speech_ratio = std::clamp(static_cast<double>(features.vad[0]), 0.0, 1.0);
  const ctx::Micros timestamp = static_cast<ctx::Micros>(timestamp_ms) * 1000;
  json feature_event =
      ctx::prediction_to_feature_event(prediction, impl_->trace_id, speech_ratio, elapsed_seconds, timestamp,
                                       impl_->options.feature_ttl_seconds, impl_->options.simulated);
  ContextUpdate update;
  if (impl_->options.presence_zone != "UNKNOWN") impl_->fusion.ingest(impl_->presence_event(timestamp));
  std::vector<json> due;  // live presence at or before this window (validated in submit_presence)
  {
    std::lock_guard lock(impl_->presence_mutex);
    while (!impl_->pending_presence.empty() && impl_->pending_presence.front().first <= timestamp) {
      due.push_back(std::move(impl_->pending_presence.front().second));
      impl_->pending_presence.pop_front();
    }
  }
  for (json& event : due) {
    const ctx::StreamIngestResult ingested = impl_->fusion.ingest(event);
    update.presence_events.push_back({{"event", std::move(event)}, {"result", ingested.reason}});
  }
  ctx::StreamIngestResult result = impl_->fusion.ingest(feature_event);
  if (!result.context_event) throw std::runtime_error("fusion did not fuse the audio window: " + result.reason);
  update.raw_label = prediction.label;
  update.raw_confidence = prediction.confidence;
  update.speech_ratio = speech_ratio;
  update.feature_event = std::move(feature_event);
  update.context_event = std::move(*result.context_event);
  update.latency_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return update;
}

std::string AudioContextPipeline::model_version() const { return impl_->head.model_version(); }

std::string AudioContextPipeline::policy_version() const { return impl_->policy_version; }

void AudioContextPipeline::submit_presence(const json& event) {
  if (impl_->options.presence_zone != "UNKNOWN") {
    throw std::logic_error("live presence and the presence_zone override are exclusive");
  }
  ctx::validate_feature_event(event);
  if (event.at("event_type").get<std::string>() != "PRESENCE") {
    throw ctx::ValidationError("only PRESENCE events can be submitted as live presence");
  }
  const ctx::Micros timestamp = ctx::parse_iso(event.at("timestamp").get<std::string>());
  const ctx::Micros valid_until = ctx::parse_iso(event.at("valid_until").get<std::string>());
  if (valid_until <= timestamp) throw std::invalid_argument("valid_until must be later than timestamp");
  std::lock_guard lock(impl_->presence_mutex);
  auto& pending = impl_->pending_presence;
  const auto position = std::upper_bound(pending.begin(), pending.end(), timestamp,
                                         [](ctx::Micros value, const auto& item) { return value < item.first; });
  pending.insert(position, {timestamp, event});
  if (pending.size() > Impl::kMaxPendingPresence) {
    pending.pop_front();
    ++impl_->dropped_presence;
  }
}

std::size_t AudioContextPipeline::pending_presence() const {
  std::lock_guard lock(impl_->presence_mutex);
  return impl_->pending_presence.size();
}

std::size_t AudioContextPipeline::dropped_presence() const {
  std::lock_guard lock(impl_->presence_mutex);
  return impl_->dropped_presence;
}

}  // namespace hm
