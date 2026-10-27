// JSONL probe for the PC parity tests (native/tests/test_partial_*.py). One request per stdin line, one JSON
// answer per stdout line. "text_b64" carries raw bytes (invalid UTF-8 for the malformed-text path).
// Test tool: it prints normalized text only for the explicit "normalize" op; events never contain text.
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hm/action_map.hpp"
#include "hm/common.hpp"
#include "hm/config.hpp"
#include "hm/partial/intent.hpp"

using hm::json;
namespace hp = hm::partial;

namespace {

std::string base64_decode(const std::string& input) {
  static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int value = 0, bits = -8;
  for (unsigned char c : input) {
    const auto position = alphabet.find(static_cast<char>(c));
    if (position == std::string::npos) continue;  // '=' and whitespace
    value = (value << 6) + static_cast<int>(position);
    bits += 6;
    if (bits >= 0) {
      out += static_cast<char>((value >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return out;
}

std::string text_of(const json& request) {
  if (request.contains("text_b64")) return base64_decode(request["text_b64"].get<std::string>());
  return request.at("text").get<std::string>();
}

std::map<std::string, std::unique_ptr<hp::IntentRuntime>> g_runtimes;

const hp::IntentRuntime& runtime(const json& request) {
  const std::string model = request.at("model").get<std::string>();
  auto& slot = g_runtimes[model];
  if (!slot) slot = std::make_unique<hp::IntentRuntime>(model);
  return *slot;
}

std::optional<double> optional_double(const json& request, const char* key) {
  return request.contains(key) && !request[key].is_null() ? std::optional(request[key].get<double>()) : std::nullopt;
}

std::optional<std::string> optional_string(const json& request, const char* key) {
  return request.contains(key) && !request[key].is_null() ? std::optional(request[key].get<std::string>()) : std::nullopt;
}

double percentile(std::vector<double> values, double q) {  // numpy.percentile (linear)
  std::sort(values.begin(), values.end());
  const double position = q / 100.0 * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(position);
  const auto upper = std::min(lower + 1, values.size() - 1);
  return values[lower] + (values[upper] - values[lower]) * (position - static_cast<double>(lower));
}

json handle(const json& request) {
  const std::string op = request.at("op").get<std::string>();
  if (op == "tables") return hp::term_tables();
  if (op == "normalize") {
    const hp::CodePoints normalized = hp::normalize_text(hp::decode_utf8(text_of(request)));
    const std::string bytes = hp::encode_utf8_replace(normalized);
    return {{"normalized", bytes},
            {"length", normalized.size()},
            {"sha256", hp::sha256_hex(bytes)},
            {"malformed", hp::has_surrogate(normalized)}};
  }
  if (op == "features") {
    const auto vector = hp::vectorize_one(hp::decode_utf8(text_of(request)), request.value("dimensions", 4096));
    json indices = json::array(), values = json::array();
    for (std::size_t i = 0; i < vector.size(); ++i) {
      if (vector[i] != 0.0f) {
        indices.push_back(i);
        values.push_back(static_cast<double>(vector[i]));
      }
    }
    return {{"indices", indices}, {"values", values}};
  }
  if (op == "probabilities") {
    const auto& model = runtime(request);
    return {{"classes", model.metadata().classes},
            {"probabilities", model.probabilities(hp::decode_utf8(text_of(request)))}};
  }
  if (op == "predict") {
    return runtime(request).predict(text_of(request), request.value("is_final", false),
                                    optional_string(request, "trace_id"), optional_double(request, "threshold"));
  }
  if (op == "stream") {  // one gate per request, like the evaluation's per-row StreamIntentGate
    hp::StreamIntentGate gate(runtime(request), request.value("confirmation_updates", 2),
                              optional_double(request, "threshold"));
    json events = json::array();
    for (const auto& update : request.at("updates")) {
      events.push_back(gate.update(text_of(update), update.value("is_final", false), optional_string(update, "trace_id")));
    }
    return {{"events", events}, {"threshold", gate.threshold()}};
  }
  if (op == "utterances") {  // native per-utterance gate reset
    hp::UtteranceIntentStream stream(runtime(request), request.value("confirmation_updates", 2),
                                     optional_double(request, "threshold"));
    json events = json::array();
    for (const auto& update : request.at("updates")) {
      events.push_back(stream.update(text_of(update), update.value("is_final", false),
                                     update.value("utterance_id", std::string())));
    }
    return {{"events", events}};
  }
  if (op == "hint") {
    static const hm::ActionMap action_map(hm::default_contracts_dir());
    const auto perception = hp::hint_perception(request.at("event"), action_map, request.value("simulated", true));
    return {{"perception", perception ? *perception : json(nullptr)}};
  }
  if (op == "bench") {
    const auto& model = runtime(request);
    const auto texts = request.at("texts").get<std::vector<std::string>>();
    const int runs = request.value("runs", 1000);
    model.predict(texts.front());
    std::vector<double> timings;
    for (int i = 0; i < runs; ++i) {
      const auto started = std::chrono::steady_clock::now();
      model.predict(texts[static_cast<std::size_t>(i) % texts.size()]);
      timings.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    }
    return {{"runs", runs}, {"p50_ms", percentile(timings, 50)}, {"p95_ms", percentile(timings, 95)},
            {"max_ms", *std::max_element(timings.begin(), timings.end())}};
  }
  throw std::invalid_argument("unknown op: " + op);
}

}  // namespace

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    json answer;
    try {
      answer = handle(json::parse(line));
    } catch (const std::exception& error) {
      answer = {{"error", error.what()}};
    }
    std::cout << hm::dump_compact(answer) << std::endl;
  }
  return 0;
}
