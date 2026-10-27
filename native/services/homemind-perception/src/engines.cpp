#include "engines.hpp"

#include <stdexcept>

#if HM_PERCEPTION_HAVE_PARTIAL
#include "hm/partial/intent.hpp"
#include "hm/vosk.hpp"
#endif

namespace hm::perception {

void ContextEngine::submit_presence(const json&) {
  throw std::logic_error("this context engine has no live presence input");
}

namespace {

class TranscriberEngine final : public SpeechToText {
 public:
  explicit TranscriberEngine(hm::stt::TranscriberSettings settings) : transcriber_(std::move(settings)) {
    if (transcriber_.has_server()) {
      const bool up = transcriber_.server_reachable(1000);
      hm::log(up ? hm::LogLevel::info : hm::LogLevel::warning, "perception",
              std::string("whisper-server ") + (up ? "reachable" : "not reachable yet (utterances fail until it is up)") +
                  (transcriber_.has_cli() ? "; whisper-cli fallback available" : ""));
    }
  }
  hm::stt::SttResult transcribe(const std::vector<float>& samples) override { return transcriber_.transcribe(samples); }

 private:
  hm::stt::Transcriber transcriber_;
};

}  // namespace

std::unique_ptr<SpeechToText> make_transcriber(const Settings& settings) {
  hm::stt::TranscriberSettings stt;
  stt.server_url = settings.whisper_server_url;
  stt.cli_path = settings.whisper_cli_path;
  stt.model_path = settings.whisper_model;
  stt.threads = settings.whisper_threads;
  stt.model_version = settings.stt_model_version;
  stt.timeout_ms = static_cast<int>(settings.stt_timeout_seconds * 1000.0);
  return std::make_unique<TranscriberEngine>(stt);
}

#if HM_PERCEPTION_HAVE_CONTEXT
namespace {
class PipelineEngine final : public ContextEngine {
 public:
  explicit PipelineEngine(const hm::ContextPipelineOptions& options) : pipeline_(options) {}
  void warmup() override { pipeline_.warmup(); }
  hm::ContextUpdate process(const float* window, std::size_t samples, std::int64_t timestamp_ms,
                            double elapsed_seconds) override {
    return pipeline_.process(window, samples, timestamp_ms, elapsed_seconds);
  }
  std::string model_version() const override { return pipeline_.model_version(); }
  std::optional<std::string> policy_version() const override {
    const std::string version = pipeline_.policy_version();
    if (version.empty()) return std::nullopt;
    return version;
  }
  void submit_presence(const json& event) override { pipeline_.submit_presence(event); }

 private:
  hm::AudioContextPipeline pipeline_;
};
}  // namespace

bool context_engine_available() { return true; }

std::unique_ptr<ContextEngine> make_context_engine(const Settings& settings, bool simulated) {
  hm::ContextPipelineOptions options;
  options.yamnet_model = settings.yamnet_onnx;
  options.activity_head = settings.activity_head_json;
  options.fusion_policy = settings.fusion_policy;
  options.presence_zone = settings.presence_zone;
  options.presence_confidence = settings.presence_confidence;
  options.simulated = simulated;
  options.threads = settings.context_threads;
  return std::make_unique<PipelineEngine>(options);
}
#else
bool context_engine_available() { return false; }

std::unique_ptr<ContextEngine> make_context_engine(const Settings&, bool) {
  throw std::runtime_error("homemind-perception was built without libs/hm_context (context path unavailable)");
}
#endif

#if HM_PERCEPTION_HAVE_PARSER
namespace {
class ParserDecider final : public VoiceDecider {
 public:
  ParserDecider(const hm::ActionMap& action_map, hm::VoiceParserOptions options) : parser_(action_map, std::move(options)) {}
  hm::VoiceDecision handle(const std::string& text, const json& context, const std::string& stt_model_version,
                           std::optional<double> stt_confidence, std::optional<std::string> utterance_id,
                           std::optional<std::string> trace_id) override {
    return parser_.handle(text, context, stt_model_version, stt_confidence, std::move(utterance_id),
                          std::move(trace_id));
  }

