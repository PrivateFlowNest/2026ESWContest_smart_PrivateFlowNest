#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>

namespace hm::perception {

namespace fs = std::filesystem;

const std::vector<std::string> kAiZones = {"BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"};

namespace {

// Path keys resolved against base_dir (Python resolves its keys against the repository root).
const std::vector<std::string> kPathKeys = {"activity_model", "yamnet_model", "fusion_policy",  "whisper_cli_path",
                                            "whisper_model",  "yamnet_onnx",  "activity_head_json",
                                            "device_capabilities", "ptt_fifo", "presence_fifo"};

[[noreturn]] void invalid(const std::string& key, const std::string& what) {
  throw std::invalid_argument("perception setting " + key + ": " + what);
}

bool truthy(const json& value) {
  if (value.is_null()) return false;
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number()) return value.get<double>() != 0.0;
  if (value.is_string()) return !value.get_ref<const std::string&>().empty();
  return !value.empty();
}

double number(const json& values, const std::string& key) {
  const json& value = values.at(key);
  if (!value.is_number() || value.is_boolean()) invalid(key, "must be a number");
  const double result = value.get<double>();
  if (!std::isfinite(result)) invalid(key, "must be finite");
  return result;
}

int integer(const json& values, const std::string& key) {
  const json& value = values.at(key);
  if (!value.is_number_integer()) invalid(key, "must be an integer");
  return value.get<int>();
}

std::optional<std::string> optional_string(const json& values, const std::string& key) {
  const json& value = values.at(key);
  if (!truthy(value)) return std::nullopt;
  if (value.is_string()) return value.get<std::string>();
  if (value.is_number_integer()) return std::to_string(value.get<long long>());  // audio_device index
  invalid(key, "must be a string or null");
}

std::string string(const json& values, const std::string& key) {
  const json& value = values.at(key);
  if (value.is_null()) return "";
  if (!value.is_string()) invalid(key, "must be a string");
  return value.get<std::string>();
}

std::string choice(const json& values, const std::string& key, const std::set<std::string>& allowed,
                   const std::string& message) {
  const json& value = values.at(key);
  if (!value.is_string() || !allowed.count(value.get<std::string>())) throw std::invalid_argument(message);
  return value.get<std::string>();
}

json default_partial_intent() {
  return {
      {"enabled", false},
      {"vosk_library", "libvosk.so"},
      {"vosk_model", "services/edge-ai/speech-to-text/models/vosk-model-small-ko-0.22"},
      {"onnx_model", "services/edge-ai/partial-intent/artifacts/partial-intent-v2.onnx"},
      {"chunk_ms", 100},
      {"confirmation_updates", 2},
      {"max_queue_seconds", 3.0},
  };
}

// The partial_intent block: defaults + known keys of the configured object (unknown keys: one warning).
PartialIntentSettings resolve_partial_intent(json& block, const hm::RuntimeConfig& config) {
  json merged = default_partial_intent();
  if (!block.is_null()) {
    if (!block.is_object()) invalid("partial_intent", "must be an object");
    std::vector<std::string> unknown;
    for (const auto& [key, value] : block.items()) {
      if (merged.contains(key)) {
        merged[key] = value;
      } else {
        unknown.push_back(key);
      }
    }
    if (!unknown.empty()) {
      std::sort(unknown.begin(), unknown.end());
      std::string names;
      for (const auto& name : unknown) names += (names.empty() ? "'" : ", '") + name + "'";
      hm::log(hm::LogLevel::warning, "perception", "ignoring unknown partial_intent settings: [" + names + "]");
    }
  }
  PartialIntentSettings p;
  if (!merged["enabled"].is_boolean()) invalid("partial_intent.enabled", "must be true or false");
  p.enabled = merged["enabled"].get<bool>();
  for (const char* key : {"vosk_library", "vosk_model", "onnx_model"}) {
    if (!merged[key].is_string() || merged[key].get_ref<const std::string&>().empty()) {
      invalid(std::string("partial_intent.") + key, "must be a non-empty path string");
    }
  }
  p.vosk_library = merged["vosk_library"].get<std::string>();
  if (p.vosk_library.find('/') != std::string::npos) {  // a path; a bare soname is left to the dynamic loader
    p.vosk_library = config.resolve_path(p.vosk_library).string();
    merged["vosk_library"] = p.vosk_library;
  }
  for (const char* key : {"vosk_model", "onnx_model"}) {
    merged[key] = config.resolve_path(merged[key].get<std::string>()).string();
  }
  p.vosk_model = merged["vosk_model"].get<std::string>();
  p.onnx_model = merged["onnx_model"].get<std::string>();
  if (!merged["chunk_ms"].is_number_integer()) invalid("partial_intent.chunk_ms", "must be an integer");
  p.chunk_ms = merged["chunk_ms"].get<int>();
  if (p.chunk_ms < 10 || p.chunk_ms > 1000) invalid("partial_intent.chunk_ms", "must be within 10..1000");
  if (!merged["confirmation_updates"].is_number_integer()) {
    invalid("partial_intent.confirmation_updates", "must be an integer");
  }
  p.confirmation_updates = merged["confirmation_updates"].get<int>();
  if (p.confirmation_updates < 1) invalid("partial_intent.confirmation_updates", "must be >= 1");
  const json& queue = merged["max_queue_seconds"];
  if (!queue.is_number() || queue.is_boolean() || !(queue.get<double>() > 0.0)) {
    invalid("partial_intent.max_queue_seconds", "must be a number > 0");
  }
  p.max_queue_seconds = queue.get<double>();
  block = std::move(merged);
  return p;
}

json default_sound_level() { return {{"enabled", false}, {"interval_seconds", 5.0}, {"offset_db", 95.0}}; }

// The sound_level block: defaults + known keys of the configured object (unknown keys: one warning).
SoundLevelSettings resolve_sound_level(json& block) {
  json merged = default_sound_level();
  if (!block.is_null()) {
    if (!block.is_object()) invalid("sound_level", "must be an object");
    std::vector<std::string> unknown;
    for (const auto& [key, value] : block.items()) {
      if (merged.contains(key)) {
        merged[key] = value;
      } else {
        unknown.push_back(key);
      }
    }
    if (!unknown.empty()) {
      std::sort(unknown.begin(), unknown.end());
      std::string names;
      for (const auto& name : unknown) names += (names.empty() ? "'" : ", '") + name + "'";
      hm::log(hm::LogLevel::warning, "perception", "ignoring unknown sound_level settings: [" + names + "]");
    }
  }
  SoundLevelSettings s;
  if (!merged["enabled"].is_boolean()) invalid("sound_level.enabled", "must be true or false");
  s.enabled = merged["enabled"].get<bool>();
  s.interval_seconds = number(merged, "interval_seconds");
  if (s.interval_seconds < 1.0 || s.interval_seconds > 60.0) invalid("sound_level.interval_seconds", "must be within 1..60");
  s.offset_db = number(merged, "offset_db");
  if (s.offset_db < 0.0 || s.offset_db > 160.0) invalid("sound_level.offset_db", "must be within 0..160");
  block = std::move(merged);
  return s;
}

}  // namespace

