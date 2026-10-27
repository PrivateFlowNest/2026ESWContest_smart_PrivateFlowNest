// JSONL probe for the PC parity tests (native/tests/test_perception_audio.py). One request per stdin line,
// one JSON answer per line. Sample arrays travel as raw little-endian float32 files ("*.f32").
//   {"op":"load","wav":p,"out":o[,"offset":s,"seconds":s]} -> {"n":N}    (load_wav[_segment]_16k_mono)
//   {"op":"resample","in":f32,"up":u,"down":d,"out":o}      -> {"n":N}    (resample_poly)
//   {"op":"stream_resample","in":f32,"up":u,"down":d,"chunk":c,"out":o} -> {"n":N}
//   {"op":"taps","up":u,"down":d}                            -> {"taps":[...],"pre_remove":k}
//   {"op":"encode","in":f32,"out":wav}                       -> {"bytes":B} (encode_pcm16_wav)
//   {"op":"vad","in":f32,"config":{...},"armed":b,"block":1600,"begin":[i..],"stop":[i..],"trace":b}
//        -> {"utterances":[{"start","end","reason","n"}],"events":[...],"trace":[[level,threshold,voiced],...]}
//      begin/stop: block indices before which begin()/stop("manual") is called; the end calls stop("flush").
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "hm/audio/resample.hpp"
#include "hm/audio/vad.hpp"
#include "hm/audio/wav.hpp"
#include "hm/common.hpp"

namespace {

using hm::json;

std::vector<float> read_f32(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read " + path);
  stream.seekg(0, std::ios::end);
  const auto bytes = static_cast<std::size_t>(stream.tellg());
  stream.seekg(0);
  std::vector<float> values(bytes / 4);
  stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * 4));
  return values;
}

void write_f32(const std::string& path, const std::vector<float>& values) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * 4));
  if (!stream) throw std::runtime_error("cannot write " + path);
}

json utterance_json(const hm::audio::Utterance& u) {
  return {{"start", u.start_sample}, {"end", u.end_sample}, {"reason", u.reason}, {"n", u.samples.size()}};
}

json vad(const json& request) {
  const auto samples = read_f32(request.at("in"));
  hm::audio::EnergyEndpointer endpointer(hm::audio::EndpointConfig::from_json(request.value("config", json::object())),
                                         request.value("armed", true));
  json trace = json::array();
  if (request.value("trace", false)) {
    endpointer.trace = [&](const hm::audio::FrameTrace& frame) {
      trace.push_back({frame.level_db, frame.threshold_db, frame.voiced});
    };
  }
  const std::size_t block = request.value("block", 1600);
  const auto begins = request.value("begin", std::vector<std::size_t>{});
  const auto stops = request.value("stop", std::vector<std::size_t>{});
  const std::set<std::size_t> begin_set(begins.begin(), begins.end());
  const std::set<std::size_t> stop_set(stops.begin(), stops.end());
  json utterances = json::array();
  json events = json::array();
  std::size_t index = 0;
  for (std::size_t offset = 0; offset < samples.size(); offset += block, ++index) {
    if (begin_set.count(index)) {
      events.push_back({{"block", index}, {"op", "begin"}, {"in_speech", endpointer.in_speech()}});
      endpointer.begin();
    }
    if (stop_set.count(index)) {
      auto manual = endpointer.stop("manual");
      events.push_back({{"block", index}, {"op", "stop"}, {"result", manual ? utterance_json(*manual) : json(nullptr)}});
      if (manual) utterances.push_back(utterance_json(*manual));
    }
    const std::size_t count = std::min(block, samples.size() - offset);
    for (const auto& u : endpointer.feed(samples.data() + offset, count)) utterances.push_back(utterance_json(u));
  }
  if (auto last = endpointer.stop("flush")) utterances.push_back(utterance_json(*last));
  json out = {{"utterances", utterances}, {"events", events},
              {"noise_floor_db", endpointer.noise_floor_db ? json(*endpointer.noise_floor_db) : json(nullptr)}};
  if (request.value("trace", false)) out["trace"] = std::move(trace);
  return out;
}

json handle(const json& request) {
  const std::string op = request.at("op");
  if (op == "load") {
    std::vector<float> samples;
    if (request.contains("offset") || request.contains("seconds")) {
      const double offset = request.value("offset", 0.0);
      const double seconds = request.contains("seconds") && !request["seconds"].is_null()
                                 ? request["seconds"].get<double>()
                                 : 24 * 3600.0;
      samples = hm::audio::load_wav_segment_16k_mono(request.at("wav").get<std::string>(), offset, offset + seconds);
    } else {
      samples = hm::audio::load_wav_16k_mono(request.at("wav").get<std::string>());
    }
    write_f32(request.at("out"), samples);
    return {{"n", samples.size()}};
  }
  if (op == "resample") {
    const auto y = hm::audio::resample_poly(read_f32(request.at("in")), request.at("up"), request.at("down"));
    write_f32(request.at("out"), y);
    return {{"n", y.size()}};
  }
  if (op == "stream_resample") {
    const auto x = read_f32(request.at("in"));
    hm::audio::StreamResampler resampler(request.at("up"), request.at("down"));
    const std::size_t chunk = request.value("chunk", 1600);
    std::vector<float> y;
    for (std::size_t offset = 0; offset < x.size(); offset += chunk) {
      resampler.push(x.data() + offset, std::min(chunk, x.size() - offset), y);
    }
    write_f32(request.at("out"), y);
    return {{"n", y.size()}};
  }
  if (op == "taps") {
    const auto filter = hm::audio::design_resampler(request.at("up"), request.at("down"));
    return {{"taps", filter.taps}, {"pre_remove", filter.pre_remove}, {"up", filter.up}, {"down", filter.down}};
  }
  if (op == "encode") {
    const auto x = read_f32(request.at("in"));
    hm::audio::write_pcm16_wav(request.at("out").get<std::string>(), x.data(), x.size());
    return {{"bytes", 44 + 2 * x.size()}};
  }
  if (op == "vad") return vad(request);
  throw std::runtime_error("unknown op " + op);
}

}  // namespace

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    json answer;
    try {
      answer = handle(json::parse(line));
    } catch (const std::exception& error) {
      answer = {{"error", error.what()}};
    }
    std::cout << hm::dump_compact(answer) << "\n" << std::flush;
  }
  return 0;
}
