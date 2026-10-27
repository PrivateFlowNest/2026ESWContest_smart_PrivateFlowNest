#include "hm/audio/vad.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "hm/audio/common.hpp"

namespace hm::audio {

namespace {

int ms_field(const json& value, const std::string& key) {
  if (!value.is_number_integer()) throw std::invalid_argument("vad." + key + " must be an integer (milliseconds)");
  return value.get<int>();
}

double number_field(const json& value, const std::string& key) {
  if (!value.is_number()) throw std::invalid_argument("vad." + key + " must be a number");
  return value.get<double>();
}

// Python floor division for the (positive) frame length.
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
  std::int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

}  // namespace

EndpointConfig EndpointConfig::from_json(const json& value) {
  EndpointConfig config;
  if (value.is_null()) return config;
  if (!value.is_object()) throw std::invalid_argument("vad must be an object");
  static const std::set<std::string> known = {"frame_ms",     "start_ms",      "start_ratio",     "end_silence_ms",
                                              "max_ms",       "min_ms",        "pre_roll_ms",     "tail_ms",
                                              "threshold_db", "noise_margin_db", "noise_adapt"};
  std::vector<std::string> unknown;
  for (const auto& [key, item] : value.items()) {
    if (!known.count(key)) unknown.push_back(key);
  }
  if (!unknown.empty()) {
    std::sort(unknown.begin(), unknown.end());
    std::string names;
    for (const auto& name : unknown) names += (names.empty() ? "" : ", ") + name;
    throw std::invalid_argument("unknown vad fields: [" + names + "]");
  }
  for (const auto& [key, item] : value.items()) {
    if (key == "frame_ms") config.frame_ms = ms_field(item, key);
    else if (key == "start_ms") config.start_ms = ms_field(item, key);
    else if (key == "end_silence_ms") config.end_silence_ms = ms_field(item, key);
    else if (key == "max_ms") config.max_ms = ms_field(item, key);
    else if (key == "min_ms") config.min_ms = ms_field(item, key);
    else if (key == "pre_roll_ms") config.pre_roll_ms = ms_field(item, key);
    else if (key == "tail_ms") config.tail_ms = ms_field(item, key);
    else if (key == "start_ratio") config.start_ratio = number_field(item, key);
    else if (key == "threshold_db") config.threshold_db = number_field(item, key);
    else if (key == "noise_margin_db") config.noise_margin_db = number_field(item, key);
    else if (key == "noise_adapt") config.noise_adapt = number_field(item, key);
  }
  config.validate();
  return config;
}

void EndpointConfig::validate() const {
  if (frame_ms <= 0 || start_ms < frame_ms || end_silence_ms < frame_ms) {
    throw std::invalid_argument("vad: frame_ms > 0 and start/end windows of at least one frame required");
  }
  if (!(min_ms <= max_ms)) throw std::invalid_argument("vad: min_ms must be <= max_ms");
  if (!(start_ratio > 0.0 && start_ratio <= 1.0) || !(noise_adapt > 0.0 && noise_adapt <= 1.0)) {
    throw std::invalid_argument("vad: start_ratio and noise_adapt must be in (0, 1]");
  }
}

EnergyEndpointer::EnergyEndpointer(EndpointConfig config, bool armed_) : armed(armed_), config_(config) {
  config_.validate();
  const auto& cfg = config_;
  frame_ = static_cast<std::size_t>(16000LL * cfg.frame_ms / 1000);
  if (frame_ == 0) throw std::invalid_argument("vad: frame_ms too small");
  auto frames = [&](int ms) { return floor_div(ms, cfg.frame_ms); };
  start_frames_ = static_cast<std::size_t>(std::max<std::int64_t>(1, frames(cfg.start_ms)));
  end_frames_ = static_cast<std::size_t>(std::max<std::int64_t>(1, frames(cfg.end_silence_ms)));
  max_frames_ = static_cast<std::size_t>(std::max<std::int64_t>(1, frames(cfg.max_ms)));
  min_frames_ = static_cast<std::size_t>(std::max<std::int64_t>(1, frames(cfg.min_ms)));
  tail_frames_ = frames(cfg.tail_ms);
  pre_frames_ = static_cast<std::size_t>(std::max<std::int64_t>(static_cast<std::int64_t>(start_frames_),
                                                                 frames(cfg.pre_roll_ms)));
}

double EnergyEndpointer::threshold_db() const {
  const double floor = noise_floor_db ? *noise_floor_db : -90.0;
  return std::max(config_.threshold_db, floor + config_.noise_margin_db);
}

void EnergyEndpointer::push_history(Frame frame) {
  history_.push_back(std::move(frame));
  while (history_.size() > pre_frames_) history_.pop_front();
}

void EnergyEndpointer::push_recent(bool voiced) {
  recent_.push_back(voiced);
  while (recent_.size() > start_frames_) recent_.pop_front();
}

bool EnergyEndpointer::start_condition() const {
  const auto needed = static_cast<std::size_t>(std::ceil(config_.start_ratio * static_cast<double>(start_frames_)));
  const auto voiced = static_cast<std::size_t>(std::count(recent_.begin(), recent_.end(), true));
  return recent_.size() == start_frames_ && voiced >= needed;
}

void EnergyEndpointer::begin() {
  if (active_) return;
  active_frames_.clear();
  std::int64_t size = 0;
  voiced_frames_ = 0;
  for (auto& item : history_) {
    size += static_cast<std::int64_t>(item.samples.size());
    if (item.voiced) ++voiced_frames_;
    active_frames_.push_back(std::move(item.samples));
  }
  active_ = true;
  ++activations_;
  active_start_ = position_ - size;
  silence_run_ = 0;
  onset_seen_ = false;
  history_.clear();
  recent_.clear();
}

void EnergyEndpointer::copy_active_frames(std::size_t from_frame, std::vector<float>& out) const {
  if (!active_) return;
  for (std::size_t i = from_frame; i < active_frames_.size(); ++i) {
    out.insert(out.end(), active_frames_[i].begin(), active_frames_[i].end());
  }
}

std::vector<Utterance> EnergyEndpointer::feed(const float* samples, std::size_t count) {
  pending_.insert(pending_.end(), samples, samples + count);
  std::vector<Utterance> finished;
  std::size_t offset = 0;
  while (pending_.size() - offset >= frame_) {
    auto result = frame(pending_.data() + offset);
    offset += frame_;
    if (result) finished.push_back(std::move(*result));
  }
  pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
  return finished;
}

std::optional<Utterance> EnergyEndpointer::stop(const std::string& reason) {
  if (!active_) return std::nullopt;
  return finish(reason, false);
}

void EnergyEndpointer::reset() {
  active_ = false;
  active_frames_.clear();
  recent_.clear();
  silence_run_ = 0;
  voiced_frames_ = 0;
  onset_seen_ = false;
}

std::optional<Utterance> EnergyEndpointer::frame(const float* samples) {
  // rms = sqrt(mean(square(frame, float64)) + 1e-12); level = 20 log10(rms + 1e-10)
  thread_local std::vector<double> squares;
  squares.resize(frame_);
  for (std::size_t i = 0; i < frame_; ++i) {
    const double value = samples[i];
    squares[i] = value * value;
  }
  const double mean = pairwise_sum(squares.data(), frame_) / static_cast<double>(frame_);
  const double rms = std::sqrt(mean + 1e-12);
  const double level = 20.0 * std::log10(rms + 1e-10);
  const double threshold = threshold_db();
  const bool voiced = level >= threshold;
  const std::int64_t start = position_;
  position_ += static_cast<std::int64_t>(frame_);
  if (trace) trace(FrameTrace{start, level, threshold, voiced, active_});
  std::vector<float> copy(samples, samples + frame_);
  if (!active_) {
    if (!voiced || !noise_floor_db) {
      const double alpha = noise_floor_db ? config_.noise_adapt : 1.0;
      const double current = noise_floor_db ? *noise_floor_db : level;
      noise_floor_db = std::max(-90.0, (1.0 - alpha) * current + alpha * level);
    }
    push_history(Frame{std::move(copy), voiced});
    if (!armed) {
      recent_.clear();
      return std::nullopt;
    }
    push_recent(voiced);
    if (start_condition()) {
      active_frames_.clear();
      std::int64_t size = 0;
      voiced_frames_ = 0;
      for (auto& item : history_) {
        size += static_cast<std::int64_t>(item.samples.size());
        if (item.voiced) ++voiced_frames_;
        active_frames_.push_back(std::move(item.samples));
      }
      active_ = true;
      ++activations_;
      active_start_ = start + static_cast<std::int64_t>(frame_) - size;
      silence_run_ = 0;
      onset_seen_ = true;
      history_.clear();
      recent_.clear();
    }
    return std::nullopt;
  }
  active_frames_.push_back(std::move(copy));
  if (voiced) {
    ++voiced_frames_;
    silence_run_ = 0;
  } else {
    ++silence_run_;
  }
  if (!onset_seen_) {
    push_recent(voiced);
    if (start_condition()) onset_seen_ = true;
  }
  if (onset_seen_ && silence_run_ >= end_frames_) return finish("silence", true);
  if (active_frames_.size() >= max_frames_) return finish("max_length", false);
  return std::nullopt;
}

std::optional<Utterance> EnergyEndpointer::finish(const std::string& reason, bool trim_silence) {
  std::size_t keep = active_frames_.size();
  if (trim_silence) {
    const auto wanted = static_cast<std::int64_t>(active_frames_.size()) - static_cast<std::int64_t>(silence_run_) +
                        tail_frames_;
    keep = static_cast<std::size_t>(std::clamp<std::int64_t>(wanted, 0, static_cast<std::int64_t>(keep)));
  }
  const std::size_t voiced = voiced_frames_;
  const std::int64_t start = active_start_;
  std::vector<std::vector<float>> frames = std::move(active_frames_);
  active_frames_.clear();
  active_ = false;
  silence_run_ = 0;
  voiced_frames_ = 0;
  onset_seen_ = false;
  recent_.clear();
  if (voiced < min_frames_ || keep == 0) return std::nullopt;
  Utterance utterance;
  for (std::size_t i = 0; i < keep; ++i) {
    utterance.samples.insert(utterance.samples.end(), frames[i].begin(), frames[i].end());
  }
  utterance.start_sample = start;
  utterance.end_sample = start + static_cast<std::int64_t>(utterance.samples.size());
  utterance.reason = reason;
  return utterance;
}

}  // namespace hm::audio