json default_settings() {
  return {
      {"audio_device", nullptr},
      {"listen_mode", "push_to_talk"},
      {"presence_zone", "UNKNOWN"},
      {"presence_confidence", 0.9},
      {"activity_model", "services/edge-ai/audio-context/artifacts/public-balanced-v6-ensemble-50-50/activity.joblib"},
      {"yamnet_model", "services/edge-ai/audio-context/artifacts/yamnet-tfhub-1-savedmodel"},
      {"yamnet_onnx", "native/.deps/models/yamnet/yamnet-embeddings.onnx"},
      {"activity_head_json", "native/.deps/models/activity-head/public-balanced-v6-ensemble-50-50.json"},
      {"fusion_policy", "services/edge-ai/context-fusion/config/audio-policy-v6.3-hold15.json"},
      {"window_seconds", 4.0},
      {"hop_seconds", 1.0},
      {"republish_seconds", 10.0},
      {"perception_ttl_seconds", 15.0},
      {"vad", json::object()},
      {"ptt_timeout_seconds", 6.0},
      {"ptt_fifo", nullptr},
      {"presence_fifo", nullptr},
      {"partial_intent", default_partial_intent()},
      {"sound_level", default_sound_level()},
      {"whisper_cli_path", "native/.deps/x64-tools/bin/whisper-cli"},
      {"whisper_model",
       "services/edge-ai/speech-to-text/artifacts/training/whisper-small-homemind-lora-v4/"
       "ggml-whisper-small-homemind-lora-v4-q5_0.bin"},
      {"whisper_threads", 8},
      {"whisper_server_url", nullptr},
      {"stt_model_version", "whisper-small-homemind-lora-v4-q5_0"},
      {"stt_timeout_seconds", 60.0},
      {"device_capabilities", "services/exaone-edge/config/device-capabilities.json"},
      {"context_threads", 2},
      {"debug_transcripts", false},
      {"publish", "mqtt"},
      {"wav_input", nullptr},
      {"wav_fast", false},
      {"wav_voice", "auto"},
      {"wav_offset_seconds", 0.0},
      {"wav_max_seconds", nullptr},
      {"max_whole_file_seconds", 30.0},
  };
}

