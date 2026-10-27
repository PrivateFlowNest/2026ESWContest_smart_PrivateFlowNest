// Perception settings: the same top-level config keys and defaults as services/runtime perception.py
// DEFAULTS, plus the native-only keys (yamnet_onnx, activity_head_json, device_capabilities, ptt_fifo,
// context_threads, stt_timeout_seconds, presence_fifo, partial_intent, sound_level). Relative paths resolve against RuntimeConfig::base_dir()
// (repository root on the PC build, the config file's folder on the device); --wav paths against the cwd.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "hm/audio/vad.hpp"
#include "hm/common.hpp"
#include "hm/config.hpp"

namespace hm::perception {

inline constexpr int kSampleRate = 16000;
extern const std::vector<std::string> kAiZones;  // BED DESK BATHROOM ENTRANCE COMMON OUTDOOR UNKNOWN

// partial_intent block: Vosk partial transcripts while an utterance is captured -> partial-intent gate ->
// PARTIAL_INTENT_HINT perceptions (execution_allowed false). Off by default.
struct PartialIntentSettings {
  bool enabled = false;
  std::string vosk_library = "libvosk.so";  // dlopen: a soname (LD_LIBRARY_PATH) or a path (resolved like the others)
  std::filesystem::path vosk_model;          // unpacked Vosk model folder
  std::filesystem::path onnx_model;          // partial-intent ONNX (+ .json metadata next to it)
  int chunk_ms = 100;                        // Vosk chunk (Python VoskBackend chunk_ms)
  int confirmation_updates = 2;              // StreamIntentGate
  double max_queue_seconds = 3.0;            // microphone: audio waiting for Vosk before the utterance stops streaming
};

// sound_level block: MIC-INDOOR-01 sound_level_db telemetry from the live microphone (never in file mode), while
// privacy/state allows the microphone. An estimate without calibration (user 2026-10-25): dBFS of the newest hop of
// each window + offset_db, energy average over interval_seconds, clamped to 0..120.
struct SoundLevelSettings {
  bool enabled = false;  // off until the broker ACL (NODE-RPI5-02 telemetry) and the UI display ship
  double interval_seconds = 5.0;
  double offset_db = 95.0;  // a typical USB microphone: quiet room about 35, speech about 60-65
};

struct Settings {
  json values;  // every known key after merging and path resolution (for logging/tests)

  std::optional<std::string> audio_device;
  std::string listen_mode = "push_to_talk";  // push_to_talk | always | off
  std::string presence_zone = "UNKNOWN";
  double presence_confidence = 0.9;
  std::filesystem::path yamnet_onnx;
  std::filesystem::path activity_head_json;
  std::filesystem::path fusion_policy;
  double window_seconds = 4.0;
  double hop_seconds = 1.0;
  double republish_seconds = 10.0;
  double perception_ttl_seconds = 15.0;
  hm::audio::EndpointConfig vad;
  double ptt_timeout_seconds = 6.0;
  std::optional<std::filesystem::path> ptt_fifo;
  std::optional<std::filesystem::path> presence_fifo;  // live camera PRESENCE (homemind-vision); replaces presence_zone
  PartialIntentSettings partial_intent;
  SoundLevelSettings sound_level;
  std::filesystem::path whisper_cli_path;
  std::filesystem::path whisper_model;
  int whisper_threads = 8;
  std::optional<std::string> whisper_server_url;
  std::string stt_model_version;
  double stt_timeout_seconds = 60.0;
  std::filesystem::path device_capabilities;
  int context_threads = 2;
  bool debug_transcripts = false;
  std::string publish = "mqtt";  // mqtt | stdout
  std::vector<std::filesystem::path> wav_input;
  bool wav_fast = false;
  std::string wav_voice = "auto";  // auto | whole | vad | off | ptt (native: real-time file + push-to-talk)
  double wav_offset_seconds = 0.0;
  std::optional<double> wav_max_seconds;
  double max_whole_file_seconds = 30.0;
};

// The defaults as a JSON object (relative paths, unresolved).
json default_settings();
// DEFAULTS + known keys of `extra` (unknown keys: one warning, ignored), validated like resolve_settings().
// Throws std::invalid_argument for invalid values.
Settings resolve_settings(const json& extra, const hm::RuntimeConfig& config);

}  // namespace hm::perception
