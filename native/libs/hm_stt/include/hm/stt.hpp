// Whisper (whisper.cpp) transcription of one endpointed utterance. Port of services/runtime audio/stt.py
// with homemind_stt WhisperCppHttpBackend / WhisperCppBackend:
//   primary   whisper-server: POST <url>/inference multipart (file=utterance.wav audio/wav,
//             response_format=json, language=ko), 60 s timeout; the WAV is built in memory (no file at all)
//   fallback  whisper-cli: <cli> -m <model> -f <wav> -l ko -t <threads> -np -nt -ng (cwd = cli folder); the WAV
//             goes to a private mkdtemp folder ($TMPDIR/hm-stt-XXXXXX, 0700) and is deleted right after
//   when a server URL is set the CLI is only used if the server call fails and the CLI + model exist.
// Transcripts are returned to the caller and never logged here.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hm::stt {

struct TranscriberSettings {
  std::optional<std::string> server_url;  // e.g. http://127.0.0.1:8178
  std::filesystem::path cli_path;         // whisper-cli executable
  std::filesystem::path model_path;       // ggml model (required by the CLI)
  int threads = 4;
  std::string model_version;  // empty -> "whisper.cpp-<model stem>"; cut to 64 code points
  int timeout_ms = 60000;
};

struct SttResult {
  std::string text;          // cleaned (clean_transcript); empty when nothing usable was heard
  std::size_t raw_length = 0;  // code points before cleaning
  double latency_ms = 0.0;
  std::string model_version;
  std::optional<double> confidence;  // whisper.cpp json output has none; always nullopt for now
  std::string backend;               // server | cli
};

// Drop whisper markers ([BLANK_AUDIO], (박수) ...) and known silence hallucinations (stt.py clean_transcript).
std::string clean_transcript(const std::string& text);
// Python str.strip() for UTF-8 text (Unicode whitespace).
std::string strip(const std::string& text);
// Replace invalid UTF-8 with U+FFFD (Python errors="replace").
std::string sanitize_utf8(const std::string& text);

class Transcriber {
 public:
  // Throws std::runtime_error when neither the server URL nor an existing CLI + model is configured
  // (Python: FileNotFoundError -> voice path disabled).
  explicit Transcriber(TranscriberSettings settings);

  // 16 kHz mono float32 samples. Throws std::runtime_error when every backend failed.
  SttResult transcribe(const float* samples, std::size_t count);
  SttResult transcribe(const std::vector<float>& samples) { return transcribe(samples.data(), samples.size()); }

  const std::string& model_version() const { return model_version_; }
  bool has_server() const { return settings_.server_url.has_value(); }
  bool has_cli() const { return cli_ok_; }
  // GET <server>/ within timeout_ms: true when whisper-server answers (start-up log only).
  bool server_reachable(int timeout_ms = 1000) const;

 private:
  std::string via_server(const std::string& wav) const;
  std::string via_cli(const std::string& wav) const;

  TranscriberSettings settings_;
  std::string model_version_;
  bool cli_ok_ = false;
};

// Runs argv[0] with arguments in `cwd`, capturing stdout (stderr discarded). Kills it after timeout_ms.
// Returns the exit status (128 + signal when killed); throws std::runtime_error when it cannot start.
int run_capture(const std::vector<std::string>& argv, const std::filesystem::path& cwd, int timeout_ms,
                std::string& out);

}  // namespace hm::stt
