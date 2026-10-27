// PC integration probe: one JSON request per stdin line, one answer per line.
//   {"wav": path, "server_url": url | null, "cli": path, "model": path, "threads": n}
//     -> {"text": ..., "latency_ms": ..., "backend": ..., "seconds": ..., "model_version": ...}
// The transcript goes to stdout for the test to score; this tool is for PC tests only (never on the device).
#include <iostream>
#include <string>

#include "hm/audio/wav.hpp"
#include "hm/common.hpp"
#include "hm/stt.hpp"

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    hm::json answer;
    try {
      const auto request = hm::json::parse(line);
      hm::stt::TranscriberSettings settings;
      if (request.contains("server_url") && !request["server_url"].is_null()) {
        settings.server_url = request["server_url"].get<std::string>();
      }
      settings.cli_path = request.value("cli", std::string());
      settings.model_path = request.value("model", std::string());
      settings.threads = request.value("threads", 4);
      settings.model_version = request.value("model_version", std::string());
      hm::stt::Transcriber transcriber(settings);
      const auto samples = hm::audio::load_wav_16k_mono(request.at("wav").get<std::string>());
      const auto result = transcriber.transcribe(samples);
      answer = {{"text", result.text}, {"latency_ms", result.latency_ms}, {"backend", result.backend},
                {"seconds", static_cast<double>(samples.size()) / 16000.0}, {"model_version", result.model_version},
                {"raw_length", result.raw_length}};
    } catch (const std::exception& error) {
      answer = {{"error", error.what()}};
    }
    std::cout << hm::dump_compact(answer) << "\n" << std::flush;
  }
  return 0;
}
