// Remote push-to-talk: pfn/v1/ai/voice/listen (NODE-RPI5-01 7-inch UI button -> NODE-RPI5-02).
// contracts/pfn.v1/voice-listen.schema.json, policy in protocol.json topic_policies.ai_voice_listen.
//   start  -> exactly what the local trigger "start" does (stdin/FIFO): begin listening for one utterance;
//             ignored while already listening
//   cancel -> stop listening and discard what was captured (nothing is transcribed); ignored while idle
// A request is acted on only when it is schema-valid, addressed to this node, not expired (now <= expires_at_ms)
// and not a duplicate (QoS 1 redelivery). Everything else is ignored with a log line that never quotes the
// message. Only active while the voice path runs in push_to_talk mode (otherwise ignored, logged).
// The start's trace_id (when it is lower-case RFC 4122 text) becomes the utterance trace: its partial-intent hints and
// the task/llm request of the final transcript carry the UI press's trace (the UI keeps it for its cancel too).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

#include "hm/audio/ptt.hpp"
#include "hm/common.hpp"

namespace hm::perception {

inline constexpr const char* kVoiceListenTopic = "pfn/v1/ai/voice/listen";  // protocol.json topics.ai_voice_listen

// "" when the document satisfies voice-listen.schema.json (Draft 2020-12, format "uuid" as jsonschema's
// FormatChecker), else the first problem as "<field>: <rule>" (never the value).
std::string validate_voice_listen(const json& document);

class RemoteListen {
 public:
  explicit RemoteListen(std::string node_id = "NODE-RPI5-02", std::size_t remembered_events = 256);

  // The MQTT handler body. Returns what happened: "start", "cancel", or "ignored:<reason>" with reason one of
  // invalid, wrong_target, expired, duplicate, inactive.
  std::string handle(const json& document, std::int64_t now_ms);
  // The voice loop's push-to-talk control while it runs (nullptr = not listening for presses).
  void attach(hm::audio::PttControl* control);

  struct Counts {
    std::size_t start = 0, cancel = 0, invalid = 0, wrong_target = 0, expired = 0, duplicate = 0, inactive = 0;
  };
  Counts counts() const;

 private:
  bool remember(const std::string& event_id);  // false when already seen

  std::string node_id_;
  std::size_t remembered_;
  mutable std::mutex mutex_;
  hm::audio::PttControl* control_ = nullptr;
  std::deque<std::string> seen_order_;
  std::unordered_set<std::string> seen_;
  Counts counts_;
};

}  // namespace hm::perception
