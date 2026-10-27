#include "partial_hints.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "hm/pfn.hpp"

namespace hm::perception {

namespace {

using Clock = std::chrono::steady_clock;

double since_ms(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

json stats(std::vector<double> values) {
  if (values.empty()) return nullptr;
  std::sort(values.begin(), values.end());
  auto at = [&](double q) { return values[std::min(values.size() - 1, static_cast<std::size_t>(q * values.size()))]; };
  double sum = 0;
  for (double value : values) sum += value;
  return {{"n", values.size()}, {"p50", hm::round_to(at(0.5), 3)}, {"p95", hm::round_to(at(0.95), 3)},
          {"max", hm::round_to(values.back(), 3)}, {"mean", hm::round_to(sum / static_cast<double>(values.size()), 3)}};
}

void keep_newest(std::vector<double>& values, double value) {
  if (values.size() >= 20000) values.erase(values.begin(), values.begin() + 10000);
  values.push_back(value);
}

}  // namespace

PartialHintWorker::PartialHintWorker(PartialTranscriber& transcriber, PartialIntentModel& model, Publisher& publisher,
                                     const hm::ActionMap& action_map, Options options)
    : transcriber_(transcriber), model_(model), publisher_(publisher), action_map_(action_map), options_(options) {
  thread_ = std::thread([this] { run(); });
}

PartialHintWorker::~PartialHintWorker() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    if (!options_.drain_at_end) queue_.clear();
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PartialHintWorker::begin(const std::string& trace_id) {
  {
    std::lock_guard lock(mutex_);
    overloaded_ = false;
    queue_.push_back(Item{Item::begin, trace_id, {}});
  }
  wake_.notify_one();
}

void PartialHintWorker::audio(const float* samples, std::size_t count) {
  if (count == 0) return;
  {
    std::lock_guard lock(mutex_);
    if (overloaded_) {
      dropped_samples_ += count;
      return;
    }
    const auto limit = static_cast<std::size_t>(options_.max_queue_seconds * 16000.0);
    if (!options_.drain_at_end && queued_samples_ + count > limit) {
      overloaded_ = true;
      ++overloaded_utterances_;
      dropped_samples_ += count;
      char line[160];
      std::snprintf(line, sizeof line,
                    "partial transcripts behind real time (%.1fs queued); no more hints for this utterance",
                    static_cast<double>(queued_samples_) / 16000.0);
      hm::log(hm::LogLevel::warning, "perception", line);
      return;
    }
    queued_samples_ += count;
    queue_.push_back(Item{Item::audio, {}, std::vector<float>(samples, samples + count)});
  }
  wake_.notify_one();
}

void PartialHintWorker::end() {
  {
    std::lock_guard lock(mutex_);
    if (!options_.drain_at_end) {  // drop this utterance's audio that Vosk has not reached yet
      for (auto it = queue_.end(); it != queue_.begin();) {
        --it;
        if (it->kind == Item::begin) break;
        if (it->kind == Item::audio) {
          queued_samples_ -= it->samples.size();
          dropped_samples_ += it->samples.size();
          it = queue_.erase(it);
        }
      }
    }
    queue_.push_back(Item{Item::end, {}, {}});
  }
  wake_.notify_one();
}

void PartialHintWorker::finish() {
  {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PartialHintWorker::run() {
  for (;;) {
    Item item;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) {
        idle_.notify_all();
        return;  // stop_ and nothing left
      }
      item = std::move(queue_.front());
      queue_.pop_front();
      if (item.kind == Item::audio) queued_samples_ -= item.samples.size();
      busy_ = true;
    }
    try {
      process(item);
    } catch (const std::exception& error) {  // one failing utterance must not stop the voice path
      hm::log(hm::LogLevel::error, "perception", std::string("partial transcripts failed: ") + error.what());
      session_.reset();
      gate_.reset();
    }
    {
      std::lock_guard lock(mutex_);
      busy_ = false;
      if (queue_.empty()) idle_.notify_all();
    }
  }
}

void PartialHintWorker::process(Item& item) {
  switch (item.kind) {
    case Item::begin: {
      session_.reset();
      gate_.reset();
      trace_ = item.trace;
      utterance_partials_ = utterance_hints_ = 0;
      {
        std::lock_guard lock(mutex_);
        ++utterances_;
      }
      session_ = transcriber_.start();
      gate_ = model_.gate(options_.confirmation_updates);
      return;
    }
    case Item::audio: {
      if (!session_) return;
      const auto started = Clock::now();
      const std::vector<std::string> texts = session_->accept(item.samples.data(), item.samples.size());
      const double elapsed = since_ms(started);
      {
        std::lock_guard lock(mutex_);
        ++chunks_;
        keep_newest(chunk_ms_, elapsed);
      }
      for (const auto& text : texts) on_partial(text);
      return;
    }
    case Item::end: {
      if (!session_) return;
      if (options_.drain_at_end) {
        for (const auto& text : session_->flush()) on_partial(text);
      }
      hm::log(hm::LogLevel::debug, "perception",
              "partial transcripts: utterance done, " + std::to_string(utterance_partials_) + " partial(s), " +
                  std::to_string(utterance_hints_) + " hint(s)");
      session_.reset();
      gate_.reset();
      return;
    }
  }
}

void PartialHintWorker::on_partial(const std::string& text) {
  if (!gate_) return;
  if (options_.debug_transcripts) {
    hm::log(hm::LogLevel::info, "perception", "partial (debug_transcripts) trace=" + trace_ + " '" + text + "'");
  }
  const auto started = Clock::now();
  const json event = gate_->update(text, trace_);
  const std::optional<json> hint = model_.hint(event, action_map_, options_.simulated);
  {
    std::lock_guard lock(mutex_);
    ++partials_;
    keep_newest(gate_ms_, since_ms(started));
  }
  ++utterance_partials_;
  if (!hint) return;
  publisher_.publish(hm::pfn::kPerception, *hint);
  ++utterance_hints_;
  {
    std::lock_guard lock(mutex_);
    ++hints_;
  }
  const json& attributes = (*hint)["attributes"];
  std::string candidates;
  for (const auto& device : attributes["candidate_devices"]) {
    candidates += (candidates.empty() ? "" : ",") + device.get<std::string>();
  }
  char confidence[32];
  std::snprintf(confidence, sizeof confidence, "%.2f", (*hint)["confidence"].get<double>());
  hm::log(hm::LogLevel::info, "perception",
          "partial-intent hint " + attributes["intent"].get<std::string>() + " conf=" + confidence + " -> [" +
              candidates + "] (preparation only, trace " + trace_ + ")");
}

json PartialHintWorker::summary() const {
  std::lock_guard lock(mutex_);
  return {{"utterances", utterances_},
          {"chunks", chunks_},
          {"partials", partials_},
          {"hints", hints_},
          {"dropped_seconds", hm::round_to(static_cast<double>(dropped_samples_) / 16000.0, 2)},
          {"overloaded_utterances", overloaded_utterances_},
          {"chunk_ms", stats(chunk_ms_)},
          {"gate_ms", stats(gate_ms_)}};
}

}  // namespace hm::perception
