#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

#include "hm/action_map.hpp"
#include "hm/config.hpp"
#include "hm/heartbeat.hpp"
#include "hm/http.hpp"
#include "hm/pfn.hpp"

namespace {

const std::filesystem::path kContracts = HM_TEST_CONTRACTS_DIR;

hm::json proposal(const char* intent, hm::json device, hm::json action, hm::json parameters = hm::json::object()) {
  return {{"intent", intent}, {"device", device}, {"action", action}, {"parameters", parameters},
          {"ambiguous", false}, {"safety_flags", hm::json::array()}, {"clarification_question", nullptr}};
}

TEST(Common, UuidAndTime) {
  const std::regex uuid("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  for (int i = 0; i < 100; ++i) EXPECT_TRUE(std::regex_match(hm::new_id(), uuid));
  EXPECT_NE(hm::new_id(), hm::new_id());
  EXPECT_GT(hm::now_ms(), 1700000000000LL);
  EXPECT_DOUBLE_EQ(hm::confidence4(1.7), 1.0);
  EXPECT_DOUBLE_EQ(hm::confidence4(0.123456), 0.1235);
  // Python round(): exact binary value, ties to even
  EXPECT_DOUBLE_EQ(hm::confidence4(0.12345), 0.1235);  // 0.12345 is slightly above the tie
  EXPECT_DOUBLE_EQ(hm::confidence4(0.00005), 0.0001);
  EXPECT_DOUBLE_EQ(hm::round_to(2.675, 2), 2.67);       // 2.675 is slightly below the tie
  EXPECT_DOUBLE_EQ(hm::round_to(0.5, 0), 0.0);
  EXPECT_DOUBLE_EQ(hm::round_to(1.5, 0), 2.0);
  EXPECT_DOUBLE_EQ(hm::round_to(812.5, 3), 812.5);
}

TEST(ActionMap, LightAndDoor) {
  hm::ActionMap map(kContracts);
  auto light = map.resolve(proposal("DEVICE_CONTROL", "LIGHT", "TURN_ON"));
  EXPECT_EQ(light.kind, "device_control");
  EXPECT_EQ(*light.device_id, "LED-LIVING-01");
  EXPECT_EQ(*light.command, "set_switch");
  EXPECT_EQ(light.parameters, (hm::json{{"on", true}}));
  EXPECT_FALSE(map.is_door_open(light));

  auto door = map.resolve(proposal("DEVICE_CONTROL", "ENTRY_DOOR", "OPEN"));
  ASSERT_EQ(door.kind, "device_control");
  EXPECT_EQ(*door.device_id, "SERVO-DOOR-01");
  EXPECT_EQ(*door.command, "set_position");
  EXPECT_TRUE(map.is_door_open(door));
  auto closed = map.resolve(proposal("DEVICE_CONTROL", "ENTRY_DOOR", "CLOSE"));
  EXPECT_FALSE(map.is_door_open(closed));
}

TEST(ActionMap, NonExecutable) {
  hm::ActionMap map(kContracts);
  EXPECT_EQ(map.resolve(proposal("CLARIFY", nullptr, nullptr)).kind, "none");
  EXPECT_EQ(map.resolve(proposal("NO_ACTION", nullptr, nullptr)).reason, "NO_ACTION");
  EXPECT_EQ(map.resolve(proposal("DEVICE_CONTROL", "LIGHT", "SET_BRIGHTNESS")).kind, "unsupported");
  EXPECT_EQ(map.resolve(proposal("DEVICE_CONTROL", "ROBOT", "FLY")).reason, "no mapping for ROBOT/FLY");
  EXPECT_EQ(map.zone("DESK"), "desk");
  EXPECT_EQ(map.zone("NOWHERE"), "unknown");
}

TEST(Pfn, Builders) {
  hm::ActionMap map(kContracts);
  auto p = proposal("DEVICE_CONTROL", "LIGHT", "TURN_OFF");
  auto resolution = map.resolve(p);
  auto request = hm::pfn::task_request({.source_node_id = "NODE-RPI5-02",
                                        .intent = "device_control",
                                        .target_device = *resolution.device_id,
                                        .parameters = hm::control_parameters(resolution, "voice_parser", p),
                                        .confidence = 0.93333333});
  EXPECT_EQ(request["confidence"], 0.9333);
  EXPECT_EQ(request["parameters"]["origin"], "voice_parser");
  EXPECT_EQ(request["parameters"]["ai_action"], "TURN_OFF");

  std::string long_text;
  for (int i = 0; i < 600; ++i) long_text += "가";
  auto llm = hm::pfn::llm_request({.text = long_text, .utterance_id = hm::new_id(), .stt_model_version = "kb"});
  EXPECT_EQ(llm["transcript"]["text"].get<std::string>().size(), 500u * 3u);  // 500 code points
  EXPECT_TRUE(llm["transcript"]["stt_confidence"].is_null());
  EXPECT_FALSE(llm.contains("context"));
  EXPECT_EQ(hm::pfn::check_llm_request(llm), "");
  hm::json broken = llm;
  broken["request_id"] = "x";
  EXPECT_NE(hm::pfn::check_llm_request(broken), "");

  auto result = hm::pfn::llm_result({.request = &llm, .status = "no_action", .proposal = nullptr, .origin = "none",
                                     .latency_ms = 1.23456, .model_name = "m"});
  EXPECT_EQ(result["request_id"], llm["request_id"]);
  EXPECT_TRUE(result["ai_intent"].is_null());
  EXPECT_EQ(result["safety_flags"], hm::json::array());
  EXPECT_DOUBLE_EQ(result["model"]["latency_ms"].get<double>(), 1.235);
}

TEST(Config, LoadAndRefusePassword) {
  const auto dir = std::filesystem::temp_directory_path() / ("hm-config-" + hm::new_id());
  std::filesystem::create_directories(dir);
  {
    std::ofstream(dir / "pw") << "secret-value\n";
    std::ofstream(dir / "a.json") << R"({"base_dir": ".", "broker": {"host": "10.0.0.2", "username": "NODE-RPI5-01",
      "password_file": "pw"}, "llm_model": "x", "vad": {"min_ms": 1}})";
    std::ofstream(dir / "b.json") << R"({"broker": {"password": "no"}})";
  }
  auto config = hm::RuntimeConfig::load(dir / "a.json");
  EXPECT_EQ(config.broker.host, "10.0.0.2");
  EXPECT_EQ(config.broker.password().value(), "secret-value");
  EXPECT_EQ(config.llm_model, "x");
  EXPECT_EQ(config.extra["vad"]["min_ms"], 1);
  EXPECT_FALSE(config.extra.contains("llm_model"));
  EXPECT_THROW(hm::RuntimeConfig::load(dir / "b.json"), std::runtime_error);
  std::filesystem::remove_all(dir);
}

