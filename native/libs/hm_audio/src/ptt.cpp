#include "hm/audio/ptt.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "hm/common.hpp"

namespace hm::audio {

std::optional<PttCommand> parse_ptt_command(const std::string& line) {
  std::string word;
  for (char c : line) {
    if (!std::isspace(static_cast<unsigned char>(c))) word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (word.empty() || word == "press" || word == "toggle" || word == "enter") return PttCommand::toggle;
  if (word == "start" || word == "begin") return PttCommand::start;
  if (word == "stop" || word == "end") return PttCommand::stop;
  if (word == "cancel" || word == "abort") return PttCommand::cancel;
  return std::nullopt;
}

void PttControl::push(PttCommand command, std::string trace_id) {
  std::lock_guard lock(mutex_);
  commands_.push(PttRequest{command, std::move(trace_id)});
}

std::optional<PttCommand> PttControl::poll() {
  const auto request = poll_request();
  if (!request) return std::nullopt;
  return request->command;
}

std::optional<PttRequest> PttControl::poll_request() {
  std::lock_guard lock(mutex_);
  if (commands_.empty()) return std::nullopt;
  PttRequest request = std::move(commands_.front());
  commands_.pop();
  return request;
}

LineTrigger::~LineTrigger() { stop(); }

void LineTrigger::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (owns_fd_ && fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

void LineTrigger::start_reader(int fd, bool owns_fd, PttControl& control, std::string name) {
  fd_ = fd;
  owns_fd_ = owns_fd;
  name_ = std::move(name);
  thread_ = std::thread([this, &control] { run(control); });
}

void LineTrigger::run(PttControl& control) {
  std::string line;
  char buffer[256];
  while (!stop_) {
    pollfd item{fd_, POLLIN, 0};
    const int rc = ::poll(&item, 1, 200);
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0) {
      hm::log(hm::LogLevel::warning, "ptt", name_ + ": poll failed: " + std::strerror(errno));
      return;
    }
    if (rc == 0) continue;
    const ssize_t n = ::read(fd_, buffer, sizeof buffer);
    if (n == 0) return;  // EOF (stdin closed); a FIFO opened O_RDWR never reports EOF
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      hm::log(hm::LogLevel::warning, "ptt", name_ + ": read failed: " + std::strerror(errno));
      return;
    }
    for (ssize_t i = 0; i < n; ++i) {
      if (buffer[i] != '\n') {
        if (line.size() < 64) line.push_back(buffer[i]);
        continue;
      }
      if (auto command = parse_ptt_command(line)) {
        control.push(*command);
      } else {
        hm::log(hm::LogLevel::warning, "ptt", name_ + ": unknown command ignored");
      }
      line.clear();
    }
  }
}

StdinTrigger::StdinTrigger(PttControl& control) { start_reader(STDIN_FILENO, false, control, "stdin"); }

FifoTrigger::FifoTrigger(const std::filesystem::path& path, PttControl& control) {
  struct stat info {};
  if (::stat(path.c_str(), &info) == 0) {
    if (!S_ISFIFO(info.st_mode)) throw std::runtime_error("ptt_fifo exists and is not a FIFO: " + path.string());
  } else if (::mkfifo(path.c_str(), 0660) != 0) {
    throw std::runtime_error("cannot create ptt_fifo " + path.string() + ": " + std::strerror(errno));
  }
  // O_RDWR keeps one writer open ourselves, so writers coming and going never produce EOF.
  const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot open ptt_fifo " + path.string() + ": " + std::strerror(errno));
  start_reader(fd, true, control, "fifo " + path.string());
}

}  // namespace hm::audio