Settings resolve_settings(const json& extra, const hm::RuntimeConfig& config) {
  json values = default_settings();
  std::vector<std::string> unknown;
  if (extra.is_object()) {
    for (const auto& [key, value] : extra.items()) {
      if (values.contains(key)) {
        values[key] = value;
      } else {
        unknown.push_back(key);
      }
    }
  }
  if (!unknown.empty()) {
    std::sort(unknown.begin(), unknown.end());
    std::string names;
    for (const auto& name : unknown) names += (names.empty() ? "'" : ", '") + name + "'";
    hm::log(hm::LogLevel::warning, "perception", "ignoring unknown perception settings: [" + names + "]");
  }
  for (const auto& key : kPathKeys) {
    json& value = values[key];
    if (value.is_null()) continue;
    if (!value.is_string()) invalid(key, "must be a path string");
    value = config.resolve_path(value.get<std::string>()).string();
  }
  Settings s;
  s.audio_device = optional_string(values, "audio_device");
  std::string zone = truthy(values["presence_zone"]) ? string(values, "presence_zone") : "UNKNOWN";
  std::transform(zone.begin(), zone.end(), zone.begin(), [](unsigned char c) { return std::toupper(c); });
  if (std::find(kAiZones.begin(), kAiZones.end(), zone) == kAiZones.end()) {
    throw std::invalid_argument("presence_zone must be one of BED, DESK, BATHROOM, ENTRANCE, COMMON, OUTDOOR, UNKNOWN, got '" +
                                zone + "'");
  }
  if (values["presence_fifo"].is_string()) s.presence_fifo = values["presence_fifo"].get<std::string>();
  if (s.presence_fifo && zone != "UNKNOWN") {  // live presence replaces the fixed override (never both)
    hm::log(hm::LogLevel::warning, "perception",
            "presence_zone override ignored: live presence comes from presence_fifo " + s.presence_fifo->string());
    zone = "UNKNOWN";
  }
  values["presence_zone"] = zone;
  s.presence_zone = zone;
  s.partial_intent = resolve_partial_intent(values["partial_intent"], config);
  s.sound_level = resolve_sound_level(values["sound_level"]);
  s.listen_mode = choice(values, "listen_mode", {"push_to_talk", "always", "off"},
                         "listen_mode must be push_to_talk, always or off");
  s.wav_voice = choice(values, "wav_voice", {"auto", "whole", "vad", "off", "ptt"},
                       "wav_voice must be auto, whole, vad, off or ptt");
  s.publish = choice(values, "publish", {"mqtt", "stdout"}, "publish must be mqtt or stdout");
  s.presence_confidence = number(values, "presence_confidence");
  s.yamnet_onnx = string(values, "yamnet_onnx");
  s.activity_head_json = string(values, "activity_head_json");
  s.fusion_policy = string(values, "fusion_policy");
  s.window_seconds = number(values, "window_seconds");
  s.hop_seconds = number(values, "hop_seconds");
  if (s.window_seconds <= 0 || s.hop_seconds <= 0) throw std::invalid_argument("window_seconds and hop_seconds must be > 0");
  s.republish_seconds = number(values, "republish_seconds");
  s.perception_ttl_seconds = number(values, "perception_ttl_seconds");
  s.vad = hm::audio::EndpointConfig::from_json(values["vad"]);  // Python validates when the voice path starts
  s.ptt_timeout_seconds = number(values, "ptt_timeout_seconds");
  if (values["ptt_fifo"].is_string()) s.ptt_fifo = values["ptt_fifo"].get<std::string>();
  s.whisper_cli_path = string(values, "whisper_cli_path");
  s.whisper_model = string(values, "whisper_model");
  s.whisper_threads = integer(values, "whisper_threads");
  s.whisper_server_url = optional_string(values, "whisper_server_url");
  s.stt_model_version = string(values, "stt_model_version");
  s.stt_timeout_seconds = number(values, "stt_timeout_seconds");
  s.device_capabilities = string(values, "device_capabilities");
  s.context_threads = integer(values, "context_threads");
  s.debug_transcripts = truthy(values["debug_transcripts"]);
  s.wav_fast = truthy(values["wav_fast"]);
  const json& wav = values["wav_input"];
  if (wav.is_string()) {
    values["wav_input"] = json::array({wav});
  } else if (!wav.is_null() && !wav.is_array()) {
    invalid("wav_input", "must be a path or a list of paths");
  }
  if (values["wav_input"].is_array()) {
    for (const auto& item : values["wav_input"]) {
      if (!item.is_string()) invalid("wav_input", "must contain paths");
      s.wav_input.emplace_back(item.get<std::string>());
    }
  }
  s.wav_offset_seconds = truthy(values["wav_offset_seconds"]) ? number(values, "wav_offset_seconds") : 0.0;
  if (truthy(values["wav_max_seconds"])) s.wav_max_seconds = number(values, "wav_max_seconds");
  s.max_whole_file_seconds = number(values, "max_whole_file_seconds");
  s.values = std::move(values);
  return s;
}

}  // namespace hm::perception
