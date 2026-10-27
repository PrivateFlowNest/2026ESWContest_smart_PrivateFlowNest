// Unit tests for homemind-mobile-api (PC x86_64 Docker build; SIMULATION, no broker, no device).
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <thread>

#include "api.hpp"
#include "crypto.hpp"
#include "pairing_relay.hpp"
#include "phones_relay.hpp"
#include "service_config.hpp"
#include "terminal_qr.hpp"
#include "tls.hpp"

namespace fs = std::filesystem;
using hm::json;
using namespace hm::mobile;

namespace {

const fs::path kRegistry = fs::path(HM_TEST_CONTRACTS_DIR) / "pfn.v1" / "device-registry.json";

fs::path temp_dir(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / ("hm-mobile-api-test-" + name + "-" + hm::crypto::random_hex(4));
  fs::remove_all(dir);
  return dir;
}

unsigned mode_of(const fs::path& path) {
  struct stat st {};
  EXPECT_EQ(::stat(path.c_str(), &st), 0) << path;
  return st.st_mode & 0777;
}

std::string read_file(const fs::path& path) {
  std::ifstream stream(path);
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

class FakePublisher : public Publisher {
 public:
  bool up = true;
  std::vector<std::pair<std::string, json>> published;
  bool connected() const override { return up; }
  bool publish(const std::string& topic, const json& document) override {
    published.emplace_back(topic, document);
    return true;
  }
  std::vector<json> on(const std::string& topic) const {
    std::vector<json> out;
    for (const auto& [t, d] : published) {
      if (t == topic) out.push_back(d);
    }
    return out;
  }
};

AuthConfig auth_config(const fs::path& dir) {
  AuthConfig config;
  config.data_dir = dir;
  config.public_base_url = "http://192.0.2.10:8000";
  config.max_failures_per_client = 3;
  config.max_failures_global = 5;
  config.failure_window_s = 60;
  return config;
}

// Whole API on a temp data dir with a fake MQTT side.
struct Fixture {
  explicit Fixture(bool mqtt = true)
      : dir(temp_dir("api")),
        registry(kRegistry),
        store(registry),
        tokens(dir),
        pairing(auth_config(dir), tokens),
        bridge(BridgeConfig{mqtt, "homemind-mobile-api", "NODE-RPI4-01"}, &publisher,
               [this](const TrackedTask& task) {
                 if (task.state == "completed") store.confirm_command(task.device_id, task.command);
                 store.task_changed(task.device_id, task.task_id, task.state, task.failure_field("code"));
               },
               [this](const std::map<std::string, std::string>& s) { store.set_node_status(s); }),
        api(registry, store, bridge, tokens, pairing, approvals) {
    pairing.prepare();
    tokens.issue("test phone", token);
  }
  ~Fixture() { fs::remove_all(dir); }

  HttpResponse call(const std::string& method, const std::string& path, const json& body = nullptr,
                    std::optional<std::string> bearer = std::nullopt) {
    HttpRequest request;
    request.method = method;
    request.path = path;
    request.authorization = "Bearer " + bearer.value_or(token);
    if (!body.is_null()) {
      request.body = body.dump();
      request.content_type = "application/json";
    }
    request.remote_addr = "192.0.2.50";
    return api.handle(request);
  }
  json device(const std::string& id) {
    const HttpResponse response = call("GET", "/api/v1/devices");
    for (const auto& item : response.body["devices"]) {
      if (item["id"] == id) return item;
    }
    return nullptr;
  }
  void update(const std::string& task_id, const std::string& state, int revision, json failure = nullptr) {
    json task = {{"task_id", task_id}, {"state", state}, {"revision", revision}};
    if (!failure.is_null()) task["failure"] = failure;
    bridge.handle_task_update({{"task", task}});
  }

  fs::path dir;
  Registry registry;
  HardwareStore store;
  TokenStore tokens;
  PairingService pairing;
  FakePublisher publisher;
  CoreBridge bridge;
  ApprovalBook approvals;
  Api api;
  std::string token;
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
TEST(Crypto, Sha256Vectors) {
  using hm::crypto::sha256_hex;
  EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(sha256_hex(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // padding boundaries (Python hashlib)
  const std::map<int, std::string> boundaries = {
      {55, "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072"},
      {56, "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e"},
      {63, "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2"},
      {64, "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c"},
      {65, "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9"},
      {119, "000b48d4edf0fa7bee3c6236ecd2785baa5db4eeb8bb54341b029e0d9fa5fb0c"},
      {120, "13f05a0b594787f5ecd315edc96141bd3243203d1b7d4f0836f37308b276ba98"}};
  for (const auto& [length, expected] : boundaries) EXPECT_EQ(sha256_hex(std::string(length, 'x')), expected) << length;
}

TEST(Crypto, HmacRfc4231AndBase32) {
  using hm::crypto::hex;
  using hm::crypto::hmac_sha256;
  EXPECT_EQ(hex(hmac_sha256(std::string(20, '\x0b'), "Hi There")),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  EXPECT_EQ(hex(hmac_sha256("Jefe", "what do ya want for nothing?")),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  EXPECT_EQ(hex(hmac_sha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  auto b32 = [](const std::string& s) {
    return hm::crypto::base32(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
  };
  EXPECT_EQ(b32("f"), "MY");
  EXPECT_EQ(b32("foo"), "MZXW6");
  EXPECT_EQ(b32("fooba"), "MZXW6YTB");
  EXPECT_EQ(b32("foobar"), "MZXW6YTBOI");
  EXPECT_EQ(hm::crypto::unhex("00ff10"), std::string("\x00\xff\x10", 3));
  EXPECT_THROW(hm::crypto::unhex("0g"), std::invalid_argument);
  EXPECT_NE(hm::crypto::random_hex(32), hm::crypto::random_hex(32));
  EXPECT_EQ(hm::crypto::random_hex(32).size(), 64u);
  EXPECT_TRUE(hm::crypto::constant_time_equal("abc", "abc"));
  EXPECT_FALSE(hm::crypto::constant_time_equal("abc", "abd"));
  EXPECT_FALSE(hm::crypto::constant_time_equal("abc", "abcd"));
}

TEST(Pairing, CodeMatchesPythonReference) {
  std::string secret;
  for (int i = 0; i < 32; ++i) secret.push_back(static_cast<char>(i));
  // Python: base64.b32encode(hmac.new(secret, b"homemind-pairing-v1\n" + nonce, sha256).digest()[:5])
  EXPECT_EQ(pairing_code(secret, "v1.2000000000.00112233445566778899aabbccddeeff"), "NLI7ENCI");
  EXPECT_EQ(format_code("NLI7ENCI"), "NLI7-ENCI");
  EXPECT_EQ(normalize_code(" nli7-enci "), "NLI7ENCI");
  EXPECT_EQ(nonce_expiry("v1.2000000000.00112233445566778899aabbccddeeff"), 2000000000);
  EXPECT_FALSE(nonce_expiry("v2.2000000000.00112233445566778899aabbccddeeff"));
  EXPECT_FALSE(nonce_expiry("v1.2000000000.00112233445566778899AABBCCDDEEFF"));
  EXPECT_FALSE(nonce_expiry("v1..00112233445566778899aabbccddeeff"));
  EXPECT_FALSE(nonce_expiry("v1.20x0.00112233445566778899aabbccddeeff"));
  EXPECT_FALSE(nonce_expiry("v1.2000000000.0011"));
  EXPECT_TRUE(std::regex_match(make_nonce(123), std::regex("^v1\\.123\\.[0-9a-f]{32}$")));
  EXPECT_EQ(clean_device_name("  Galaxy\n\x1b[31m S24 "), "Galaxy[31m S24");
  EXPECT_EQ(clean_device_name("\t"), "unnamed");
  EXPECT_EQ(bearer_token("Bearer abc"), "abc");
  EXPECT_EQ(bearer_token("  bearer   abc  "), "abc");
  EXPECT_FALSE(bearer_token("Basic abc"));
  EXPECT_FALSE(bearer_token("Bearer"));
  EXPECT_FALSE(bearer_token(""));
}

TEST(Pairing, SecretAndFilesArePrivate) {
  const fs::path dir = temp_dir("files");
  TokenStore tokens(dir);
  PairingService pairing(auth_config(dir), tokens);
  pairing.prepare();
  EXPECT_EQ(mode_of(dir), 0700u);
  EXPECT_EQ(mode_of(dir / "pairing-secret"), 0600u);
  const std::string secret = read_file(dir / "pairing-secret");
  EXPECT_TRUE(std::regex_match(secret, std::regex("^[0-9a-f]{64}\n$")));
  pairing.prepare();  // existing secret is kept
  EXPECT_EQ(read_file(dir / "pairing-secret"), secret);
  std::string token;
  const TokenRecord record = tokens.issue("phone", token);
  EXPECT_TRUE(std::regex_match(token, std::regex("^hm1_[0-9a-f]{64}$")));
  EXPECT_EQ(mode_of(dir / "tokens.json"), 0600u);
  const std::string stored = read_file(dir / "tokens.json");
  EXPECT_EQ(stored.find(token), std::string::npos) << "plaintext token on disk";
  EXPECT_EQ(stored.find(token.substr(4)), std::string::npos);
  EXPECT_NE(stored.find(hm::crypto::sha256_hex(token)), std::string::npos);
  EXPECT_EQ(record.sha256, hm::crypto::sha256_hex(token));
  fs::remove_all(dir);
}

TEST(Pairing, ClaimOnceThenUsedExpiredWrongAndRateLimited) {
  const fs::path dir = temp_dir("claim");
  TokenStore tokens(dir);
  PairingService pairing(auth_config(dir), tokens);
  pairing.prepare();
  const std::int64_t now_ms = hm::now_ms();
  const PairingOffer offer = pairing.make_offer(now_ms / 1000, "http://192.0.2.10:8000/", 600);
  EXPECT_EQ(offer.qr["version"], 1);
  EXPECT_EQ(offer.qr["base_url"], "http://192.0.2.10:8000");
  EXPECT_EQ(offer.qr["device_id"], "NODE-RPI4-01");
  EXPECT_EQ(offer.qr["pairing_code"], offer.code);
  EXPECT_EQ(offer.qr["nonce"], offer.nonce);
  EXPECT_TRUE(std::regex_match(offer.qr["expires_at"].get<std::string>(),
                               std::regex("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z$")));
  EXPECT_THROW(pairing.make_offer(0, "ftp://x", 600), std::invalid_argument);
  EXPECT_THROW(pairing.make_offer(0, "http://x", 5), std::invalid_argument);

  auto ok = pairing.claim(offer.code, offer.nonce, "Pixel", "198.51.100.1", now_ms);
  ASSERT_EQ(ok.status, PairingService::Status::ok);
  EXPECT_EQ(tokens.verify(ok.token), ok.record.id);
  EXPECT_EQ(ok.record.device_name, "Pixel");
  // one-time: the same QR again fails, also after a restart (used nonces persist)
  EXPECT_EQ(pairing.claim(offer.code, offer.nonce, "Pixel", "198.51.100.1", now_ms).status,
            PairingService::Status::invalid);
  {
    TokenStore tokens2(dir);
    PairingService restarted(auth_config(dir), tokens2);
    EXPECT_EQ(restarted.claim(offer.code, offer.nonce, "x", "198.51.100.9", now_ms).status,
              PairingService::Status::invalid);
  }
  // lowercase / no dash is accepted for a fresh code
  const PairingOffer second = pairing.make_offer(now_ms / 1000, "http://192.0.2.10:8000", 60);
  std::string sloppy = normalize_code(second.code);
  std::transform(sloppy.begin(), sloppy.end(), sloppy.begin(), [](unsigned char c) { return std::tolower(c); });
  EXPECT_EQ(pairing.claim(sloppy, second.nonce, "", "198.51.100.2", now_ms).status, PairingService::Status::ok);
  // expired (own instance: its own failure counter)
  const PairingOffer third = pairing.make_offer(now_ms / 1000, "http://192.0.2.10:8000", 30);
  {
    PairingService other(auth_config(dir), tokens);
    EXPECT_EQ(other.claim(third.code, third.nonce, "x", "198.51.100.3", now_ms + 31000).status,
              PairingService::Status::invalid);
  }
  // failures so far: 1 (198.51.100.1 reused its QR). Per-client limit 3 in 60 s: the 4th attempt of
  // 198.51.100.3 is refused before the code is even checked (the right code does not help).
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(pairing.claim("AAAA-AAAA", third.nonce, "x", "198.51.100.3", now_ms).status,
              PairingService::Status::invalid);
  }
  auto limited = pairing.claim(third.code, third.nonce, "x", "198.51.100.3", now_ms);
  EXPECT_EQ(limited.status, PairingService::Status::rate_limited);
  EXPECT_EQ(limited.retry_after_s, 60);
  EXPECT_EQ(pairing.claim(third.code, third.nonce, "x", "198.51.100.3", now_ms + 30000).retry_after_s, 30);
  // other clients still work until the global limit (5 failures) is reached
  EXPECT_EQ(pairing.claim("AAAA-AAAA", third.nonce, "x", "198.51.100.4", now_ms).status, PairingService::Status::invalid);
  EXPECT_EQ(pairing.claim(third.code, third.nonce, "x", "198.51.100.5", now_ms).status,
            PairingService::Status::rate_limited);
  // after the window the right code works again
  EXPECT_EQ(pairing.claim(third.code, third.nonce, "x", "198.51.100.5", now_ms + 61000).status,
            PairingService::Status::invalid);  // expired by then (30 s code), but no longer limited
  const PairingOffer fourth = pairing.make_offer(now_ms / 1000 + 61, "http://192.0.2.10:8000", 600);
  EXPECT_EQ(pairing.claim(fourth.code, fourth.nonce, "x", "198.51.100.5", now_ms + 61000).status,
            PairingService::Status::ok);
  fs::remove_all(dir);
}

TEST(Pairing, ForgedNonceFarInTheFutureIsRefused) {
  const fs::path dir = temp_dir("forged");
  TokenStore tokens(dir);
  PairingService pairing(auth_config(dir), tokens);
  pairing.prepare();
  const std::int64_t now_s = hm::now_ms() / 1000;
  const std::string nonce = make_nonce(now_s + kMaxCodeTtlS + 3600);
  const std::string code = pairing_code(load_pairing_secret(dir / "pairing-secret"), nonce);  // even the right MAC
  EXPECT_EQ(pairing.claim(code, nonce, "x", "c", now_s * 1000).status, PairingService::Status::invalid);
  fs::remove_all(dir);
}

TEST(Tokens, RevocationFromAnotherProcessIsSeen) {
  const fs::path dir = temp_dir("revoke");
  TokenStore service(dir);
  std::string a, b;
  const auto first = service.issue("phone A", a);
  const auto second = service.issue("phone B", b);
  EXPECT_EQ(service.verify(a), first.id);
  EXPECT_EQ(service.verify(b), second.id);
  EXPECT_FALSE(service.verify("hm1_" + std::string(64, '0')));
  TokenStore cli(dir);  // what `--revoke-token` does
  ASSERT_EQ(cli.list().size(), 2u);
  EXPECT_TRUE(cli.revoke(first.id));
  EXPECT_FALSE(cli.revoke(first.id));
  EXPECT_FALSE(service.verify(a));
  EXPECT_EQ(service.verify(b), second.id);
  EXPECT_EQ(cli.revoke_all(), 1u);
  EXPECT_FALSE(service.verify(b));
  EXPECT_EQ(mode_of(dir / "tokens.json"), 0600u);
  // an unreadable file refuses every token (fail closed) and blocks new tokens until reset
  std::string c;
  service.issue("phone C", c);
  std::ofstream(dir / "tokens.json") << "{not json";
  EXPECT_FALSE(service.verify(c));
  std::string d;
  EXPECT_THROW(service.issue("phone D", d), std::runtime_error);
  EXPECT_EQ(cli.revoke_all(), 0u);
  EXPECT_NO_THROW(service.issue("phone D", d));
  EXPECT_TRUE(service.verify(d));
  fs::remove_all(dir);
}

// ---------------------------------------------------------------------------------------------------------------
TEST(Api, HealthIsOpenEverythingElseNeedsAToken) {
  Fixture f;
  HttpRequest bare{"GET", "/api/v1/health", "", "", "", "192.0.2.50"};
  const auto health = f.api.handle(bare);
  EXPECT_EQ(health.status, 200);
  EXPECT_EQ(health.body["status"], "ok");
  EXPECT_EQ(health.body["version"], "0.5.0-beta");
  EXPECT_EQ(health.body["mqtt_enabled"], true);
  for (const char* path : {"/api/v1/dashboard", "/api/v1/hardware-map", "/api/v1/devices", "/api/v1/tasks/x",
                           "/api/v1/unknown"}) {
    bare.path = path;
    const auto refused = f.api.handle(bare);
    EXPECT_EQ(refused.status, 401) << path;
    EXPECT_EQ(refused.body["detail"], "missing bearer token");
    ASSERT_FALSE(refused.headers.empty());
    EXPECT_EQ(refused.headers[0].first, "WWW-Authenticate");
  }
  bare.method = "POST";
  bare.path = "/api/v1/devices/FAN-01/commands";
  bare.body = R"({"command":"ON"})";
  bare.content_type = "application/json";
  EXPECT_EQ(f.api.handle(bare).status, 401);
  EXPECT_TRUE(f.publisher.published.empty());
  const auto wrong = f.call("GET", "/api/v1/dashboard", nullptr, "hm1_" + std::string(64, 'f'));
  EXPECT_EQ(wrong.status, 401);
  EXPECT_EQ(wrong.body["detail"], "invalid or revoked token");
  EXPECT_EQ(f.call("GET", "/api/v1/dashboard", nullptr, "").status, 401);
  EXPECT_EQ(f.call("GET", "/api/v1/dashboard").status, 200);
  // revoked token
  for (const auto& record : f.tokens.list()) f.tokens.revoke(record.id);
  EXPECT_EQ(f.call("GET", "/api/v1/dashboard").status, 401);
}

TEST(Api, MapUsesRegistryV4) {
  Fixture f;
  const auto map = f.call("GET", "/api/v1/hardware-map").body;
  std::set<std::string> nodes;
  for (const auto& node : map["nodes"]) nodes.insert(node["id"].get<std::string>());
  // The doll axis (Octopus host and controller) plays the person: kept in the registry, not shown.
  EXPECT_EQ(nodes, (std::set<std::string>{"NODE-RPI4-01", "NODE-RPI5-01", "NODE-RPI5-02"}));
  EXPECT_EQ(map["controllers"].size(), 2u);
  EXPECT_EQ(map["nodes"][0]["hardware"], "Raspberry Pi 4");
  EXPECT_TRUE(map["nodes"][0].contains("symbol"));
  EXPECT_TRUE(map["controllers"][0].contains("name"));
  const json door = f.device("SERVO-DOOR-01");
  EXPECT_EQ(door["hardware"], "열기/닫기 모터");
  EXPECT_EQ(door["name"], "현관문");
  EXPECT_EQ(door["category"], "actuator");
  EXPECT_EQ(door["alias"], "door1");
  EXPECT_EQ(door["controller_name"], "ESP32");
  EXPECT_EQ(door["supported_commands"], json::array({"OPEN", "CLOSE"}));
  EXPECT_EQ(door["state"], "UNKNOWN");
  EXPECT_EQ(f.device("LED-LIVING-01")["supported_commands"], json::array({"ON", "OFF"}));
  EXPECT_EQ(f.device("FAN-01")["controller_id"], "CTRL-ESP8266-LED-01");
  EXPECT_EQ(f.device("FAN-01")["supported_commands"], json::array({"ON", "OFF"}));
  EXPECT_EQ(f.device("CAM-INDOOR-01")["supported_commands"], json::array());
  EXPECT_EQ(f.device("CAM-INDOOR-01")["category"], "input");
  std::set<std::string> listed;
  for (const auto& device : map["devices"]) listed.insert(device["id"].get<std::string>());
  for (const char* hidden : {"SERVO-DOOR-02", "LED-TV-01", "LED-WASHER-01", "LED-LAPTOP-01", "LED-GUIDE-01",
                             "SERVO-BED-CURTAIN-01", "FND-TV-01", "MOTOR-XY-X-01", "MOTOR-XY-Y-01",
                             "SENSOR-HOME-X-01", "SENSOR-HOME-Y-01"}) {
    EXPECT_EQ(listed.count(hidden), 0u) << hidden;
  }
  EXPECT_EQ(listed.size(), 8u);  // 21 registry IDs - 9 not in the model - 4 doll axis IDs
  EXPECT_EQ(f.device("FAN-01")["category"], "actuator");
  const auto dashboard = f.call("GET", "/api/v1/dashboard").body["snapshot"];
  EXPECT_EQ(dashboard["connected"], true);
  EXPECT_TRUE(std::regex_match(dashboard["updated_at"].get<std::string>(),
                               std::regex("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(\\.\\d{6})?\\+00:00$")));
  EXPECT_EQ(dashboard["devices"].size(), f.call("GET", "/api/v1/devices").body["devices"].size());
}

TEST(Api, UnknownOrReadOnlyDeviceCannotBeControlled) {
  Fixture f;
  auto status = [&](const char* id, const char* command) {
    return f.call("POST", std::string("/api/v1/devices/") + id + "/commands", {{"command", command}}).status;
  };
  EXPECT_EQ(status("HEATING-01", "ON"), 404);
  EXPECT_EQ(status("CAM-INDOOR-01", "ON"), 400);
  EXPECT_EQ(status("LED-GUIDE-01", "ON"), 404);  // not in the model (registry v4): not listed, so unknown here
  EXPECT_EQ(status("FAN-01", "OPEN"), 400);
  EXPECT_EQ(f.call("POST", "/api/v1/devices/FAN-01/commands", {{"command", "OPEN"}}).body["detail"],
            "OPEN is not supported by FAN-01");
  EXPECT_TRUE(f.publisher.published.empty());
}

TEST(Api, CommandWithoutMqttIsRefused) {
  Fixture disabled(false);
  EXPECT_EQ(disabled.call("POST", "/api/v1/devices/FAN-01/commands", {{"command", "ON"}}).status, 503);
  EXPECT_EQ(disabled.call("GET", "/api/v1/health").body["mqtt_enabled"], false);
  Fixture down;
  down.publisher.up = false;
  const auto refused = down.call("POST", "/api/v1/devices/FAN-01/commands", {{"command", "ON"}});
  EXPECT_EQ(refused.status, 503);
  EXPECT_EQ(refused.body["detail"], "core MQTT connection is not available");
  EXPECT_EQ(down.call("GET", "/api/v1/health").body["mqtt_connected"], false);
}

TEST(Api, CommandGoesToCoreAndStateWaitsForCompletion) {
  Fixture f;
  const auto response = f.call("POST", "/api/v1/devices/LED-LIVING-01/commands", {{"command", " on "}});
  ASSERT_EQ(response.status, 202);
  EXPECT_EQ(response.body["device"]["state"], "UNKNOWN");  // no optimistic change
  EXPECT_EQ(response.body["device"]["pending_command"], "ON");
  EXPECT_EQ(response.body["accepted"], true);
  EXPECT_EQ(response.body["task_state"], "requested");
  EXPECT_EQ(response.body["message"], "거실 조명 ON 요청을 core에 보냈습니다. 장치 응답(ACK) 후 상태가 바뀝니다.");
  const auto requests = f.publisher.on(kTaskRequest);
  ASSERT_EQ(requests.size(), 1u);
  const json& request = requests[0];
  EXPECT_EQ(request["intent"], "device_control");
  EXPECT_EQ(request["source_node_id"], "NODE-RPI4-01");
  EXPECT_EQ(request["safety_level"], "normal");
  EXPECT_EQ(request["confidence"], 1.0);
  EXPECT_EQ(request["parameters"], json({{"target_device_id", "LED-LIVING-01"},
                                         {"action", "set_switch"},
                                         {"parameters", {{"on", true}}},
                                         {"origin", "ui"},
                                         {"ai_device", nullptr},
                                         {"ai_action", nullptr}}));
  EXPECT_EQ(request["task_id"], response.body["task_id"]);
  for (const auto& [topic, document] : f.publisher.published) {
    EXPECT_TRUE(topic.rfind("pfn/v1/core/", 0) == 0) << topic;  // never a device topic
  }
  const std::string task_id = request["task_id"];
  f.update(task_id, "running", 2);
  EXPECT_EQ(f.device("LED-LIVING-01")["state"], "UNKNOWN");
  EXPECT_EQ(f.device("LED-LIVING-01")["last_task_state"], "running");
  f.update(task_id, "completed", 3);
  const json living = f.device("LED-LIVING-01");
  EXPECT_EQ(living["state"], "ON");
  EXPECT_EQ(living["online"], true);
  EXPECT_EQ(living["pending_command"], nullptr);
  EXPECT_EQ(living["pending_task_id"], nullptr);
  const auto status = f.call("GET", "/api/v1/tasks/" + task_id);
  EXPECT_EQ(status.status, 200);
  EXPECT_EQ(status.body, json({{"task_id", task_id},
                               {"device_id", "LED-LIVING-01"},
                               {"command", "ON"},
                               {"state", "completed"},
                               {"failure_code", nullptr},
                               {"failure_message", nullptr}}));
  EXPECT_EQ(f.call("GET", "/api/v1/tasks/nope").status, 404);
  EXPECT_EQ(f.call("GET", "/api/v1/tasks/nope").body["detail"], "unknown or expired task");
}

TEST(Api, ServoUsesRegistryPositionsAndDoorOpenNeedsApproval) {
  Fixture f;
  const auto response = f.call("POST", "/api/v1/devices/SERVO-DOOR-01/commands", {{"command", "OPEN"}});
  ASSERT_EQ(response.status, 202);
  EXPECT_EQ(response.body["message"], "현관문 열기는 7인치 화면에서 승인해야 실행됩니다.");
  const json door = f.publisher.published.back().second;
  EXPECT_EQ(door["parameters"]["action"], "set_position");
  EXPECT_EQ(door["parameters"]["parameters"], json({{"position_degrees", 90}}));
  EXPECT_EQ(door["safety_level"], "caution");
  f.call("POST", "/api/v1/devices/SERVO-WINDOW-01/commands", {{"command", "CLOSE"}});
  const json window = f.publisher.published.back().second;
  EXPECT_EQ(window["parameters"]["parameters"], json({{"position_degrees", 0}}));
  EXPECT_EQ(window["safety_level"], "normal");
  f.call("POST", "/api/v1/devices/SERVO-DOOR-01/commands", {{"command", "CLOSE"}});
  EXPECT_EQ(f.publisher.published.back().second["safety_level"], "normal");
}

TEST(Api, RejectedTaskKeepsStateAndReportsFailure) {
  Fixture f;
  f.call("POST", "/api/v1/devices/FAN-01/commands", {{"command", "ON"}});
  const std::string task_id = f.publisher.published.back().second["task_id"];
  f.update(task_id, "rejected", 2,
           {{"code", "DEVICE_NOT_WIRED"}, {"message", "x"}, {"occurred_at_ms", 1}, {"retry_allowed", false}});
  const json fan = f.device("FAN-01");
  EXPECT_EQ(fan["state"], "UNKNOWN");
  EXPECT_EQ(fan["last_failure_code"], "DEVICE_NOT_WIRED");
  EXPECT_EQ(fan["pending_command"], nullptr);
  const auto status = f.call("GET", "/api/v1/tasks/" + task_id).body;
  EXPECT_EQ(status["failure_code"], "DEVICE_NOT_WIRED");
  EXPECT_EQ(status["failure_message"], "x");
  f.update(task_id, "completed", 1);  // stale revision is ignored
  EXPECT_EQ(f.call("GET", "/api/v1/tasks/" + task_id).body["state"], "rejected");
  f.bridge.handle_task_update({{"task", {{"task_id", "someone-else"}, {"state", "completed"}, {"revision", 9}}}});
  f.bridge.handle_task_update(json::array());
  EXPECT_EQ(f.device("FAN-01")["state"], "UNKNOWN");
}

TEST(Api, StateResponseSetsOnlineFlags) {
  Fixture f;
  f.bridge.on_message("pfn/v1/core/state/response/homemind-mobile-api",
                      {{"devices", {{{"node_id", "CTRL-ESP32-ACT-01"}, {"status", "online"}},
                                    {{"node_id", "NODE-RPI5-01"}, {"status", "offline"}}}}});
  EXPECT_EQ(f.device("SERVO-WINDOW-01")["online"], true);  // registry v4: FAN-01 is on the ESP8266
  EXPECT_EQ(f.device("FAN-01")["online"], false);
  EXPECT_EQ(f.device("LED-LIVING-01")["online"], false);
  const auto map = f.call("GET", "/api/v1/hardware-map").body;
  for (const auto& controller : map["controllers"]) {
    EXPECT_EQ(controller["online"], controller["id"] == "CTRL-ESP32-ACT-01") << controller;
  }
  f.bridge.request_snapshot();
  const auto requests = f.publisher.on(kStateRequest);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0]["client_id"], "homemind-mobile-api");
  EXPECT_EQ(requests[0]["sections"], json::array({"devices", "tasks"}));
}

TEST(Api, ValidationRoutingAndMethodErrorsMatchFastApi) {
  Fixture f;
  auto post_raw = [&](const std::string& path, const std::string& body, const std::string& type) {
    HttpRequest request{"POST", path, "Bearer " + f.token, type, body, "192.0.2.50"};
    return f.api.handle(request);
  };
  const std::string commands = "/api/v1/devices/FAN-01/commands";
  auto missing = post_raw(commands, "{}", "application/json");
  EXPECT_EQ(missing.status, 422);
  EXPECT_EQ(missing.body, json::parse(R"({"detail":[{"type":"missing","loc":["body","command"],"msg":"Field required","input":{}}]})"));
  auto wrong_type = post_raw(commands, R"({"command":1})", "application/json; charset=utf-8");
  EXPECT_EQ(wrong_type.body, json::parse(R"({"detail":[{"type":"string_type","loc":["body","command"],"msg":"Input should be a valid string","input":1}]})"));
  EXPECT_EQ(post_raw(commands, "", "application/json").body,
            json::parse(R"({"detail":[{"type":"missing","loc":["body"],"msg":"Field required","input":null}]})"));
  EXPECT_EQ(post_raw(commands, R"({"command":"ON"})", "text/plain").body["detail"][0]["type"], "model_attributes_type");
  EXPECT_EQ(post_raw(commands, "[1]", "application/json").body["detail"][0]["input"], json::array({1}));
  auto bad_json = post_raw(commands, "{bad", "application/json");
  EXPECT_EQ(bad_json.status, 422);
  EXPECT_EQ(bad_json.body["detail"][0]["type"], "json_invalid");
  // body is validated before the device lookup (FastAPI order)
  EXPECT_EQ(post_raw("/api/v1/devices/HEATING-01/commands", "{}", "application/json").status, 422);
  const auto long_name = post_raw("/api/v1/pairing/claim",
                                  json({{"code", std::string(33, 'A')}, {"nonce", "x"}, {"device_name", [] {
                                    std::string name;
                                    for (int i = 0; i < 65; ++i) name += "가";
                                    return name;
                                  }()}}).dump(),
                                  "application/json");
  EXPECT_EQ(long_name.status, 422);
  EXPECT_EQ(long_name.body["detail"][0]["type"], "string_too_long");
  EXPECT_EQ(long_name.body["detail"][0]["ctx"]["max_length"], 32);
  EXPECT_EQ(utf8_length("가나다"), 3u);
  const auto not_found = f.call("GET", "/api/v1/nothing");
  EXPECT_EQ(not_found.status, 404);
  EXPECT_EQ(not_found.body["detail"], "Not Found");
  const auto wrong_method = f.call("GET", commands);
  EXPECT_EQ(wrong_method.status, 405);
  EXPECT_EQ(wrong_method.body["detail"], "Method Not Allowed");
  EXPECT_EQ(wrong_method.headers.at(0), (std::pair<std::string, std::string>{"Allow", "POST"}));
  EXPECT_EQ(f.call("POST", "/api/v1/devices").status, 405);
  EXPECT_EQ(f.call("GET", "/api/v1/devices//commands").status, 404);
  HttpRequest options{"OPTIONS", "/api/v1/devices", "", "", "", "192.0.2.50"};
  EXPECT_EQ(f.api.handle(options).status, 405);  // no CORS: preflight is not answered, but not 401 either
  HttpRequest root{"GET", "/", "", "", "", "192.0.2.50"};
  EXPECT_EQ(f.api.handle(root).status, 200);
}

TEST(Api, PairingClaimEndpoint) {
  Fixture f;
  const PairingOffer offer = f.pairing.make_offer(hm::now_ms() / 1000, "http://192.0.2.10:8000", 600);
  HttpRequest claim{"POST", "/api/v1/pairing/claim", "", "application/json",
                    json({{"code", offer.code}, {"nonce", offer.nonce}, {"device_name", "Galaxy"}}).dump(),
                    "198.51.100.7"};
  const auto issued = f.api.handle(claim);
  ASSERT_EQ(issued.status, 201);
  EXPECT_EQ(issued.body["token_type"], "Bearer");
  EXPECT_EQ(issued.body["device_id"], "NODE-RPI4-01");
  const std::string token = issued.body["token"];
  EXPECT_EQ(f.call("GET", "/api/v1/devices", nullptr, token).status, 200);
  const auto again = f.api.handle(claim);
  EXPECT_EQ(again.status, 401);
  EXPECT_EQ(again.body["detail"], "invalid, expired or already used pairing code");
  f.api.handle(claim);
  f.api.handle(claim);  // 3 failures from this address
  const auto limited = f.api.handle(claim);
  EXPECT_EQ(limited.status, 429);
  EXPECT_EQ(limited.headers.at(0).first, "Retry-After");
  EXPECT_GE(std::stoi(limited.headers.at(0).second), 1);
  bool listed = false;
  for (const auto& record : f.tokens.list()) {
    listed |= record.device_name == "Galaxy" && record.id == issued.body["token_id"].get<std::string>();
  }
  EXPECT_TRUE(listed);
}

TEST(Config, LoadsTemplateAndRefusesSecretsOrBadValues) {
  const fs::path dir = temp_dir("config");
  fs::create_directories(dir);
  auto write = [&](const json& document) {
    std::ofstream(dir / "mobile-api.json") << document.dump();
    return dir / "mobile-api.json";
  };
  const json base = {{"base_dir", dir.string()},
                     {"broker", {{"host", "127.0.0.1"}, {"port", 1884}, {"username", "homemind-mobile-api"},
                                 {"password_file", "secrets/mqtt"}}},
                     {"device_registry", kRegistry.string()},
                     {"listen_host", "0.0.0.0"},
                     {"listen_port", 8000},
                     {"data_dir", "mobile-api"},
                     {"public_base_url", "http://192.0.2.10:8000"}};
  const ServiceConfig config = ServiceConfig::load(write(base));
  EXPECT_EQ(config.registry_path, kRegistry);
  EXPECT_EQ(config.auth.data_dir, dir / "mobile-api");
  EXPECT_EQ(config.auth.secret_file, dir / "mobile-api" / "pairing-secret");
  EXPECT_EQ(config.runtime.broker.password_file, (dir / "secrets/mqtt").string());
  EXPECT_EQ(config.bridge.client_id, "homemind-mobile-api");
  EXPECT_TRUE(config.bridge.enabled);
  EXPECT_EQ(config.auth.device_id, "NODE-RPI4-01");
  EXPECT_EQ(config.listen_host, "0.0.0.0");
  json bad = base;
  bad.erase("data_dir");
  EXPECT_THROW(ServiceConfig::load(write(bad)), std::runtime_error);
  bad = base;
  bad["source_node_id"] = "NODE-RPI5-02";
  EXPECT_THROW(ServiceConfig::load(write(bad)), std::runtime_error);
  bad = base;
  bad["broker"]["password"] = "x";
  EXPECT_THROW(ServiceConfig::load(write(bad)), std::runtime_error);
  bad = base;
  bad["pairing_secret"] = "x";
  EXPECT_THROW(ServiceConfig::load(write(bad)), std::runtime_error);
  bad = base;
  bad["pairing_code_ttl_s"] = 99999;
  EXPECT_THROW(ServiceConfig::load(write(bad)), std::runtime_error);
  // the packaged template parses (placeholders only)
  const fs::path template_path = fs::path(HM_TEST_SERVICE_DIR) / "packaging" / "mobile-api.json";
  const ServiceConfig packaged = ServiceConfig::load(template_path);
  EXPECT_EQ(packaged.registry_path, "/data/share/homemind/config/device-registry.json");
  EXPECT_EQ(packaged.auth.data_dir, "/data/share/homemind/mobile-api");
  EXPECT_EQ(packaged.runtime.broker.username, "homemind-mobile-api");
  EXPECT_TRUE(packaged.tls.enabled);
  EXPECT_EQ(packaged.tls.key_file, "/data/share/homemind/mobile-api/tls-key.pem");
  EXPECT_EQ(packaged.auth.public_base_url.rfind("https://", 0), 0u);
  // the copy the NODE-RPI4-01 package installs (native/packaging/rpi4-mobile-api) says the same
  const ServiceConfig device = ServiceConfig::load(fs::path(HM_TEST_SERVICE_DIR) / ".." / ".." / "packaging" /
                                                   "rpi4-mobile-api" / "config" / "mobile-api.json");
  EXPECT_TRUE(device.tls.enabled);
  EXPECT_EQ(device.tls.cert_file, packaged.tls.cert_file);
  EXPECT_EQ(device.auth.public_base_url, packaged.auth.public_base_url);
  EXPECT_FALSE(device.runtime.simulated);
  // HTTPS settings: default files under data_dir, https:// base URL required, no key material in the config
  EXPECT_FALSE(config.tls.enabled);
  EXPECT_EQ(config.tls.cert_file, dir / "mobile-api" / "tls-cert.pem");
  bad = base;
  bad["tls_enabled"] = true;
  EXPECT_THROW(ServiceConfig::load(write(bad)), std::runtime_error);  // http:// base URL
  bad["public_base_url"] = "https://192.0.2.10:8000";
  EXPECT_TRUE(ServiceConfig::load(write(bad)).tls.enabled);
  bad["tls_key"] = "-----BEGIN PRIVATE KEY-----";
  EXPECT_THROW(ServiceConfig::load(write(bad)), std::runtime_error);
  fs::remove_all(dir);
}

TEST(Tls, UrlHost) {
  EXPECT_EQ(url_host("https://192.168.0.14:8000"), "192.168.0.14");
  EXPECT_EQ(url_host("https://homemind.local/api"), "homemind.local");
  EXPECT_EQ(url_host("http://[fd00::1]:8000/"), "fd00::1");
  EXPECT_EQ(url_host("https://user@10.0.0.2"), "10.0.0.2");
  EXPECT_EQ(url_host(""), "");
  EXPECT_EQ(url_host("192.168.0.14"), "");
}

TEST(Tls, CreatesOnceThenReloadsTheSameCertificate) {
  if (!tls_supported()) GTEST_SKIP() << "built without OpenSSL";
  const fs::path dir = temp_dir("tls");
  const TlsIdentity first = ensure_tls_identity(dir / "cert.pem", dir / "key.pem", "NODE-RPI4-01", "192.0.2.10");
  EXPECT_TRUE(first.created);
  EXPECT_TRUE(std::regex_match(first.sha256, std::regex("^[0-9a-f]{64}$")));
  EXPECT_EQ(mode_of(dir / "key.pem"), 0600u);
  EXPECT_EQ(read_file(dir / "cert.pem").rfind("-----BEGIN CERTIFICATE-----", 0), 0u);
  const TlsIdentity again = ensure_tls_identity(dir / "cert.pem", dir / "key.pem", "NODE-RPI4-01", "192.0.2.99");
  EXPECT_FALSE(again.created);
  EXPECT_EQ(again.sha256, first.sha256) << "a restart keeps the pinned certificate";

  const TlsIdentity other = ensure_tls_identity(dir / "o-cert.pem", dir / "o-key.pem", "x", "homemind.local");
  EXPECT_NE(other.sha256, first.sha256);
  fs::copy_file(dir / "o-key.pem", dir / "key.pem", fs::copy_options::overwrite_existing);
  EXPECT_THROW(ensure_tls_identity(dir / "cert.pem", dir / "key.pem", "NODE-RPI4-01", ""), std::runtime_error);
  fs::remove_all(dir);
}

TEST(Tls, PairingQrPinsTheCertificateOnlyForHttps) {
  const fs::path dir = temp_dir("tls-qr");
  TokenStore tokens(dir);
  AuthConfig auth = auth_config(dir);
  auth.tls_sha256 = std::string(64, 'a');
  PairingService pairing(auth, tokens);
  pairing.prepare();
  const PairingOffer https = pairing.make_offer(2000000000, "https://192.0.2.10:8000", 600);
  EXPECT_EQ(https.qr["tls_sha256"], std::string(64, 'a'));
  const PairingOffer http = pairing.make_offer(2000000000, "http://192.0.2.10:8000", 600);
  EXPECT_FALSE(http.qr.contains("tls_sha256"));
  fs::remove_all(dir);
}

TEST(TerminalQr, DrawsHalfBlockRowsWithQuietZone) {
  const std::string payload =
      R"({"version":1,"device_id":"NODE-RPI4-01","device_name":"HomeMind","base_url":"http://192.168.0.10:8000",)"
      R"("pairing_code":"ABCD-EFGH","nonce":"v1.1792560600.0123456789abcdef0123456789abcdef","expires_at":"x"})";
  const int size = hm::mobile::terminal_qr_size(payload);
  EXPECT_GE(size, 21);
  EXPECT_EQ((size - 21) % 4, 0);  // QR side = 17 + 4 * version
  const std::string drawing = hm::mobile::render_terminal_qr(payload);
  int lines = 0;
  for (char c : drawing) lines += c == '\n';
  EXPECT_EQ(lines, (size + 8 + 1) / 2);
  EXPECT_NE(drawing.find("\x1b[0m\n"), std::string::npos);
}


// ---- 7-inch UI pairing QR (pfn/v1/mobile/pairing/request -> offer) ------------------------------------------------
namespace {

json pairing_request(const std::string& request_id, std::int64_t now_ms) {
  return {{"schema_version", "pfn.v1"}, {"event_id", hm::new_id()}, {"request_id", request_id},
          {"source_node_id", "NODE-RPI5-01"}, {"target_node_id", "NODE-RPI4-01"}, {"client_id", "homemind-ui-7inch"},
          {"sent_at_ms", now_ms}, {"expires_at_ms", now_ms + 5000}};
}

json phones_request(const std::string& action, std::int64_t now_ms, const std::string& phone_id = "") {
  json request = {{"schema_version", "pfn.v1"}, {"event_id", hm::new_id()}, {"request_id", hm::new_id()},
                  {"source_node_id", "NODE-RPI5-01"}, {"target_node_id", "NODE-RPI4-01"},
                  {"client_id", "homemind-ui-7inch"}, {"action", action}, {"sent_at_ms", now_ms},
                  {"expires_at_ms", now_ms + 5000}};
  if (!phone_id.empty()) request["phone_id"] = phone_id;
  return request;
}

}  // namespace

TEST(PairingRelay, OfferIsARealOneTimeCodeForTheUi) {
  const fs::path dir = temp_dir("relay");
  TokenStore tokens(dir);
  const AuthConfig auth = auth_config(dir);
  PairingService pairing(auth, tokens);
  pairing.prepare();
  std::vector<std::pair<std::string, json>> sent;
  PairingRelay relay(pairing, auth, "NODE-RPI4-01", false, [&sent](const std::string& topic, const json& document) {
    sent.emplace_back(topic, document);
    return true;
  });
  const std::int64_t now = hm::now_ms();
  const std::string id = hm::new_id();
  const auto offer = relay.on_request(pairing_request(id, now), now);
  ASSERT_TRUE(offer);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first, "pfn/v1/mobile/pairing/offer");
  EXPECT_EQ((*offer)["status"], "offered");
  EXPECT_EQ((*offer)["request_id"], id);
  EXPECT_EQ((*offer)["source_node_id"], "NODE-RPI4-01");
  EXPECT_EQ((*offer)["target_node_id"], "NODE-RPI5-01");
  EXPECT_FALSE(offer->contains("simulated"));
  EXPECT_TRUE(std::regex_match((*offer)["pairing_code"].get<std::string>(), std::regex("^[0-9A-Z]{4}-[0-9A-Z]{4}$")));
  const json qr = json::parse((*offer)["qr_payload"].get<std::string>());
  EXPECT_EQ(qr["pairing_code"], (*offer)["pairing_code"]);
  EXPECT_EQ(qr["base_url"], "http://192.0.2.10:8000");
  EXPECT_EQ((*offer)["expires_at_ms"].get<std::int64_t>() / 1000 - now / 1000, auth.code_ttl_s);
  // the QR on the screen pairs a phone exactly like the terminal one
  const auto claimed = pairing.claim(qr["pairing_code"], qr["nonce"], "Pixel", "198.51.100.9", now);
  EXPECT_EQ(claimed.status, PairingService::Status::ok);
  fs::remove_all(dir);
}

TEST(PairingRelay, IgnoresBadStaleAndReplayedRequestsAndLimitsTheRate) {
  const fs::path dir = temp_dir("relay-limits");
  TokenStore tokens(dir);
  AuthConfig auth = auth_config(dir);
  PairingService pairing(auth, tokens);
  pairing.prepare();
  int published = 0;
  PairingRelay relay(pairing, auth, "NODE-RPI4-01", true, [&published](const std::string&, const json&) {
    ++published;
    return true;
  });
  const std::int64_t now = hm::now_ms();
  const std::string id = hm::new_id();
  ASSERT_TRUE(relay.on_request(pairing_request(id, now), now));
  EXPECT_FALSE(relay.on_request(pairing_request(id, now), now + 10)) << "replay";
  json expired = pairing_request(hm::new_id(), now - 10000);
  EXPECT_EQ(relay.check(expired, now), "expired");
  json other = pairing_request(hm::new_id(), now);
  other["target_node_id"] = "NODE-RPI5-02";
  EXPECT_EQ(relay.check(other, now), "target_node_id");
  json forged = pairing_request(hm::new_id(), now);
  forged["source_node_id"] = "NODE-RPI5-02";
  EXPECT_EQ(relay.check(forged, now), "source_node_id");
  json extra = pairing_request(hm::new_id(), now);
  extra["pairing_code"] = "AAAA-AAAA";
  EXPECT_EQ(relay.check(extra, now), "unknown field pairing_code");
  json forever = pairing_request(hm::new_id(), now);
  forever["expires_at_ms"] = now + 3600000;
  EXPECT_EQ(relay.check(forever, now), "lifetime");
  EXPECT_EQ(published, 1);

  const auto busy = relay.on_request(pairing_request(hm::new_id(), now + 500), now + 500);
  ASSERT_TRUE(busy);
  EXPECT_EQ((*busy)["status"], "unavailable");
  EXPECT_FALSE(busy->contains("pairing_code"));
  EXPECT_EQ((*busy)["simulated"], true);
  const auto later = relay.on_request(pairing_request(hm::new_id(), now + 2500), now + 2500);
  ASSERT_TRUE(later);
  EXPECT_EQ((*later)["status"], "offered");
  fs::remove_all(dir);
}

TEST(PairingRelay, NoPublicAddressMeansNoCode) {
  const fs::path dir = temp_dir("relay-no-url");
  TokenStore tokens(dir);
  AuthConfig auth = auth_config(dir);
  auth.public_base_url.clear();
  PairingService pairing(auth, tokens);
  pairing.prepare();
  PairingRelay relay(pairing, auth, "NODE-RPI4-01", false, [](const std::string&, const json&) { return true; });
  const std::int64_t now = hm::now_ms();
  const auto offer = relay.on_request(pairing_request(hm::new_id(), now), now);
  ASSERT_TRUE(offer);
  EXPECT_EQ((*offer)["status"], "unavailable");
  EXPECT_EQ((*offer)["reason"], "public_base_url is not set on the Mobile API");
  EXPECT_FALSE(offer->contains("qr_payload"));
  fs::remove_all(dir);
}

// ---------------------------------------------------------------------------------------------------------------
// Phone approvals (pfn/v1/mobile/task/action) and live events.
namespace {

json proposed_task(const std::string& task_id, const std::string& device, const json& control, int revision = 1) {
  json parameters = control;
  parameters["target_device_id"] = device;
  return {{"task_id", task_id},
          {"trace_id", "7d1f0c52-3a8e-4b8e-9a51-2f4f0e6c1a01"},
          {"intent", "device_control"},
          {"target_device", device},
          {"state", "proposed"},
          {"revision", revision},
          {"action_token", "token-" + task_id.substr(0, 12)},
          {"parameters", parameters}};
}

const std::string kLlmTask = "3b0a3f6e-1c2d-4e5f-8a9b-0c1d2e3f4a5b";
const std::string kDoorTask = "9c8b7a6d-5e4f-4a3b-8c2d-1e0f9a8b7c6d";

}  // namespace

TEST(Approvals, PhoneApprovesAndRejectsButNeverOpensTheEntranceDoor) {
  Fixture f;
  EXPECT_TRUE(f.approvals.observe(
      proposed_task(kLlmTask, "LED-AIRCON-01",
                    {{"origin", "llm"}, {"action", "set_switch"}, {"parameters", {{"on", true}}}}),
      1000));
  EXPECT_TRUE(f.approvals.observe(
      proposed_task(kDoorTask, "SERVO-DOOR-01",
                    {{"origin", "voice_parser"}, {"action", "set_position"}, {"parameters", {{"position_degrees", 90}}}}),
      2000));

  HttpResponse list = f.call("GET", "/api/v1/approvals");
  ASSERT_EQ(list.status, 200);
  ASSERT_EQ(list.body["approvals"].size(), 2u);
  std::map<std::string, json> by_id;
  for (const auto& item : list.body["approvals"]) by_id[item["task_id"]] = item;
  EXPECT_EQ(by_id[kLlmTask]["title"], "에어컨 표시등 켜기");
  EXPECT_EQ(by_id[kLlmTask]["origin"], "llm");
  EXPECT_TRUE(by_id[kLlmTask]["phone_can_approve"].get<bool>());
  EXPECT_TRUE(by_id[kLlmTask]["note"].is_null());
  EXPECT_EQ(by_id[kDoorTask]["title"], "현관문 열기");
  EXPECT_FALSE(by_id[kDoorTask]["phone_can_approve"].get<bool>());
  EXPECT_EQ(by_id[kDoorTask]["note"], "현관문 열기는 집 안 7인치 화면에서 승인해요");
  EXPECT_EQ(by_id[kDoorTask]["waiting_since_ms"], 2000);
  EXPECT_EQ(list.body.dump().find("token-"), std::string::npos) << "action tokens stay on the server";

  EXPECT_EQ(f.call("GET", "/api/v1/approvals", nullptr, std::string("wrong")).status, 401);
  EXPECT_EQ(f.call("POST", "/api/v1/approvals").status, 405);
  EXPECT_EQ(f.call("GET", "/api/v1/tasks/" + kLlmTask + "/approve").status, 405);
  EXPECT_EQ(f.call("POST", "/api/v1/tasks/" + kLlmTask + "/approve", nullptr, std::string("wrong")).status, 401);

  HttpResponse door = f.call("POST", "/api/v1/tasks/" + kDoorTask + "/approve");
  EXPECT_EQ(door.status, 403);
  EXPECT_TRUE(f.publisher.on(kMobileTaskAction).empty());

  HttpResponse reject = f.call("POST", "/api/v1/tasks/" + kDoorTask + "/reject");
  ASSERT_EQ(reject.status, 202);
  EXPECT_EQ(reject.body, json({{"task_id", kDoorTask}, {"action", "reject"}, {"status", "sent"}}));
  HttpResponse approve = f.call("POST", "/api/v1/tasks/" + kLlmTask + "/approve");
  ASSERT_EQ(approve.status, 202);
  const auto sent = f.publisher.on(kMobileTaskAction);
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_TRUE(f.publisher.on("pfn/v1/ui/action").empty()) << "the phone never poses as the 7-inch UI";
  EXPECT_EQ(sent[0]["action"], "reject");
  EXPECT_EQ(sent[0]["task_id"], kDoorTask);
  EXPECT_EQ(sent[1]["action"], "approve");
  EXPECT_EQ(sent[1]["task_id"], kLlmTask);
  EXPECT_EQ(sent[1]["client_id"], "homemind-mobile-api");
  EXPECT_EQ(sent[1]["expected_revision"], 1);
  EXPECT_EQ(sent[1]["action_token"], "token-" + kLlmTask.substr(0, 12));
  EXPECT_EQ(sent[1]["trace_id"], "7d1f0c52-3a8e-4b8e-9a51-2f4f0e6c1a01");

  EXPECT_EQ(f.call("POST", "/api/v1/tasks/00000000-0000-4000-8000-000000000000/approve").status, 404);

  // core answers: the LLM task runs and leaves the list; the phone can still poll its state
  json ready = proposed_task(kLlmTask, "LED-AIRCON-01", {{"origin", "llm"}}, 2);
  ready["state"] = "ready";
  EXPECT_TRUE(f.approvals.observe(ready, 3000));
  EXPECT_EQ(f.call("GET", "/api/v1/approvals").body["approvals"].size(), 1u);
  HttpResponse status = f.call("GET", "/api/v1/tasks/" + kLlmTask);
  ASSERT_EQ(status.status, 200);
  EXPECT_EQ(status.body["state"], "ready");
  EXPECT_EQ(f.call("POST", "/api/v1/tasks/" + kLlmTask + "/approve").status, 404);

  f.publisher.up = false;
  EXPECT_EQ(f.call("POST", "/api/v1/tasks/" + kDoorTask + "/reject").status, 503);
}

TEST(Approvals, StaleUpdatesAndSnapshotSync) {
  ApprovalBook book;
  const json first = proposed_task(kLlmTask, "LED-TV-01", {{"origin", "llm"}}, 3);
  EXPECT_TRUE(book.observe(first, 10));
  EXPECT_FALSE(book.observe(first, 11)) << "same proposal again changes nothing";
  json older = first;
  older["state"] = "rejected";
  older["revision"] = 2;
  EXPECT_FALSE(book.observe(older, 12)) << "a late, older update must not drop the approval";
  ASSERT_EQ(book.pending().size(), 1u);
  EXPECT_EQ(book.pending()[0].seen_ms, 10);
  EXPECT_EQ(book.state_of(kLlmTask), "proposed");

  json no_token = proposed_task(kDoorTask, "SERVO-DOOR-01", {{"origin", "ui"}});
  no_token.erase("action_token");
  EXPECT_FALSE(book.observe(no_token, 13));
  EXPECT_FALSE(book.observe(json::array(), 13));
  EXPECT_FALSE(book.observe({{"task_id", kDoorTask}}, 13));

  // snapshot: the LLM task was answered elsewhere while we were away, the door task now waits
  const json snapshot = json::array({proposed_task(kDoorTask, "SERVO-DOOR-01", {{"origin", "ui"}}, 1)});
  EXPECT_TRUE(book.sync(snapshot, 20));
  ASSERT_EQ(book.pending().size(), 1u);
  EXPECT_EQ(book.pending()[0].task_id, kDoorTask);
  EXPECT_FALSE(book.sync(snapshot, 21));
  EXPECT_FALSE(book.sync(json::object(), 22));
}

TEST(Approvals, ActionDocumentMatchesTheUiActionContract) {
  const json schema = json::parse(read_file(fs::path(HM_TEST_CONTRACTS_DIR) / "pfn.v1" / "ui-action.schema.json"));
  PendingApproval approval;
  approval.task_id = kLlmTask;
  approval.action_token = "0123456789abcdef0123";
  approval.revision = 4;
  const json document = ui_action_document(approval, "approve", "homemind-mobile-api", 1234);
  for (const auto& key : schema["required"]) EXPECT_TRUE(document.contains(key.get<std::string>())) << key;
  for (const auto& [key, value] : document.items()) EXPECT_TRUE(schema["properties"].contains(key)) << key;
  const std::regex uuid("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  EXPECT_TRUE(std::regex_match(document["event_id"].get<std::string>(), uuid));
  EXPECT_TRUE(std::regex_match(document["trace_id"].get<std::string>(), uuid)) << "a missing trace gets a new one";
  EXPECT_EQ(document["expected_revision"], 4);
  EXPECT_EQ(document["sent_at_ms"], 1234);

  Registry registry(kRegistry);
  approval.intent = "prepare_sleep";
  EXPECT_EQ(approval_title(approval, registry), "취침 준비");
  approval.intent = "make_coffee";
  EXPECT_EQ(approval_title(approval, registry), "make_coffee");
  approval.intent = "device_control";
  approval.target_device = "SERVO-WINDOW-01";
  approval.parameters = {{"action", "set_position"}, {"parameters", {{"position_degrees", 0}}}};
  EXPECT_EQ(approval_title(approval, registry), "창문 닫기");
  EXPECT_TRUE(phone_may_approve(approval));
  approval.target_device = "SERVO-DOOR-01";
  EXPECT_FALSE(phone_may_approve(approval));
}

TEST(EventHub, DeliversInOrderWakesWaitersAndCloses) {
  EventHub hub(3);
  EXPECT_EQ(hub.latest(), 0u);
  EXPECT_TRUE(hub.wait_after(0, std::chrono::milliseconds(20)).empty());
  hub.publish({{"type", "task"}, {"n", 1}});
  hub.publish({{"type", "task"}, {"n", 2}});
  auto events = hub.wait_after(0, std::chrono::milliseconds(20));
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0]["seq"], 1);
  EXPECT_EQ(events[1]["n"], 2);
  EXPECT_TRUE(events[1].contains("at_ms"));
  EXPECT_EQ(hub.wait_after(1, std::chrono::milliseconds(20)).size(), 1u);
  for (int i = 3; i <= 6; ++i) hub.publish({{"type", "task"}, {"n", i}});
  events = hub.wait_after(0, std::chrono::milliseconds(20));
  ASSERT_EQ(events.size(), 3u) << "keeps only the last 3";
  EXPECT_EQ(events[0]["seq"], 4);

  std::vector<json> woken;
  const std::uint64_t mark = hub.latest();
  std::thread waiter([&] { woken = hub.wait_after(mark, std::chrono::seconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  hub.publish({{"type", "approvals"}, {"pending", 1}});
  waiter.join();
  ASSERT_EQ(woken.size(), 1u);
  EXPECT_EQ(woken[0]["type"], "approvals");

  const std::uint64_t last = hub.latest();
  std::thread closing([&] { woken = hub.wait_after(last, std::chrono::seconds(5)); });
  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  hub.close();
  closing.join();
  EXPECT_TRUE(woken.empty());
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  EXPECT_TRUE(hub.closed());
}

TEST(PhonesRelay, ListsPhonesWithoutTokensAndUnpairsOne) {
  const fs::path dir = temp_dir("phones");
  TokenStore tokens(dir);
  std::string galaxy_token, pixel_token;
  const TokenRecord galaxy = tokens.issue("Galaxy S24", galaxy_token);
  const TokenRecord pixel = tokens.issue("Pixel 9", pixel_token);
  ASSERT_TRUE(tokens.verify(galaxy_token));  // the Galaxy made a request, the Pixel did not
  std::vector<std::pair<std::string, json>> sent;
  PhonesRelay phones(tokens, "NODE-RPI4-01", false, [&sent](const std::string& topic, const json& document) {
    sent.emplace_back(topic, document);
    return true;
  });
  const std::int64_t now = hm::now_ms();
  const json list_request = phones_request("list", now);
  const auto listed = phones.on_request(list_request, now);
  ASSERT_TRUE(listed);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first, "pfn/v1/mobile/phones/state");
  EXPECT_EQ((*listed)["request_id"], list_request["request_id"]);
  EXPECT_EQ((*listed)["action"], "list");
  EXPECT_EQ((*listed)["status"], "ok");
  EXPECT_EQ((*listed)["source_node_id"], "NODE-RPI4-01");
  EXPECT_EQ((*listed)["target_node_id"], "NODE-RPI5-01");
  EXPECT_FALSE(listed->contains("revoked"));
  EXPECT_FALSE(listed->contains("simulated"));
  ASSERT_EQ((*listed)["phones"].size(), 2u);
  for (const auto& phone : (*listed)["phones"]) {
    EXPECT_EQ(phone.size(), 4u) << phone.dump();  // phone_id, device_name, paired_at, last_seen_at_ms only
    if (phone["phone_id"] == galaxy.id) {
      EXPECT_EQ(phone["device_name"], "Galaxy S24");
      EXPECT_EQ(phone["paired_at"], galaxy.created_at);
      EXPECT_GE(phone["last_seen_at_ms"].get<std::int64_t>(), now - 60000);
    } else {
      EXPECT_EQ(phone["phone_id"], pixel.id);
      EXPECT_TRUE(phone["last_seen_at_ms"].is_null());
    }
  }
  const std::string text = listed->dump();
  for (const auto& secret : {galaxy_token, pixel_token, galaxy.sha256, pixel.sha256}) {
    EXPECT_EQ(text.find(secret), std::string::npos) << "a token or its hash reached the UI";
  }

  // unpair the Pixel: refused from now on, gone from the list
  const auto unpaired = phones.on_request(phones_request("revoke", now, pixel.id), now);
  ASSERT_TRUE(unpaired);
  EXPECT_EQ((*unpaired)["action"], "revoke");
  EXPECT_EQ((*unpaired)["revoked"], true);
  ASSERT_EQ((*unpaired)["phones"].size(), 1u);
  EXPECT_EQ((*unpaired)["phones"][0]["phone_id"], galaxy.id);
  EXPECT_FALSE(tokens.verify(pixel_token));
  EXPECT_TRUE(tokens.verify(galaxy_token));
  const auto unknown = phones.on_request(phones_request("revoke", now, "0badc0de"), now);
  ASSERT_TRUE(unknown);
  EXPECT_EQ((*unknown)["revoked"], false);
  EXPECT_EQ((*unknown)["phones"].size(), 1u);

  // a new pairing is announced without a request
  const json paired = phones.notify_paired(now);
  EXPECT_TRUE(paired["request_id"].is_null());
  EXPECT_EQ(paired["action"], "paired");
  EXPECT_EQ(sent.size(), 4u);
  fs::remove_all(dir);
}

TEST(PhonesRelay, IgnoresBadStaleReplayedAndForgedRequests) {
  const fs::path dir = temp_dir("phones-checks");
  TokenStore tokens(dir);
  int published = 0;
  PhonesRelay phones(tokens, "NODE-RPI4-01", true, [&published](const std::string&, const json&) {
    ++published;
    return true;
  });
  const std::int64_t now = hm::now_ms();
  const json first = phones_request("list", now);
  const auto answered = phones.on_request(first, now);
  ASSERT_TRUE(answered);
  EXPECT_EQ((*answered)["simulated"], true);
  EXPECT_TRUE((*answered)["phones"].empty());
  EXPECT_FALSE(phones.on_request(first, now + 10)) << "replay";
  EXPECT_EQ(phones.check(phones_request("list", now - 10000), now), "expired");
  EXPECT_EQ(phones.check(phones_request("list", now, "a1b2c3d4"), now), "phone_id") << "list names no phone";
  EXPECT_EQ(phones.check(phones_request("revoke", now), now), "phone_id") << "revoke needs a phone";
  EXPECT_EQ(phones.check(phones_request("revoke", now, "../tokens"), now), "phone_id");
  EXPECT_EQ(phones.check(phones_request("delete_all", now), now), "action");
  json forged = phones_request("revoke", now, "a1b2c3d4");
  forged["source_node_id"] = "NODE-RPI5-02";
  EXPECT_EQ(phones.check(forged, now), "source_node_id");
  json other = phones_request("list", now);
  other["target_node_id"] = "NODE-RPI5-01";
  EXPECT_EQ(phones.check(other, now), "target_node_id");
  json extra = phones_request("list", now);
  extra["token"] = "x";
  EXPECT_EQ(phones.check(extra, now), "unknown field token");
  json forever = phones_request("list", now);
  forever["expires_at_ms"] = now + 3600000;
  EXPECT_EQ(phones.check(forever, now), "lifetime");
  EXPECT_EQ(published, 1);
  fs::remove_all(dir);
}

TEST(Api, PairingClaimAnnouncesThePhoneList) {
  Fixture f;
  int announced = 0;
  f.api.on_paired([&announced] { ++announced; });
  const PairingOffer offer = f.pairing.make_offer(hm::now_ms() / 1000, "http://192.0.2.10:8000", 600);
  HttpRequest claim{"POST", "/api/v1/pairing/claim", "", "application/json",
                    json({{"code", offer.code}, {"nonce", offer.nonce}, {"device_name", "Galaxy"}}).dump(),
                    "198.51.100.7"};
  ASSERT_EQ(f.api.handle(claim).status, 201);
  EXPECT_EQ(f.api.handle(claim).status, 401);  // used code: no second announcement
  EXPECT_EQ(announced, 1);
}
