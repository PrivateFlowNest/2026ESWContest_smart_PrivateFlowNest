// hm_stt unit tests: transcript cleaning (stt.py), whisper-server multipart request, whisper-cli arguments,
// temp WAV deletion, fallback and failure handling. Real whisper.cpp: native/tests/test_perception_stt.py.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <gtest/gtest.h>

#include "hm/stt.hpp"

namespace fs = std::filesystem;
using hm::stt::clean_transcript;

namespace {

fs::path temp_dir() {
  char pattern[] = "/tmp/hm-stt-test-XXXXXX";
  const char* made = ::mkdtemp(pattern);
  if (!made) throw std::runtime_error("mkdtemp failed");
  return made;
}

std::vector<float> tone(double seconds) {
  std::vector<float> out(static_cast<std::size_t>(seconds * 16000));
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = 0.3f * static_cast<float>(std::sin(i * 0.0864));
  return out;
}

std::string read_file(const fs::path& path) {
  std::ifstream stream(path);
  std::stringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

// Fake whisper-cli: records argv and cwd next to itself, prints `output`, exits with `status`.
fs::path fake_cli(const fs::path& dir, const std::string& output, int status = 0, int sleep_s = 0) {
  const fs::path script = dir / "whisper-cli";
  std::ofstream(script) << "#!/bin/sh\n"
                        << "here=$(dirname \"$0\")\n"
                        << "printf '%s\\n' \"$@\" > \"$here/args.txt\"\n"
                        << "pwd > \"$here/cwd.txt\"\n"
                        << "head -c 4 \"$4\" > \"$here/magic.txt\"\n"
                        << (sleep_s ? "sleep " + std::to_string(sleep_s) + "\n" : "")
                        << "printf '%b' '" << output << "'\n"
                        << "exit " << status << "\n";
  fs::permissions(script, fs::perms::owner_all);
  std::ofstream(dir / "model.bin") << "ggml";
  return script;
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::stringstream stream(text);
  for (std::string line; std::getline(stream, line);) out.push_back(line);
  return out;
}

// One-shot HTTP server on 127.0.0.1 (random port) answering one request with `status` and `body`.
class FakeServer {
 public:
  FakeServer(int status, std::string body) : status_(status), body_(std::move(body)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address);
    socklen_t length = sizeof address;
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    ::listen(fd_, 4);
    thread_ = std::thread([this] { serve(); });
  }
  ~FakeServer() {
    thread_.join();
    ::close(fd_);
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::string request;

 private:
  void serve() {
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client < 0) return;
    char buffer[65536];
    std::size_t expected = std::string::npos;
    for (;;) {
      const ssize_t n = ::recv(client, buffer, sizeof buffer, 0);
      if (n <= 0) break;
      request.append(buffer, static_cast<std::size_t>(n));
      const auto head_end = request.find("\r\n\r\n");
      if (head_end != std::string::npos && expected == std::string::npos) {
        const auto pos = request.find("Content-Length: ");
        expected = head_end + 4 + std::stoul(request.substr(pos + 16));
      }
      if (request.size() >= expected) break;
    }
    const std::string reply = "HTTP/1.1 " + std::to_string(status_) + " X\r\nContent-Type: application/json\r\n"
                              "Content-Length: " + std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
    ::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
    ::close(client);
  }
  int status_;
  std::string body_;
  int fd_ = -1;
  int port_ = 0;
  std::thread thread_;
};

}  // namespace

TEST(CleanTranscript, PythonCases) {
  EXPECT_EQ(clean_transcript("[BLANK_AUDIO]"), "");
  EXPECT_EQ(clean_transcript(" 감사합니다."), "");
  EXPECT_EQ(clean_transcript("- 불 켜줘\n"), "불 켜줘");
  EXPECT_EQ(clean_transcript("(박수) 에어컨 켜"), "에어컨 켜");
}

TEST(CleanTranscript, MoreEdges) {
  EXPECT_EQ(clean_transcript("시청해 주셔서 감사합니다!"), "");
  EXPECT_EQ(clean_transcript("…"), "");
  EXPECT_EQ(clean_transcript("?!"), "");
  EXPECT_EQ(clean_transcript("（음악）【자막】 창문 열어"), "창문 열어");
  EXPECT_EQ(clean_transcript("[ 열린 괄호만 불 꺼"), "[ 열린 괄호만 불 꺼");
  EXPECT_EQ(clean_transcript(" - 거실\r\n  - 불   꺼줘 　"), "거실 불 꺼줘");
  EXPECT_EQ(clean_transcript("TV 켜"), "TV 켜");
  EXPECT_EQ(clean_transcript("ㅋㅋ"), "");  // jamo only: no [0-9A-Za-z가-힣]
  EXPECT_EQ(clean_transcript("끝."), "");
  EXPECT_EQ(clean_transcript("끝났어"), "끝났어");
  EXPECT_EQ(hm::stt::sanitize_utf8(std::string("a\xff" "b")), "a\xEF\xBF\xBD" "b");
  EXPECT_EQ(hm::stt::strip("  x  "), "x");
}

