#include "common.hpp"

#include <mosquitto.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <cerrno>
#include <sys/types.h>
#include <unistd.h>

namespace homemind {
using Json = nlohmann::json;

inline void require_mosquitto(int result, const std::string& operation) {
  if (result != MOSQ_ERR_SUCCESS) {
    throw std::runtime_error(operation + ": " + mosquitto_strerror(result));
  }
}

struct Options {
  std::string broker_host{"127.0.0.1"};
  int broker_port{28883};
  std::string database_path;
  std::string node_id{"homemind-esp32-soak-01"};
  int duration_seconds{7200};
  int interval_ms{200};
  int report_interval_seconds{60};
  pid_t core_pid{0};
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (index + 1 >= argc) {
      throw std::runtime_error("missing value for " + argument);
    }
    const std::string value = argv[++index];
    if (argument == "--broker-host") {
      options.broker_host = value;
    } else if (argument == "--broker-port") {
      options.broker_port = std::stoi(value);
    } else if (argument == "--database") {
      options.database_path = value;
    } else if (argument == "--node-id") {
      options.node_id = value;
    } else if (argument == "--duration-seconds") {
      options.duration_seconds = std::stoi(value);
    } else if (argument == "--interval-ms") {
      options.interval_ms = std::stoi(value);
    } else if (argument == "--report-interval-seconds") {
      options.report_interval_seconds = std::stoi(value);
    } else if (argument == "--core-pid") {
      options.core_pid = static_cast<pid_t>(std::stoi(value));
    } else {
      throw std::runtime_error("unknown option: " + argument);
    }
  }
  if (options.database_path.empty() || options.node_id.empty() || options.node_id.size() > 64 ||
      options.broker_port < 1 || options.broker_port > 65535 || options.duration_seconds < 1 ||
      options.interval_ms < 20 || options.report_interval_seconds < 1 || options.core_pid <= 0) {
    throw std::runtime_error("invalid soak-test options");
  }
  return options;
}

class ReadOnlyDatabase {
 public:
  explicit ReadOnlyDatabase(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
      const std::string error = database_ ? sqlite3_errmsg(database_) : "open failed";
      if (database_) {
        sqlite3_close(database_);
      }
      throw std::runtime_error("cannot open soak database: " + error);
    }
    sqlite3_busy_timeout(database_, 5000);
  }

  ~ReadOnlyDatabase() { sqlite3_close(database_); }
  ReadOnlyDatabase(const ReadOnlyDatabase&) = delete;
  ReadOnlyDatabase& operator=(const ReadOnlyDatabase&) = delete;

  std::int64_t telemetry_count(const std::string& node_id) {
    sqlite3_stmt* statement = nullptr;
    const char* sql = "SELECT COUNT(*) FROM telemetry WHERE node_id=?;";
    if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
      throw std::runtime_error("cannot prepare telemetry count");
    }
    sqlite3_bind_text(statement, 1, node_id.c_str(), -1, SQLITE_TRANSIENT);
    const int result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
      sqlite3_finalize(statement);
      throw std::runtime_error("cannot read telemetry count");
    }
    const std::int64_t count = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return count;
  }

  std::string integrity_check() {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, "PRAGMA integrity_check;", -1, &statement, nullptr) !=
        SQLITE_OK) {
      throw std::runtime_error("cannot prepare integrity check");
    }
    const int result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
      sqlite3_finalize(statement);
      throw std::runtime_error("cannot run integrity check");
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    const std::string value = text ? text : "";
    sqlite3_finalize(statement);
    return value;
  }

 private:
  sqlite3* database_{nullptr};
};

std::uint64_t file_size_or_zero(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

std::uint64_t database_size(const std::string& path) {
  return file_size_or_zero(path) + file_size_or_zero(path + "-wal") +
         file_size_or_zero(path + "-shm");
}

bool process_alive(pid_t pid) {
  return kill(pid, 0) == 0 || errno == EPERM;
}

std::int64_t read_rss_kib(pid_t pid) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::istringstream stream(line.substr(6));
      std::int64_t value = 0;
      stream >> value;
      return value;
    }
  }
  throw std::runtime_error("cannot read homemind-core RSS");
}

struct MqttContext {
  std::atomic<bool> connected{false};
};

