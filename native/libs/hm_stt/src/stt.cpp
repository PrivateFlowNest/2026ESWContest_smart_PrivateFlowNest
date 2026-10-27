#include "hm/stt.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "hm/audio/common.hpp"
#include "hm/audio/wav.hpp"
#include "hm/common.hpp"
#include "hm/http.hpp"

namespace hm::stt {

namespace fs = std::filesystem;

namespace {

// Python: text = output.strip(); if text.startswith("-"): text = text[1:].lstrip()
std::string backend_text(const std::string& output) {
  std::string text = strip(sanitize_utf8(output));
  if (!text.empty() && text.front() == '-') text = strip(text.substr(1));
  return text;
}

// Private folder + file for one utterance; both removed in the destructor (also on errors).
class TempWav {
 public:
  explicit TempWav(const std::string& bytes) {
    const char* base = std::getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/hm-stt-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (!::mkdtemp(buffer.data())) throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
    folder_ = buffer.data();
    path_ = folder_ / "utterance.wav";
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      cleanup();
      throw std::runtime_error(std::string("cannot create temp WAV: ") + std::strerror(errno));
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
      const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        ::close(fd);
        cleanup();
        throw std::runtime_error("cannot write temp WAV");
      }
      written += static_cast<std::size_t>(n);
    }
    ::close(fd);
  }
  ~TempWav() { cleanup(); }
  TempWav(const TempWav&) = delete;
  TempWav& operator=(const TempWav&) = delete;
  const fs::path& path() const { return path_; }

 private:
  void cleanup() {
    if (!path_.empty()) ::unlink(path_.c_str());
    if (!folder_.empty()) ::rmdir(folder_.c_str());
  }
  fs::path folder_;
  fs::path path_;
};

}  // namespace

int run_capture(const std::vector<std::string>& argv, const fs::path& cwd, int timeout_ms, std::string& out) {
  if (argv.empty()) throw std::invalid_argument("empty command");
  // Everything the child needs is prepared before fork (no allocation after fork in a threaded process).
  std::vector<char*> args;
  for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
  args.push_back(nullptr);
  const std::string directory = cwd.string();
  int pipefd[2];
  if (::pipe2(pipefd, O_CLOEXEC) != 0) throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
  }
  if (pid == 0) {
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      ::dup2(null_fd, STDIN_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
    }
    ::dup2(pipefd[1], STDOUT_FILENO);
    if (!directory.empty() && ::chdir(directory.c_str()) != 0) ::_exit(126);
    ::execv(args[0], args.data());
    ::_exit(127);
  }
  ::close(pipefd[1]);
  out.clear();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool timed_out = false;
  char buffer[4096];
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) {
      timed_out = true;
      break;
    }
    pollfd item{pipefd[0], POLLIN, 0};
    const int rc = ::poll(&item, 1, static_cast<int>(left));
    if (rc < 0 && errno == EINTR) continue;
    if (rc == 0) continue;
    const ssize_t n = ::read(pipefd[0], buffer, sizeof buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(pipefd[0]);
  if (timed_out) ::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (timed_out) return 128 + SIGKILL;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

Transcriber::Transcriber(TranscriberSettings settings) : settings_(std::move(settings)) {
  if (settings_.server_url && settings_.server_url->empty()) settings_.server_url.reset();
  std::string version = settings_.model_version;
  if (version.empty()) version = "whisper.cpp-" + settings_.model_path.stem().string();
  model_version_ = hm::audio::utf8_prefix(version, 64);
  std::error_code ec;
  const bool cli_exists = !settings_.cli_path.empty() && fs::is_regular_file(settings_.cli_path, ec);
  const bool model_exists = !settings_.model_path.empty() && fs::is_regular_file(settings_.model_path, ec);
  cli_ok_ = cli_exists && model_exists;
  if (!settings_.server_url && !cli_ok_) {
    if (!cli_exists) throw std::runtime_error("whisper-cli not found: " + settings_.cli_path.string());
    throw std::runtime_error("whisper model not found: " + settings_.model_path.string());
  }
}

bool Transcriber::server_reachable(int timeout_ms) const {
  if (!settings_.server_url) return false;
  return hm::http::get(*settings_.server_url, timeout_ms).status != 0;
}

std::string Transcriber::via_server(const std::string& wav) const {
  std::string endpoint = *settings_.server_url;
  while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
  endpoint += "/inference";
  const std::vector<hm::http::FormPart> parts = {
      {"response_format", "json", "", ""},
      {"language", "ko", "", ""},
      {"file", wav, "utterance.wav", "audio/wav"},
  };
  const auto response = hm::http::post_multipart(endpoint, parts, settings_.timeout_ms);
  if (response.status == 0) throw std::runtime_error("whisper-server " + endpoint + ": " + response.error);
  if (!response.ok()) throw std::runtime_error("whisper-server " + endpoint + ": HTTP " + std::to_string(response.status));
  hm::json body;
  try {
    body = hm::json::parse(response.body);
  } catch (const std::exception&) {
    throw std::runtime_error("whisper-server returned invalid JSON");
  }
  if (!body.is_object() || !body.contains("text")) return "";
  const auto& text = body["text"];
  return backend_text(text.is_string() ? text.get<std::string>() : text.dump());
}

std::string Transcriber::via_cli(const std::string& wav) const {
  if (!cli_ok_) throw std::runtime_error("whisper-cli fallback not available");
  TempWav file(wav);
  const fs::path executable = fs::absolute(settings_.cli_path);
  const std::vector<std::string> argv = {executable.string(),
                                         "-m", fs::absolute(settings_.model_path).string(),
                                         "-f", fs::absolute(file.path()).string(),
                                         "-l", "ko",
                                         "-t", std::to_string(settings_.threads),
                                         "-np", "-nt", "-ng"};
  std::string output;
  const int status = run_capture(argv, executable.parent_path(), settings_.timeout_ms, output);
  if (status != 0) throw std::runtime_error("whisper-cli exited with status " + std::to_string(status));
  return backend_text(output);
}

SttResult Transcriber::transcribe(const float* samples, std::size_t count) {
  const auto started = std::chrono::steady_clock::now();
  const std::string wav = hm::audio::encode_pcm16_wav(samples, count, 16000);
  SttResult result;
  std::string raw;
  if (settings_.server_url) {
    try {
      raw = via_server(wav);
      result.backend = "server";
    } catch (const std::exception& error) {
      if (!cli_ok_) throw;
      hm::log(hm::LogLevel::warning, "stt", std::string(error.what()) + "; falling back to whisper-cli");
      raw = via_cli(wav);
      result.backend = "cli";
    }
  } else {
    raw = via_cli(wav);
    result.backend = "cli";
  }
  result.latency_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  result.raw_length = hm::audio::utf8_length(raw);
  result.text = clean_transcript(raw);
  result.model_version = model_version_;
  return result;
}

}  // namespace hm::stt
