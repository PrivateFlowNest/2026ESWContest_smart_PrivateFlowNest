// Push-to-talk triggers for the voice path.
//   console: every line on stdin is one press (Enter), like services/runtime perception.py.
//   FIFO:    a named pipe for a button or UI on the device (no terminal under systemd). One command per line:
//              "" / "press" / "toggle"  -> same as Enter (idle: start listening; listening: stop now)
//              "start"                  -> start listening (ignored while listening)
//              "stop"                   -> stop now and transcribe (ignored while idle)
//              "cancel"                 -> stop now and discard the utterance (ignored while idle)
//            e.g.  echo press > /run/homemind/ptt
//   MQTT:    pfn/v1/ai/voice/listen from the 7-inch UI (homemind-perception voice_listen.hpp): start / cancel.
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

namespace hm::audio {

enum class PttCommand { toggle, start, stop, cancel };

std::optional<PttCommand> parse_ptt_command(const std::string& line);

// A command and an optional correlation id (a remote press carries the UI's trace_id; local presses none).
struct PttRequest {
  PttCommand command = PttCommand::toggle;
  std::string trace_id;
};

class PttControl {
 public:
  void push(PttCommand command, std::string trace_id = {});
  void press() { push(PttCommand::toggle); }
  std::optional<PttCommand> poll();          // non-blocking
  std::optional<PttRequest> poll_request();  // non-blocking, with the trace id

 private:
  std::mutex mutex_;
  std::queue<PttRequest> commands_;
};

// Reads newline-terminated commands from a file descriptor in a thread (poll with timeout, clean stop).
class LineTrigger {
 public:
  ~LineTrigger();
  void stop();

 protected:
  void start_reader(int fd, bool owns_fd, PttControl& control, std::string name);

 private:
  void run(PttControl& control);
  int fd_ = -1;
  bool owns_fd_ = false;
  std::string name_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

class StdinTrigger : public LineTrigger {
 public:
  explicit StdinTrigger(PttControl& control);  // every stdin line = toggle (Python: Enter)
};

class FifoTrigger : public LineTrigger {
 public:
  // Creates the FIFO (mode 0660) when missing; refuses an existing non-FIFO path. The parent folder must exist.
  FifoTrigger(const std::filesystem::path& path, PttControl& control);
};

}  // namespace hm::audio