void on_connect(mosquitto*, void* userdata, int result) {
  static_cast<MqttContext*>(userdata)->connected = result == 0;
}

void on_disconnect(mosquitto*, void* userdata, int) {
  static_cast<MqttContext*>(userdata)->connected = false;
}

std::atomic<bool> running{true};
void stop_handler(int) { running = false; }

void publish_json(mosquitto* client, const std::string& topic, const Json& value) {
  const std::string payload = value.dump();
  require_mosquitto(mosquitto_publish(client, nullptr, topic.c_str(),
                                      static_cast<int>(payload.size()), payload.data(), 0, false),
                    "publish soak message");
}

int run(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  if (!process_alive(options.core_pid)) {
    throw std::runtime_error("homemind-core PID is not alive before soak test");
  }

  ReadOnlyDatabase initial_database(options.database_path);
  const std::int64_t initial_count = initial_database.telemetry_count(options.node_id);
  const std::int64_t initial_rss_kib = read_rss_kib(options.core_pid);
  std::int64_t warm_rss_kib = initial_rss_kib;
  bool warm_rss_captured = false;
  std::int64_t minimum_rss_kib = initial_rss_kib;
  std::int64_t maximum_rss_kib = initial_rss_kib;

  require_mosquitto(mosquitto_lib_init(), "mosquitto_lib_init");
  MqttContext context;
  std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)> client(
      mosquitto_new(("soak-publisher-" + uuid_v4()).c_str(), true, &context), mosquitto_destroy);
  if (!client) {
    throw std::runtime_error("cannot create soak MQTT client");
  }
  mosquitto_connect_callback_set(client.get(), on_connect);
  mosquitto_disconnect_callback_set(client.get(), on_disconnect);
  mosquitto_reconnect_delay_set(client.get(), 1, 10, true);
  require_mosquitto(mosquitto_connect_async(client.get(), options.broker_host.c_str(),
                                            options.broker_port, 30),
                    "connect soak MQTT client");
  require_mosquitto(mosquitto_loop_start(client.get()), "start soak MQTT loop");

  for (int attempt = 0; attempt < 100 && !context.connected; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!context.connected) {
    throw std::runtime_error("soak MQTT client did not connect");
  }

  std::signal(SIGINT, stop_handler);
  std::signal(SIGTERM, stop_handler);
  const std::string trace_id = uuid_v4();
  const std::string telemetry_topic = "pfn/v1/device/" + options.node_id + "/telemetry";
  const auto started = std::chrono::steady_clock::now();
  auto next_publish = started;
  auto next_heartbeat = started;
  auto next_report = started + std::chrono::seconds(options.report_interval_seconds);
  const auto finish_at = started + std::chrono::seconds(options.duration_seconds);
  std::uint64_t telemetry_published = 0;
  std::uint64_t heartbeat_published = 0;
  std::uint64_t sequence = 0;
  std::uint64_t publish_failures = 0;

  std::cout << "SOAK_STARTED duration_seconds=" << options.duration_seconds
            << " interval_ms=" << options.interval_ms << " core_pid=" << options.core_pid
            << " initial_rss_kib=" << initial_rss_kib << std::endl;

  while (running && std::chrono::steady_clock::now() < finish_at) {
    const auto current = std::chrono::steady_clock::now();
    if (!process_alive(options.core_pid)) {
      throw std::runtime_error("homemind-core stopped or restarted during soak test");
    }
    if (current >= next_publish) {
      const double fan_level = static_cast<double>(sequence % 101);
      const Json telemetry{{"schema_version", "pfn.v1"},
                           {"event_id", uuid_v4()},
                           {"trace_id", trace_id},
                           {"node_id", options.node_id},
                           {"seq", sequence},
                           {"sent_at_ms", now_ms()},
                           {"uptime_ms", uptime_ms()},
                           {"sensor_ready", true},
                           {"simulated_value", fan_level},
                           {"readings", {{"fan_level_percent", fan_level}}},
                           {"reading_sources", {{"fan_level_percent", "FAN-01"}}}};
      try {
        publish_json(client.get(), telemetry_topic, telemetry);
        ++telemetry_published;
      } catch (const std::exception&) {
        ++publish_failures;
      }
      ++sequence;
      next_publish += std::chrono::milliseconds(options.interval_ms);
    }
    if (current >= next_heartbeat) {
      const Json heartbeat{{"schema_version", "pfn.v1"},
                           {"event_id", uuid_v4()},
                           {"trace_id", trace_id},
                           {"node_id", options.node_id},
                           {"node_type", "esp32"},
                           {"status", "online"},
                           {"seq", heartbeat_published},
                           {"sent_at_ms", now_ms()},
                           {"uptime_ms", uptime_ms()},
                           {"version", "soaktest-0.3.1"}};
      try {
        publish_json(client.get(), "pfn/v1/system/heartbeat", heartbeat);
        ++heartbeat_published;
      } catch (const std::exception&) {
        ++publish_failures;
      }
      next_heartbeat += std::chrono::seconds(5);
    }
    if (current >= next_report) {
      const std::int64_t rss_kib = read_rss_kib(options.core_pid);
      minimum_rss_kib = std::min(minimum_rss_kib, rss_kib);
      maximum_rss_kib = std::max(maximum_rss_kib, rss_kib);
      if (!warm_rss_captured) {
        warm_rss_kib = rss_kib;
        warm_rss_captured = true;
      }
      ReadOnlyDatabase progress_database(options.database_path);
      const std::int64_t stored = progress_database.telemetry_count(options.node_id) - initial_count;
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current - started).count();
      std::cout << "SOAK_PROGRESS elapsed_seconds=" << elapsed
                << " telemetry_published=" << telemetry_published
                << " telemetry_stored=" << stored << " core_rss_kib=" << rss_kib
                << " database_kib=" << database_size(options.database_path) / 1024 << std::endl;
      next_report += std::chrono::seconds(options.report_interval_seconds);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));
  mosquitto_disconnect(client.get());
  mosquitto_loop_stop(client.get(), true);
  client.reset();
  mosquitto_lib_cleanup();

  ReadOnlyDatabase final_database(options.database_path);
  const std::int64_t final_count = final_database.telemetry_count(options.node_id);
  const std::int64_t stored_delta = final_count - initial_count;
  const std::string integrity = final_database.integrity_check();
  const bool core_alive = process_alive(options.core_pid);
  const std::int64_t final_rss_kib = core_alive ? read_rss_kib(options.core_pid) : -1;
  if (final_rss_kib >= 0) {
    minimum_rss_kib = std::min(minimum_rss_kib, final_rss_kib);
    maximum_rss_kib = std::max(maximum_rss_kib, final_rss_kib);
  }
  const std::int64_t rss_growth_kib = final_rss_kib - warm_rss_kib;
  const double delivery_percent = telemetry_published == 0
                                      ? 0.0
                                      : 100.0 * static_cast<double>(stored_delta) /
                                            static_cast<double>(telemetry_published);
  const auto actual_duration_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count();
  const bool passed = running && core_alive && integrity == "ok" && publish_failures == 0 &&
                      delivery_percent >= 99.0 && rss_growth_kib <= 16384;

  const Json result{{"passed", passed},
                    {"requested_duration_seconds", options.duration_seconds},
                    {"actual_duration_seconds", actual_duration_seconds},
                    {"interval_ms", options.interval_ms},
                    {"telemetry_published", telemetry_published},
                    {"telemetry_stored", stored_delta},
                    {"delivery_percent", delivery_percent},
                    {"heartbeat_published", heartbeat_published},
                    {"publish_failures", publish_failures},
                    {"core_pid_unchanged", core_alive},
                    {"core_rss_initial_kib", initial_rss_kib},
                    {"core_rss_warm_kib", warm_rss_kib},
                    {"core_rss_final_kib", final_rss_kib},
                    {"core_rss_min_kib", minimum_rss_kib},
                    {"core_rss_max_kib", maximum_rss_kib},
                    {"core_rss_growth_kib", rss_growth_kib},
                    {"database_bytes", database_size(options.database_path)},
                    {"integrity_check", integrity}};
  std::cout << "SOAK_RESULT " << result.dump() << std::endl;
  return passed ? 0 : 1;
}

}  // namespace homemind

int main(int argc, char** argv) {
  try {
    return homemind::run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "SOAK_FATAL " << error.what() << std::endl;
    return 1;
  }
}
