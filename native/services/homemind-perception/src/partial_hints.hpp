// Partial-intent hints (architecture 1 and 3-1): while the voice path captures an utterance its audio is streamed
// to Vosk (PartialTranscriber), every new partial text goes through a fresh partial-intent gate per utterance
// (StreamIntentGate, two matching DETECTED updates), and each confirmed intent is published once as a
// PARTIAL_INTENT_HINT perception (execution_allowed false, requires_confirmation true, no text). Hints are read-only
// preparation: execution still only follows the Whisper final through the voice parser and core.
//
// A worker thread does the decoding so a slow Vosk never stalls the voice path (the microphone ring is lossy):
//   microphone: at the end of an utterance the audio still queued is dropped (a late hint is useless), and an
//               utterance whose queue exceeds max_queue_seconds stops streaming (logged once)
//   file mode:  everything queued is decoded, including the last short chunk (deterministic, same as Python)
// Partial texts are never logged (debug_transcripts only). The hint shares the utterance's trace_id with the
// task/llm request that the final transcript produces.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engines.hpp"
#include "service.hpp"

namespace hm::perception {

class PartialHintWorker {
 public:
  struct Options {
    bool drain_at_end = false;  // file mode
    int confirmation_updates = 2;
    double max_queue_seconds = 3.0;  // microphone only
    bool simulated = false;
    bool debug_transcripts = false;
  };

  PartialHintWorker(PartialTranscriber& transcriber, PartialIntentModel& model, Publisher& publisher,
                    const hm::ActionMap& action_map, Options options);
  ~PartialHintWorker();
  PartialHintWorker(const PartialHintWorker&) = delete;
  PartialHintWorker& operator=(const PartialHintWorker&) = delete;

  // Voice-loop side (never blocks on Vosk).
  void begin(const std::string& trace_id);
  void audio(const float* samples, std::size_t count);
  void end();
  // Waits until everything queued is processed (file mode end), then stops the thread.
  void finish();

  // {"utterances", "chunks", "partials", "hints", "dropped_seconds", "overloaded_utterances", "chunk_ms", "gate_ms"}
  json summary() const;

 private:
  struct Item {
    enum Kind { begin, audio, end } kind = begin;
    std::string trace;
    std::vector<float> samples;
  };
  void run();
  void process(Item& item);
  void on_partial(const std::string& text);

  PartialTranscriber& transcriber_;
  PartialIntentModel& model_;
  Publisher& publisher_;
  const hm::ActionMap& action_map_;
  Options options_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::deque<Item> queue_;
  std::size_t queued_samples_ = 0;
  bool busy_ = false;
  bool stop_ = false;
  bool overloaded_ = false;  // current utterance (voice-loop side)
  std::size_t dropped_samples_ = 0;
  std::size_t overloaded_utterances_ = 0;

  // worker-thread state
  std::unique_ptr<PartialSession> session_;
  std::unique_ptr<IntentGate> gate_;
  std::string trace_;
  std::size_t utterances_ = 0, chunks_ = 0, partials_ = 0, hints_ = 0;
  std::size_t utterance_partials_ = 0, utterance_hints_ = 0;
  std::vector<double> chunk_ms_;  // per accept() call (Vosk), newest 20,000
  std::vector<double> gate_ms_;   // per partial (ONNX + gate)
  std::thread thread_;
};

}  // namespace hm::perception
