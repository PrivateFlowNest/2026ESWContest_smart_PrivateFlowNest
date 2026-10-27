// PC test probe for hm_vosk (never shipped): one JSON request per stdin line, one JSON answer per line.
//   hm_vosk_probe --library /build/_deps/.../libvosk.so --model /w/services/.../vosk-model-small-ko-0.22
//   {"op": "info"}                                   -> library, model version, model load ms, RSS
//   {"op": "partials", "wav": PATH, "chunk_ms": 100}  -> partials, finalized, final, per-chunk timing
// Transcripts appear only in this test tool's answers (PC parity tests), never in a service log.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "hm/audio/wav.hpp"
#include "hm/common.hpp"
#include "hm/vosk.hpp"

namespace {

long rss_kb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) return std::stol(line.substr(6));
  }
  return -1;
}

hm::json stats(std::vector<double> values) {
  if (values.empty()) return nullptr;
  std::sort(values.begin(), values.end());
  auto at = [&](double q) { return values[std::min(values.size() - 1, static_cast<std::size_t>(q * values.size()))]; };
  double sum = 0;
  for (double v : values) sum += v;
  return {{"n", values.size()}, {"p50", at(0.5)}, {"p95", at(0.95)}, {"max", values.back()},
          {"mean", sum / static_cast<double>(values.size())}};
}

}  // namespace

int main(int argc, char** argv) {
  std::string library_path = "libvosk.so";
  std::string model_path;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string arg = argv[i];
    if (arg == "--library") library_path = argv[i + 1];
    else if (arg == "--model") model_path = argv[i + 1];
  }
  try {
    const long rss_before = rss_kb();
    auto library = hm::vosk::Library::load(library_path);
    auto model = std::make_shared<hm::vosk::Model>(library, model_path);
    const long rss_model = rss_kb();
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
      const hm::json request = hm::json::parse(line);
      const std::string op = request.value("op", "");
      hm::json out;
      if (op == "info") {
        out = {{"library", library->name()}, {"model", model->version()}, {"model_load_ms", model->load_ms()},
               {"rss_kb_before", rss_before}, {"rss_kb_after_model", rss_model}, {"rss_kb_now", rss_kb()}};
      } else if (op == "partials") {
        const std::vector<float> audio = hm::audio::load_wav_16k_mono(request.at("wav").get<std::string>());
        const int chunk_ms = request.value("chunk_ms", 100);
        const std::size_t feed = request.value("feed_samples", 1600);  // how the service hands audio over
        const auto started = std::chrono::steady_clock::now();
        hm::vosk::PartialStream stream(model, static_cast<std::size_t>(16 * chunk_ms));
        hm::json partials = hm::json::array();
        for (std::size_t offset = 0; offset < audio.size(); offset += feed) {
          for (auto& text : stream.accept(audio.data() + offset, std::min(feed, audio.size() - offset))) {
            partials.push_back(text);
          }
        }
        for (auto& text : stream.flush()) partials.push_back(text);
        const std::string final_text = stream.final_text();
        const double total_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        const double seconds = static_cast<double>(audio.size()) / 16000.0;
        out = {{"partials", partials},
               {"finalized", stream.finalized()},
               {"final", final_text},
               {"chunks", stream.chunks()},
               {"audio_seconds", seconds},
               {"recognizer_ms", stream.create_ms()},
               {"chunk_ms", stats(stream.chunk_ms())},
               {"total_ms", total_ms},
               {"rtf", total_ms / 1000.0 / std::max(seconds, 1e-9)},
               {"rss_kb", rss_kb()}};
        if (request.value("raw", false)) out["chunk_ms_raw"] = stream.chunk_ms();
      } else {
        out = {{"error", "unknown op"}};
      }
      std::cout << out.dump(-1, ' ', false, hm::json::error_handler_t::replace) << std::endl;
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "hm_vosk_probe: %s\n", error.what());
    return 1;
  }
  return 0;
}
