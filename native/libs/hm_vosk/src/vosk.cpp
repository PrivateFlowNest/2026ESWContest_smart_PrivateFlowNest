#include "hm/vosk.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace hm::vosk {

// The subset of vosk_api.h (Vosk 0.3.45) used here. Opaque handles are void*.
struct Api {
  void* (*model_new)(const char* path) = nullptr;
  void (*model_free)(void* model) = nullptr;
  void* (*recognizer_new)(void* model, float sample_rate) = nullptr;
  int (*accept_waveform_s)(void* recognizer, const short* data, int length) = nullptr;
  const char* (*result)(void* recognizer) = nullptr;
  const char* (*partial_result)(void* recognizer) = nullptr;
  const char* (*final_result)(void* recognizer) = nullptr;
  void (*recognizer_free)(void* recognizer) = nullptr;
  void (*set_log_level)(int level) = nullptr;
};

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point since) {
  return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

template <typename Fn>
void resolve(void* handle, const char* name, Fn& target) {
  ::dlerror();
  void* symbol = ::dlsym(handle, name);
  if (!symbol) throw std::runtime_error(std::string("libvosk: missing symbol ") + name);
  target = reinterpret_cast<Fn>(symbol);
}

std::string text_field(const char* document, const char* key) {
  if (!document) return "";
  const auto parsed = nlohmann::json::parse(document, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_object()) return "";
  const auto it = parsed.find(key);
  if (it == parsed.end() || !it->is_string()) return "";
  return py_strip(it->get<std::string>());
}

// Python str.isspace() code points.
bool py_space(char32_t c) {
  return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Decodes the UTF-8 code point starting at text[i] (valid UTF-8 from Vosk's JSON); returns its length.
std::size_t decode_at(const std::string& text, std::size_t i, char32_t& out) {
  const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
  const unsigned char c = byte(i);
  std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
  if (i + length > text.size()) length = 1;
  if (length == 1) {
    out = c;
    return 1;
  }
  char32_t value = c & (0xFF >> (length + 1));
  for (std::size_t k = 1; k < length; ++k) value = (value << 6) | (byte(i + k) & 0x3F);
  out = value;
  return length;
}

}  // namespace

std::string py_strip(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size()) {
    char32_t c = 0;
    const std::size_t length = decode_at(text, begin, c);
    if (!py_space(c)) break;
    begin += length;
  }
  std::size_t end = begin;  // one past the last non-space code point
  for (std::size_t i = begin; i < text.size();) {
    char32_t c = 0;
    const std::size_t length = decode_at(text, i, c);
    i += length;
    if (!py_space(c)) end = i;
  }
  return text.substr(begin, end - begin);
}

std::shared_ptr<Library> Library::load(const std::string& path_or_soname, int log_level) {
  void* handle = ::dlopen(path_or_soname.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* error = ::dlerror();
    throw std::runtime_error("cannot load " + path_or_soname + ": " + (error ? error : "dlopen failed"));
  }
  std::shared_ptr<Library> library(new Library());
  library->handle_ = handle;
  library->name_ = path_or_soname;
  library->api_ = std::make_unique<Api>();
  Api& api = *library->api_;
  resolve(handle, "vosk_model_new", api.model_new);
  resolve(handle, "vosk_model_free", api.model_free);
  resolve(handle, "vosk_recognizer_new", api.recognizer_new);
  resolve(handle, "vosk_recognizer_accept_waveform_s", api.accept_waveform_s);
  resolve(handle, "vosk_recognizer_result", api.result);
  resolve(handle, "vosk_recognizer_partial_result", api.partial_result);
  resolve(handle, "vosk_recognizer_final_result", api.final_result);
  resolve(handle, "vosk_recognizer_free", api.recognizer_free);
  resolve(handle, "vosk_set_log_level", api.set_log_level);
  api.set_log_level(log_level);  // Python VoskBackend: SetLogLevel(-1)
  return library;
}

Library::~Library() {
  if (handle_) ::dlclose(handle_);
}

Model::Model(std::shared_ptr<Library> library, const std::filesystem::path& directory) : library_(std::move(library)) {
  if (!std::filesystem::is_directory(directory)) {
    throw std::runtime_error("Vosk model folder not found: " + directory.string());
  }
  const auto started = Clock::now();
  model_ = library_->api().model_new(directory.c_str());
  load_ms_ = elapsed_ms(started);
  if (!model_) throw std::runtime_error("Vosk could not load the model in " + directory.string());
  version_ = directory.filename().string();
  if (version_.empty()) version_ = directory.parent_path().filename().string();
}

Model::~Model() {
  if (model_) library_->api().model_free(model_);
}

Recognizer::Recognizer(std::shared_ptr<Model> model, float sample_rate) : model_(std::move(model)) {
  recognizer_ = model_->library().api().recognizer_new(model_->raw(), sample_rate);
  if (!recognizer_) throw std::runtime_error("Vosk could not create a recognizer");
}

Recognizer::~Recognizer() {
  if (recognizer_) model_->library().api().recognizer_free(recognizer_);
}

bool Recognizer::accept(const std::int16_t* samples, std::size_t count) {
  static_assert(sizeof(short) == sizeof(std::int16_t));
  const int rc = model_->library().api().accept_waveform_s(recognizer_, reinterpret_cast<const short*>(samples),
                                                           static_cast<int>(count));
  if (rc < 0) throw std::runtime_error("Vosk accept_waveform failed");
  return rc == 1;
}

std::string Recognizer::result() {
  const char* text = model_->library().api().result(recognizer_);
  return text ? text : "";
}

std::string Recognizer::partial_result() {
  const char* text = model_->library().api().partial_result(recognizer_);
  return text ? text : "";
}

std::string Recognizer::final_result() {
  const char* text = model_->library().api().final_result(recognizer_);
  return text ? text : "";
}

void to_pcm16(const float* samples, std::size_t count, std::vector<std::int16_t>& out) {
  out.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    const float scaled = std::nearbyint(samples[i] * 32768.0f);
    out[i] = static_cast<std::int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
  }
}