TEST(Transcriber, RequiresABackend) {
  hm::stt::TranscriberSettings settings;
  settings.cli_path = "/nonexistent/whisper-cli";
  settings.model_path = "/nonexistent/model.bin";
  EXPECT_THROW(hm::stt::Transcriber{settings}, std::runtime_error);
  settings.server_url = "http://127.0.0.1:9";
  hm::stt::Transcriber server_only(settings);
  EXPECT_TRUE(server_only.has_server());
  EXPECT_FALSE(server_only.has_cli());
  EXPECT_EQ(server_only.model_version(), "whisper.cpp-model");
}

TEST(Transcriber, CliArgumentsAndTempWavDeletion) {
  const fs::path dir = temp_dir();
  hm::stt::TranscriberSettings settings;
  settings.cli_path = fake_cli(dir, "- 불 켜줘\\n");
  settings.model_path = dir / "model.bin";
  settings.threads = 3;
  settings.model_version = "whisper-small-homemind-lora-v4-q5_0";
  hm::stt::Transcriber transcriber(settings);
  const auto result = transcriber.transcribe(tone(1.0));
  EXPECT_EQ(result.text, "불 켜줘");
  EXPECT_EQ(result.backend, "cli");
  EXPECT_EQ(result.model_version, "whisper-small-homemind-lora-v4-q5_0");
  EXPECT_FALSE(result.confidence.has_value());
  const auto args = lines(read_file(dir / "args.txt"));
  ASSERT_EQ(args.size(), 11u);
  EXPECT_EQ(args[0], "-m");
  EXPECT_EQ(args[1], (dir / "model.bin").string());
  EXPECT_EQ(args[2], "-f");
  EXPECT_EQ(std::vector<std::string>(args.begin() + 4, args.end()),
            (std::vector<std::string>{"-l", "ko", "-t", "3", "-np", "-nt", "-ng"}));
  EXPECT_EQ(read_file(dir / "magic.txt"), "RIFF");
  EXPECT_EQ(lines(read_file(dir / "cwd.txt")).at(0), dir.string());
  const fs::path wav = args[3];
  EXPECT_EQ(wav.filename(), "utterance.wav");
  EXPECT_EQ(wav.parent_path().filename().string().rfind("hm-stt-", 0), 0u);
  EXPECT_FALSE(fs::exists(wav));
  EXPECT_FALSE(fs::exists(wav.parent_path()));
  fs::remove_all(dir);
}

TEST(Transcriber, CliFailureAndTimeoutThrowAndStillDeleteTheWav) {
  const fs::path dir = temp_dir();
  hm::stt::TranscriberSettings settings;
  settings.cli_path = fake_cli(dir, "x", 3);
  settings.model_path = dir / "model.bin";
  hm::stt::Transcriber failing(settings);
  EXPECT_THROW(failing.transcribe(tone(0.5)), std::runtime_error);
  EXPECT_FALSE(fs::exists(lines(read_file(dir / "args.txt")).at(3)));
  settings.cli_path = fake_cli(dir, "x", 0, 5);
  settings.timeout_ms = 300;
  hm::stt::Transcriber slow(settings);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_THROW(slow.transcribe(tone(0.5)), std::runtime_error);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
  fs::remove_all(dir);
}

TEST(Transcriber, ServerMultipartRequest) {
  FakeServer server(200, R"({"text":" - 에어컨 켜줘\n"})");
  hm::stt::TranscriberSettings settings;
  settings.server_url = server.url() + "/";
  settings.model_version = "m";
  hm::stt::Transcriber transcriber(settings);
  const auto result = transcriber.transcribe(tone(1.0));
  EXPECT_EQ(result.text, "에어컨 켜줘");
  EXPECT_EQ(result.backend, "server");
  const std::string& request = server.request;
  EXPECT_EQ(request.rfind("POST /inference HTTP/1.1\r\n", 0), 0u);
  EXPECT_NE(request.find("multipart/form-data; boundary="), std::string::npos);
  EXPECT_NE(request.find("name=\"response_format\"\r\n\r\njson\r\n"), std::string::npos);
  EXPECT_NE(request.find("name=\"language\"\r\n\r\nko\r\n"), std::string::npos);
  EXPECT_NE(request.find("name=\"file\"; filename=\"utterance.wav\"\r\nContent-Type: audio/wav\r\n\r\nRIFF"),
            std::string::npos);
  EXPECT_NE(request.find(std::string("data") + std::string("\x00\x7d\x00\x00", 4)), std::string::npos);  // 32000 bytes
}

TEST(Transcriber, ServerErrorFallsBackToCliOrThrows) {
  const fs::path dir = temp_dir();
  {
    FakeServer server(500, R"({"error":"x"})");
    hm::stt::TranscriberSettings settings;
    settings.server_url = server.url();
    settings.cli_path = fake_cli(dir, "창문 열어\\n");
    settings.model_path = dir / "model.bin";
    hm::stt::Transcriber transcriber(settings);
    const auto result = transcriber.transcribe(tone(0.5));
    EXPECT_EQ(result.backend, "cli");
    EXPECT_EQ(result.text, "창문 열어");
  }
  hm::stt::TranscriberSettings down;
  down.server_url = "http://127.0.0.1:9";  // nothing listens on discard
  down.timeout_ms = 2000;
  hm::stt::Transcriber unreachable(down);
  EXPECT_FALSE(unreachable.server_reachable(300));
  EXPECT_THROW(unreachable.transcribe(tone(0.5)), std::runtime_error);
  fs::remove_all(dir);
}
