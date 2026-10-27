#include "presence_feed.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace hm::perception {

PresenceFeed::PresenceFeed(Sink sink) : sink_(std::move(sink)) {}

PresenceFeed::~PresenceFeed() { stop(); }

void PresenceFeed::start(const std::filesystem::path& fifo) {
  struct stat info {};
  if (::stat(fifo.c_str(), &info) == 0) {
    if (!S_ISFIFO(info.st_mode)) throw std::runtime_error("presence_fifo exists and is not a FIFO: " + fifo.string());
  } else if (::mkfifo(fifo.c_str(), 0660) != 0 && errno != EEXIST) {  // vision may create it at the same moment
    throw std::runtime_error("cannot create presence_fifo " + fifo.string() + ": " + std::strerror(errno));
  }
  // O_RDWR keeps a writer open ourselves: homemind-vision opens, writes one line and closes for every event.
  fd_ = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) throw std::runtime_error("cannot open presence_fifo " + fifo.string() + ": " + std::strerror(errno));
  const std::string name = fifo.string();
  thread_ = std::thread([this, name] {
    char buffer[4096];
    while (!stop_) {
      pollfd item{fd_, POLLIN, 0};
      const int rc = ::poll(&item, 1, 200);
      if (rc < 0 && errno == EINTR) continue;
      if (rc < 0) {
        hm::log(hm::LogLevel::error, "perception", "presence_fifo " + name + ": poll failed: " + std::strerror(errno));
        return;
      }
      if (rc == 0) continue;
      const ssize_t n = ::read(fd_, buffer, sizeof buffer);
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      if (n <= 0) {
        hm::log(hm::LogLevel::error, "perception", "presence_fifo " + name + ": read failed");
        return;
      }
      feed(buffer, static_cast<std::size_t>(n));
    }
  });
}

void PresenceFeed::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

void PresenceFeed::feed(const char* data, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    const char c = data[i];
    if (c == '\n') {
      if (skipping_) {
        skipping_ = false;
      } else {
        line(partial_);
      }
      partial_.clear();
      continue;
    }
    if (skipping_) continue;
    if (partial_.size() >= kMaxLine) {
      {
        std::lock_guard lock(mutex_);
        ++counts_.oversized;
      }
      warn("over-long line dropped");
      partial_.clear();
      skipping_ = true;
      continue;
    }
    partial_.push_back(c);
  }
}

void PresenceFeed::line(const std::string& text) {
  if (text.find_first_not_of(" \t\r") == std::string::npos) return;
  json event;
  try {
    event = json::parse(text);
  } catch (const json::exception&) {
    event = nullptr;
  }
  if (!event.is_object()) {
    {
      std::lock_guard lock(mutex_);
      ++counts_.not_json;
    }
    warn("line is not a JSON object");
    return;
  }
  try {
    sink_(event);
  } catch (const std::exception&) {  // schema/timestamp problems: the reason may quote values, so it is not logged
    {
      std::lock_guard lock(mutex_);
      ++counts_.rejected;
    }
    warn("event rejected (not a valid rpi5-internal PRESENCE observation)");
    return;
  }
  std::lock_guard lock(mutex_);
  ++counts_.accepted;
}

void PresenceFeed::warn(const std::string& what) {
  std::size_t count = 0;
  {
    std::lock_guard lock(mutex_);
    count = ++warnings_;
  }
  // A broken writer must not flood the journal: the first 20, then every 1000th.
  if (count <= 20 || count % 1000 == 0) {
    hm::log(hm::LogLevel::warning, "perception", "presence_fifo: " + what + " (" + std::to_string(count) + " so far)");
  }
}

PresenceFeed::Counts PresenceFeed::counts() const {
  std::lock_guard lock(mutex_);
  return counts_;
}

}  // namespace hm::perception