PartialStream::PartialStream(std::shared_ptr<Model> model, std::size_t chunk_samples)
    : chunk_(chunk_samples == 0 ? 1 : chunk_samples) {
  const auto started = Clock::now();
  recognizer_ = std::make_unique<Recognizer>(std::move(model), 16000.0f);
  create_ms_ = elapsed_ms(started);
}

std::vector<std::string> PartialStream::decode(const std::int16_t* samples, std::size_t count) {
  const auto started = Clock::now();
  std::vector<std::string> out;
  if (recognizer_->accept(samples, count)) {
    std::string text = text_field(recognizer_->result().c_str(), "text");
    if (!text.empty()) finalized_.push_back(std::move(text));
  } else {
    std::string text = text_field(recognizer_->partial_result().c_str(), "partial");
    if (!text.empty() && text != last_partial_) {
      last_partial_ = text;
      out.push_back(std::move(text));
    }
  }
  chunk_ms_.push_back(elapsed_ms(started));
  return out;
}

std::vector<std::string> PartialStream::accept(const float* samples, std::size_t count) {
  std::vector<std::string> out;
  to_pcm16(samples, count, scratch_);
  pending_.insert(pending_.end(), scratch_.begin(), scratch_.end());
  std::size_t offset = 0;
  while (pending_.size() - offset >= chunk_) {
    for (auto& text : decode(pending_.data() + offset, chunk_)) out.push_back(std::move(text));
    offset += chunk_;
  }
  pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
  return out;
}

std::vector<std::string> PartialStream::flush() {
  if (pending_.empty()) return {};
  std::vector<std::int16_t> rest;
  rest.swap(pending_);
  return decode(rest.data(), rest.size());
}

std::string PartialStream::final_text() { return text_field(recognizer_->final_result().c_str(), "text"); }

}  // namespace hm::vosk
