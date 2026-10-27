// libmosquitto wrapper with the same behaviour as services/runtime Bus:
// JSON publish/subscribe (QoS 1), reconnect 1..30 s, resubscribe on every connect,
// one bad message never stops the node.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "hm/common.hpp"
#include "hm/config.hpp"

struct mosquitto;
struct mosquitto_message;

namespace hm {

class Bus {
 public:
  using Handler = std::function<void(const std::string& topic, const json& document)>;

  Bus(BrokerConfig broker, std::string client_id);
  ~Bus();
  Bus(const Bus&) = delete;
  Bus& operator=(const Bus&) = delete;

  void subscribe(const std::string& topic_filter, Handler handler);
  // Called on the network thread after every (re)connect, once the subscriptions are renewed
  // (e.g. to ask core for a fresh snapshot). Set it before start().
  void on_connected(std::function<void()> callback);
  // Starts the network thread. Returns true when connected within timeout_ms (keeps retrying either way).
  bool start(int timeout_ms = 10000);
  void stop();
  // Returns false when the client refused the message (not connected is still queued by mosquitto).
  bool publish(const std::string& topic, const json& document, int qos = 1);
  bool connected() const { return connected_.load(); }
  // Waits until every message handed to publish() has gone out (QoS 1: PUBACK received), up to timeout_ms.
  // stop() calls it first so a short-lived client (homemind-voice-text) does not drop its last message.
  bool flush(int timeout_ms);

 private:
  static void on_connect(mosquitto* client, void* self, int rc);
  static void on_disconnect(mosquitto* client, void* self, int rc);
  static void on_message(mosquitto* client, void* self, const mosquitto_message* message);
  static void on_publish(mosquitto* client, void* self, int mid);

  BrokerConfig broker_;
  std::string client_id_;
  mosquitto* client_ = nullptr;
  std::mutex mutex_;
  std::condition_variable connected_cv_;
  std::vector<std::pair<std::string, Handler>> handlers_;
  std::function<void()> on_connected_;
  std::atomic<bool> connected_{false};
  bool started_ = false;
  int in_flight_ = 0;  // published but not yet confirmed by on_publish (guarded by mutex_)
  std::condition_variable flushed_cv_;
};

// Blocks until SIGINT/SIGTERM.
void wait_for_shutdown_signal();

}  // namespace hm
