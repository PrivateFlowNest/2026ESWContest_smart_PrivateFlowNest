#include "hm/mqtt.hpp"

#include <mosquitto.h>

#include <chrono>
#include <csignal>
#include <stdexcept>
#include <thread>

namespace hm {

namespace {
struct LibraryInit {
  LibraryInit() { mosquitto_lib_init(); }
  ~LibraryInit() { mosquitto_lib_cleanup(); }
};
LibraryInit& library() {
  static LibraryInit init;
  return init;
}
}  // namespace

Bus::Bus(BrokerConfig broker, std::string client_id) : broker_(std::move(broker)), client_id_(std::move(client_id)) {
  library();
  client_ = mosquitto_new(client_id_.c_str(), true, this);
  if (!client_) throw std::runtime_error("mosquitto_new failed");
  if (broker_.username) {
    const auto password = broker_.password();
    if (mosquitto_username_pw_set(client_, broker_.username->c_str(), password ? password->c_str() : nullptr) !=
        MOSQ_ERR_SUCCESS) {
      throw std::runtime_error("mosquitto_username_pw_set failed");
    }
  }
  mosquitto_connect_callback_set(client_, &Bus::on_connect);
  mosquitto_disconnect_callback_set(client_, &Bus::on_disconnect);
  mosquitto_message_callback_set(client_, &Bus::on_message);
  mosquitto_publish_callback_set(client_, &Bus::on_publish);
  mosquitto_reconnect_delay_set(client_, 1, 30, true);
}

Bus::~Bus() {
  stop();
  if (client_) mosquitto_destroy(client_);
}

void Bus::subscribe(const std::string& topic_filter, Handler handler) {
  {
    std::lock_guard lock(mutex_);
    handlers_.emplace_back(topic_filter, std::move(handler));
  }
  if (connected_) mosquitto_subscribe(client_, nullptr, topic_filter.c_str(), 1);
}

void Bus::on_connected(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_connected_ = std::move(callback);
}

bool Bus::start(int timeout_ms) {
  if (started_) return connected_;
  started_ = true;
  const int rc = mosquitto_connect_async(client_, broker_.host.c_str(), broker_.port, broker_.keepalive);
  if (rc != MOSQ_ERR_SUCCESS) {
    log(LogLevel::warning, "mqtt", std::string("connect_async: ") + mosquitto_strerror(rc) + " (will retry)");
  }
  if (mosquitto_loop_start(client_) != MOSQ_ERR_SUCCESS) throw std::runtime_error("mosquitto_loop_start failed");
  if (rc != MOSQ_ERR_SUCCESS) mosquitto_reconnect_async(client_);
  std::unique_lock lock(mutex_);
  const bool ok = connected_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return connected_.load(); });
  if (!ok) {
    log(LogLevel::warning, "mqtt",
        "not connected yet to " + broker_.host + ":" + std::to_string(broker_.port) + "; will keep retrying");
  }
  return ok;
}

void Bus::stop() {
  if (!started_) return;
  if (connected_ && !flush(2000)) log(LogLevel::warning, "mqtt", "stopping with unconfirmed messages");
  started_ = false;
  mosquitto_disconnect(client_);
  mosquitto_loop_stop(client_, true);
  connected_ = false;
}

bool Bus::publish(const std::string& topic, const json& document, int qos) {
  const std::string payload = dump_compact(document);
  {
    std::lock_guard lock(mutex_);
    ++in_flight_;  // counted before the call: on_publish may run on the network thread before it returns
  }
  const int rc = mosquitto_publish(client_, nullptr, topic.c_str(), static_cast<int>(payload.size()), payload.data(),
                                   qos, false);
  if (rc != MOSQ_ERR_SUCCESS) {
    {
      std::lock_guard lock(mutex_);
      --in_flight_;
    }
    flushed_cv_.notify_all();
    log(LogLevel::error, "mqtt", "publish failed topic=" + topic + " rc=" + mosquitto_strerror(rc));
    return false;
  }
  return true;
}

bool Bus::flush(int timeout_ms) {
  std::unique_lock lock(mutex_);
  return flushed_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return in_flight_ <= 0; });
}

void Bus::on_publish(mosquitto*, void* self_ptr, int) {
  auto* self = static_cast<Bus*>(self_ptr);
  {
    std::lock_guard lock(self->mutex_);
    if (self->in_flight_ > 0) --self->in_flight_;
  }
  self->flushed_cv_.notify_all();
}

void Bus::on_connect(mosquitto* client, void* self_ptr, int rc) {
  auto* self = static_cast<Bus*>(self_ptr);
  if (rc != 0) {
    log(LogLevel::error, "mqtt", std::string("connect refused: ") + mosquitto_connack_string(rc));
    return;
  }
  std::vector<std::string> filters;
  std::function<void()> callback;
  {
    std::lock_guard lock(self->mutex_);
    for (const auto& [filter, handler] : self->handlers_) filters.push_back(filter);
    callback = self->on_connected_;
    self->connected_ = true;
  }
  for (const auto& filter : filters) mosquitto_subscribe(client, nullptr, filter.c_str(), 1);
  self->connected_cv_.notify_all();
  log(LogLevel::info, "mqtt", "connected " + self->broker_.host + ":" + std::to_string(self->broker_.port));
  if (callback) {
    try {
      callback();
    } catch (const std::exception& error) {
      log(LogLevel::error, "mqtt", std::string("on_connected failed: ") + error.what());
    }
  }
}

void Bus::on_disconnect(mosquitto*, void* self_ptr, int rc) {
  auto* self = static_cast<Bus*>(self_ptr);
  self->connected_ = false;
  if (rc != 0) log(LogLevel::warning, "mqtt", "disconnected unexpectedly; reconnecting");
}

void Bus::on_message(mosquitto*, void* self_ptr, const mosquitto_message* message) {
  auto* self = static_cast<Bus*>(self_ptr);
  std::vector<Handler> matched;
  {
    std::lock_guard lock(self->mutex_);
    for (const auto& [filter, handler] : self->handlers_) {
      bool result = false;
      if (mosquitto_topic_matches_sub(filter.c_str(), message->topic, &result) == MOSQ_ERR_SUCCESS && result) {
        matched.push_back(handler);
      }
    }
  }
  if (matched.empty()) return;
  json document;
  try {
    document = json::parse(static_cast<const char*>(message->payload),
                           static_cast<const char*>(message->payload) + message->payloadlen);
  } catch (const std::exception& error) {
    log(LogLevel::warning, "mqtt", std::string("invalid JSON on ") + message->topic);
    return;
  }
  const std::string topic = message->topic;
  for (const auto& handler : matched) {
    try {
      handler(topic, document);
    } catch (const std::exception& error) {  // one bad message must not stop the node
      log(LogLevel::error, "mqtt", "handler failed for " + topic + ": " + error.what());
    }
  }
}

namespace {
volatile std::sig_atomic_t g_stop = 0;
extern "C" void on_stop_signal(int) { g_stop = 1; }
}  // namespace

void wait_for_shutdown_signal() {
  std::signal(SIGINT, on_stop_signal);
  std::signal(SIGTERM, on_stop_signal);
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

}  // namespace hm