// One-shot HTTP server on 127.0.0.1:<ephemeral> answering with `reply`.
struct OneShotServer {
  int fd = -1;
  int port = 0;
  std::thread thread;
  std::string received;
  explicit OneShotServer(std::string reply) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    socklen_t length = sizeof addr;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length);
    port = ntohs(addr.sin_port);
    ::listen(fd, 1);
    thread = std::thread([this, reply] {
      int client = ::accept(fd, nullptr, nullptr);
      char buffer[65536];
      ssize_t n = ::recv(client, buffer, sizeof buffer, 0);
      if (n > 0) received.assign(buffer, static_cast<std::size_t>(n));
      ::send(client, reply.data(), reply.size(), 0);
      ::close(client);
    });
  }
  ~OneShotServer() {
    thread.join();
    ::close(fd);
  }
};

TEST(Http, ContentLengthAndChunked) {
  {
    OneShotServer server("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nContent-Type: application/json\r\n\r\n{\"ok\":true}");
    auto response = hm::http::post_json("http://127.0.0.1:" + std::to_string(server.port) + "/v1/x", "{}", 2000);
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "{\"ok\":true}");
    EXPECT_EQ(response.headers["content-type"], "application/json");
    server.thread.join();
    server.thread = std::thread([] {});
    EXPECT_NE(server.received.find("POST /v1/x HTTP/1.1"), std::string::npos);
  }
  {
    OneShotServer server("HTTP/1.1 503 Busy\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nwiki\r\n5\r\npedia\r\n0\r\n\r\n");
    auto response = hm::http::get("http://127.0.0.1:" + std::to_string(server.port) + "/health", 2000);
    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(response.body, "wikipedia");
    EXPECT_FALSE(response.ok());
  }
  auto refused = hm::http::get("http://127.0.0.1:1/x", 500);
  EXPECT_EQ(refused.status, 0);
  EXPECT_FALSE(refused.error.empty());
  EXPECT_THROW(hm::http::parse_url("https://x"), std::invalid_argument);
  EXPECT_EQ(hm::http::parse_url("http://[::1]:9/a").port, 9);
}

}  // namespace

TEST(Heartbeat, DocumentAndLoop) {
  const hm::json document = hm::heartbeat_document("NODE-RPI5-02", "pi5", "online", 4, 1000, 2000, "trace");
  EXPECT_EQ(document["schema_version"], "pfn.v1");
  EXPECT_EQ(document["node_id"], "NODE-RPI5-02");
  EXPECT_EQ(document["node_type"], "pi5");
  EXPECT_EQ(document["status"], "online");
  EXPECT_EQ(document["seq"], 4);
  EXPECT_EQ(document["event_id"].get<std::string>().size(), 36u);
  EXPECT_EQ(document.size(), 9u);  // heartbeat.schema.json: no other keys

  std::mutex mutex;
  std::vector<hm::json> sent;
  std::atomic<bool> connected{false};
  {
    hm::HeartbeatLoop loop(
        [&](const hm::json& beat) {
          std::lock_guard lock(mutex);
          sent.push_back(beat);
        },
        [&] { return connected.load(); }, "NODE-RPI5-02", "pi5", 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    {
      std::lock_guard lock(mutex);
      EXPECT_TRUE(sent.empty());  // nothing while disconnected
    }
    connected = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }  // destructor: one "offline"
  std::lock_guard lock(mutex);
  ASSERT_GE(sent.size(), 3u);
  EXPECT_EQ(sent.back()["status"], "offline");
  for (std::size_t i = 0; i + 1 < sent.size(); ++i) {
    EXPECT_EQ(sent[i]["status"], "online");
    EXPECT_EQ(sent[i]["seq"], static_cast<int>(i));
    EXPECT_EQ(sent[i]["trace_id"], sent.back()["trace_id"]);
  }
}
