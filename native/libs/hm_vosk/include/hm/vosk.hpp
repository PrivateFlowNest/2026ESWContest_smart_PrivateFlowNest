// Vosk (0.3.45 C API) partial transcripts for NODE-RPI5-02, loaded at run time with dlopen: a service that does
// not enable partial transcripts never needs libvosk.so (26 MB x86_64, 7.5 MB aarch64) or the model.
// Port of the partial part of services/edge-ai/speech-to-text VoskBackend.transcribe() (backends.py):
//   KaldiRecognizer(model, 16000); for each chunk: AcceptWaveform(pcm16) -> true: Result() text is a finalized
//   segment (never a partial); false: PartialResult()["partial"].strip() is a new partial when non-empty and
//   different from the previous partial. Vosk's own final hypothesis is not used by the service (the README of
//   speech-to-text: Whisper owns the final transcript).
// Transcripts are returned to the caller only; this library never logs them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hm::vosk {

struct Api;  // resolved libvosk symbols

class Library {
 public:
  // path_or_soname: a path ("/data/share/homemind/lib/libvosk.so") or a soname searched by the dynamic loader
  // ("libvosk.so", LD_LIBRARY_PATH). Throws std::runtime_error with dlerror() text.
  static std::shared_ptr<Library> load(const std::string& path_or_soname, int log_level = -1);
  ~Library();
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  const Api& api() const { return *api_; }
  const std::string& name() const { return name_; }

 private:
  Library() = default;
  void* handle_ = nullptr;
  std::unique_ptr<Api> api_;
  std::string name_;
};

class Model {
 public:
  // directory: an unpacked Vosk model (am/, conf/, graph/, ivector/). Throws when Vosk cannot load it.
  Model(std::shared_ptr<Library> library, const std::filesystem::path& directory);
  ~Model();
  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;
  const std::string& version() const { return version_; }  // folder name, like VoskBackend.model_version
  double load_ms() const { return load_ms_; }
  const Library& library() const { return *library_; }
  void* raw() const { return model_; }

 private:
  std::shared_ptr<Library> library_;
  void* model_ = nullptr;
  std::string version_;
  double load_ms_ = 0.0;
};

class Recognizer {
 public:
  explicit Recognizer(std::shared_ptr<Model> model, float sample_rate = 16000.0f);
  ~Recognizer();
  Recognizer(const Recognizer&) = delete;
  Recognizer& operator=(const Recognizer&) = delete;
  bool accept(const std::int16_t* samples, std::size_t count);  // true = endpoint (call result()); throws on error
  std::string result();          // Result() JSON
  std::string partial_result();  // PartialResult() JSON
  std::string final_result();    // FinalResult() JSON

 private:
  std::shared_ptr<Model> model_;
  void* recognizer_ = nullptr;
};

// 16 kHz float in [-1, 1] -> PCM16 exactly as a 16-bit source produced it: round(x * 32768), clamped. Lossless for
// audio that came from PCM16 (WAV files, ALSA S16), which is what the Python path reads.
void to_pcm16(const float* samples, std::size_t count, std::vector<std::int16_t>& out);

// Python str.strip() (Unicode whitespace) of a UTF-8 string.
std::string py_strip(const std::string& text);

// One utterance: fixed chunks of chunk_samples (Python chunk_ms * 16), partials as VoskBackend collects them.
class PartialStream {
 public:
  explicit PartialStream(std::shared_ptr<Model> model, std::size_t chunk_samples = 1600);
  // Buffers samples and decodes every complete chunk. Returns the new partial texts (usually 0 or 1).
  std::vector<std::string> accept(const float* samples, std::size_t count);
  // Decodes the remainder (< one chunk) as a last short chunk, like Python's last slice of the file.
  std::vector<std::string> flush();
  // FinalResult() text (stripped). Probe/tests only.
  std::string final_text();

  const std::vector<std::string>& finalized() const { return finalized_; }  // Result() texts at endpoints
  std::size_t chunks() const { return chunk_ms_.size(); }
  const std::vector<double>& chunk_ms() const { return chunk_ms_; }  // per chunk: accept + result/partial + parse
  double create_ms() const { return create_ms_; }                      // recognizer construction

 private:
  std::vector<std::string> decode(const std::int16_t* samples, std::size_t count);

  std::unique_ptr<Recognizer> recognizer_;
  std::size_t chunk_;
  std::vector<std::int16_t> pending_;
  std::vector<std::int16_t> scratch_;
  std::string last_partial_;
  std::vector<std::string> finalized_;
  std::vector<double> chunk_ms_;
  double create_ms_ = 0.0;
};

}  // namespace hm::vosk
