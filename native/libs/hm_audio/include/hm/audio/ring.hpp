// One capture, several in-process consumers (context windows, VAD): a single-producer ring buffer with
// one read cursor per consumer. Replaces services/runtime capture.py AudioFanout (one queue per consumer).
//   lossy (microphone): a consumer that falls more than `capacity` behind loses its oldest audio
//                       (whole blocks, counted in dropped()); the producer never blocks.
//   lossless (WAV file): the producer waits until every attached consumer has room.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hm::audio {

struct AudioBlock {
  std::vector<float> samples;
  std::int64_t start_sample = 0;  // stream index of samples[0]
};

class AudioRing {
 public:
  // capacity and block are in samples; capacity is rounded up to whole blocks.
  AudioRing(std::size_t capacity, bool lossy, std::size_t block = 1600);

  int add_consumer(std::string name);  // before the producer starts
  // A consumer that stops reading (thread ended) must detach so a lossless producer never waits on it.
  void detach(int consumer);

  void push(const float* samples, std::size_t count);
  void close();  // end of stream: readers drain what is left, then read() returns false

  // Up to `max_samples` (default one block) from the consumer's cursor. Blocks until data, close, or
  // timeout (timeout_ms < 0 = wait forever; on timeout returns true with an empty block).
  bool read(int consumer, AudioBlock& out, std::size_t max_samples = 0, int timeout_ms = -1);

  bool lossy() const { return lossy_; }
  std::int64_t position() const;
  std::uint64_t dropped(int consumer) const;  // samples lost by a lagging consumer (lossy mode)

 private:
  struct Consumer {
    std::string name;
    std::int64_t cursor = 0;
    std::uint64_t dropped = 0;
    bool attached = true;
  };
  float& at(std::int64_t position) { return data_[static_cast<std::size_t>(position) % data_.size()]; }

  const bool lossy_;
  const std::size_t block_;
  std::vector<float> data_;
  std::vector<Consumer> consumers_;
  std::int64_t written_ = 0;
  bool closed_ = false;
  mutable std::mutex mutex_;
  std::condition_variable readable_;
  std::condition_variable writable_;
};

}  // namespace hm::audio