 private:
  hm::VoiceParser parser_;
};
}  // namespace

bool voice_parser_available() { return true; }

std::unique_ptr<VoiceDecider> make_voice_decider(const hm::ActionMap& action_map, const Settings& settings,
                                                 const hm::RuntimeConfig& config, bool simulated) {
  hm::VoiceParserOptions options;
  options.capabilities_path = settings.device_capabilities;
  options.node_id = config.perception_node_id;
  options.llm_ttl_ms = config.llm_request_ttl_ms;
  options.simulated = simulated;
  options.contracts_dir = config.contracts_dir;
  return std::make_unique<ParserDecider>(action_map, options);
}
#else
bool voice_parser_available() { return false; }

std::unique_ptr<VoiceDecider> make_voice_decider(const hm::ActionMap&, const Settings&, const hm::RuntimeConfig&, bool) {
  throw std::runtime_error("homemind-perception was built without libs/hm_parser (voice decisions unavailable)");
}
#endif

#if HM_PERCEPTION_HAVE_PARTIAL
namespace {
class VoskSession final : public PartialSession {
 public:
  VoskSession(std::shared_ptr<hm::vosk::Model> model, std::size_t chunk) : stream_(std::move(model), chunk) {}
  std::vector<std::string> accept(const float* samples, std::size_t count) override {
    return stream_.accept(samples, count);
  }
  std::vector<std::string> flush() override { return stream_.flush(); }

 private:
  hm::vosk::PartialStream stream_;
};

class VoskTranscriber final : public PartialTranscriber {
 public:
  explicit VoskTranscriber(const PartialIntentSettings& settings)
      : model_(std::make_shared<hm::vosk::Model>(hm::vosk::Library::load(settings.vosk_library), settings.vosk_model)),
        chunk_(static_cast<std::size_t>(settings.chunk_ms) * 16) {
    hm::log(hm::LogLevel::info, "perception",
            "partial transcripts: Vosk model " + model_->version() + " loaded in " +
                std::to_string(static_cast<long>(model_->load_ms())) + " ms (" + settings.vosk_library + ")");
  }
  std::unique_ptr<PartialSession> start() override { return std::make_unique<VoskSession>(model_, chunk_); }
  std::string model_version() const override { return model_->version(); }

 private:
  std::shared_ptr<hm::vosk::Model> model_;
  std::size_t chunk_;
};

class GateAdapter final : public IntentGate {
 public:
  GateAdapter(const hm::partial::IntentRuntime& runtime, int confirmation_updates)
      : gate_(runtime, confirmation_updates) {}
  json update(const std::string& partial_text, const std::string& trace_id) override {
    return gate_.update(partial_text, false, trace_id);
  }

 private:
  hm::partial::StreamIntentGate gate_;
};

class PartialIntentRuntime final : public PartialIntentModel {
 public:
  explicit PartialIntentRuntime(const std::filesystem::path& model) : runtime_(model) {}
  std::unique_ptr<IntentGate> gate(int confirmation_updates) override {
    return std::make_unique<GateAdapter>(runtime_, confirmation_updates);
  }
  std::optional<json> hint(const json& event, const hm::ActionMap& action_map, bool simulated) override {
    return hm::partial::hint_perception(event, action_map, simulated);
  }
  std::string model_version() const override { return runtime_.metadata().model_version; }

 private:
  hm::partial::IntentRuntime runtime_;
};
}  // namespace

bool partial_intent_available() { return true; }

std::unique_ptr<PartialTranscriber> make_partial_transcriber(const Settings& settings) {
  return std::make_unique<VoskTranscriber>(settings.partial_intent);
}

std::unique_ptr<PartialIntentModel> make_partial_intent_model(const Settings& settings) {
  return std::make_unique<PartialIntentRuntime>(settings.partial_intent.onnx_model);
}
#else
bool partial_intent_available() { return false; }

std::unique_ptr<PartialTranscriber> make_partial_transcriber(const Settings&) {
  throw std::runtime_error("homemind-perception was built without libs/hm_vosk (partial transcripts unavailable)");
}

std::unique_ptr<PartialIntentModel> make_partial_intent_model(const Settings&) {
  throw std::runtime_error("homemind-perception was built without libs/hm_partial (partial intents unavailable)");
}
#endif

}  // namespace hm::perception
