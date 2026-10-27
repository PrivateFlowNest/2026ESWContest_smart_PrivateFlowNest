#include "hm/audio/ring.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace hm::audio {

AudioRing::AudioRing(std::size_t capacity, bool lossy, std::size_t block) : lossy_(lossy), block_(block) {
  if (block_ == 0) throw std::invalid_argument("ring block must be > 0");
  const std::size_t blocks = std::max<std::size_t>(2, (capacity + block_ - 1) / block_);
  data_.assign(blocks * block_, 0.0f);
}

int AudioRing::add_consumer(std::string name) {
  std::lock_guard lock(mutex_);
  consumers_.push_back(Consumer{std::move(name), written_, 0, true});
  return static_cast<int>(consumers_.size()) - 1;
}

void AudioRing::detach(int consumer) {
  {
    std::lock_guard lock(mutex_);
    consumers_.at(static_cast<std::size_t>(consumer)).attached = false;
  }
  writable_.notify_all();
}

void AudioRing::push(const float* samples, std::size_t count) {
  const auto capacity = static_cast<std::int64_t>(data_.size());
  std::size_t offset = 0;
  while (offset < count) {
    // Never write more than one ring's worth at a time (keeps lossless waiting simple).
    const std::size_t chunk = std::min(count - offset, data_.size());
    std::unique_lock lock(mutex_);
    const std::int64_t end = written_ + static_cast<std::int64_t>(chunk);
    if (!lossy_) {
      writable_.wait(lock, [&] {
        if (closed_) return true;
        for (const auto& consumer : consumers_) {
          if (consumer.attached && end - consumer.cursor > capacity) return false;
        }
        return true;
      });
      if (closed_) return;
    } else {
      // Drop whole blocks from lagging consumers so the newest audio always fits.
      for (auto& consumer : consumers_) {
        if (!consumer.attached || end - consumer.cursor <= capacity) continue;
        std::int64_t target = end - capacity;
        const auto block = static_cast<std::int64_t>(block_);
        target = ((target + block - 1) / block) * block;
        consumer.dropped += static_cast<std::uint64_t>(target - consumer.cursor);
        consumer.cursor = target;
      }
    }
    for (std::size_t i = 0; i < chunk; ++i) at(written_ + static_cast<std::int64_t>(i)) = samples[offset + i];
    written_ = end;
    offset += chunk;
    lock.unlock();
    readable_.notify_all();
  }
}

void AudioRing::close() {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  readable_.notify_all();
  writable_.notify_all();
}

bool AudioRing::read(int consumer_index, AudioBlock& out, std::size_t max_samples, int timeout_ms) {
  if (max_samples == 0) max_samples = block_;
  std::unique_lock lock(mutex_);
  Consumer& consumer = consumers_.at(static_cast<std::size_t>(consumer_index));
  auto ready = [&] { return written_ > consumer.cursor || closed_; };
  if (timeout_ms < 0) {
    readable_.wait(lock, ready);
  } else if (!readable_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
    out.samples.clear();
    out.start_sample = consumer.cursor;
    return true;
  }
  const std::int64_t available = written_ - consumer.cursor;
  if (available <= 0) return false;  // closed and drained
  const auto count = static_cast<std::size_t>(std::min<std::int64_t>(available, static_cast<std::int64_t>(max_samples)));
  out.start_sample = consumer.cursor;
  out.samples.resize(count);
  for (std::size_t i = 0; i < count; ++i) out.samples[i] = at(consumer.cursor + static_cast<std::int64_t>(i));
  consumer.cursor += static_cast<std::int64_t>(count);
  lock.unlock();
  writable_.notify_all();
  return true;
}

std::int64_t AudioRing::position() const {
  std::lock_guard lock(mutex_);
  return written_;
}

std::uint64_t AudioRing::dropped(int consumer) const {
  std::lock_guard lock(mutex_);
  return consumers_.at(static_cast<std::size_t>(consumer)).dropped;
}

}  // namespace hm::audio
