#include "runner.hpp"

#include <signal.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <thread>

#include "hm/audio/capture.hpp"
#include "hm/audio/vad.hpp"
#include "hm/audio/wav.hpp"
#include "hm/heartbeat.hpp"
#include "hm/mqtt.hpp"
#include "presence_feed.hpp"

namespace hm::perception {

namespace {

std::atomic<bool> g_stop{false};
void handle_stop_signal(int) { g_stop = true; }
// pfn/v1/system/privacy/state microphone_allowed (sound level telemetry). Allowed until core says otherwise.
std::atomic<bool> g_microphone_allowed{true};

constexpr double kSr = kSampleRate;

// Lower-case RFC 4122 text (what pfn.v1 builders and the parser accept as a trace_id).
bool canonical_uuid(const std::string& text) {
  if (text.size() != 36) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string fixed(double value, int digits) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.*f", digits, value);
  return buffer;
}

std::vector<float> load_inputs(const Settings& settings) {
  std::vector<float> waveform;
  for (const auto& path : settings.wav_input) {
    std::vector<float> part;
    if (settings.wav_offset_seconds <= 0 && !settings.wav_max_seconds) {
      part = hm::audio::load_wav_16k_mono(path);
    } else {
      // Segment read keeps hour-long recordings out of memory; offset/max apply to every file (Python).
      const double end = settings.wav_offset_seconds + settings.wav_max_seconds.value_or(24 * 3600.0);
      part = hm::audio::load_wav_segment_16k_mono(path, settings.wav_offset_seconds, end);
    }
    waveform.insert(waveform.end(), part.begin(), part.end());
  }
  return waveform;
}

}  // namespace

void request_stop() { g_stop = true; }
bool stop_requested() { return g_stop.load(); }

void context_loop(PerceptionService& service, hm::audio::AudioRing& ring, int consumer, bool lossy) {
  const Settings& settings = service.settings();
  const auto window = static_cast<std::size_t>(std::nearbyint(settings.window_seconds * kSr));
  const auto hop = static_cast<std::size_t>(std::nearbyint(settings.hop_seconds * kSr));
  std::vector<float> buffer;
  std::int64_t start = 0;  // stream index of buffer[0]
  std::size_t processed = 0;
  hm::audio::AudioBlock block;
  while (ring.read(consumer, block)) {
    if (block.samples.empty()) continue;
    if (buffer.empty()) {
      start = block.start_sample;
    } else if (block.start_sample != start + static_cast<std::int64_t>(buffer.size())) {
      hm::log(hm::LogLevel::warning, "perception", "context path lost audio (lagging consumer); restarting the window");
      buffer.clear();
      start = block.start_sample;
    }
    buffer.insert(buffer.end(), block.samples.begin(), block.samples.end());
    if (lossy && buffer.size() > window + 3 * hop) {
      const std::size_t skip = ((buffer.size() - window) / hop) * hop;
      hm::log(hm::LogLevel::warning, "perception",
              "context path behind by " + fixed(static_cast<double>(skip) / kSr, 1) + "s; skipping ahead");
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(skip));
      start += static_cast<std::int64_t>(skip);
    }
    while (buffer.size() >= window) {
      try {
        service.process_window(buffer.data(), window, start + static_cast<std::int64_t>(window));
      } catch (const std::exception& error) {  // one bad window must not stop the node
        hm::log(hm::LogLevel::error, "perception", std::string("context window failed: ") + error.what());
      }
      ++processed;
      const std::size_t drop = std::min(hop, buffer.size());
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(drop));
      start += static_cast<std::int64_t>(hop);
    }
    service.context_progress(start + static_cast<std::int64_t>(buffer.size()));
  }
  if (processed == 0 && !buffer.empty()) {  // input shorter than one window: one zero-padded window
    std::vector<float> padded(buffer);
    padded.resize(std::max(window, buffer.size()), 0.0f);
    try {
      service.process_window(padded.data(), window, start + static_cast<std::int64_t>(buffer.size()));
    } catch (const std::exception& error) {
      hm::log(hm::LogLevel::error, "perception", std::string("context window failed: ") + error.what());
    }
  }
  service.context_finished();
}

void voice_loop(PerceptionService& service, hm::audio::AudioRing& ring, int consumer, const std::string& mode,
                hm::audio::PttControl* control, std::vector<float>* whole, PartialHintWorker* partial,
                std::string* whole_trace) {
  hm::audio::AudioBlock block;
  if (mode == "whole") {
    std::string trace;
    if (partial) {
      trace = hm::new_id();
      partial->begin(trace);
    }
    while (ring.read(consumer, block)) {
      if (whole) whole->insert(whole->end(), block.samples.begin(), block.samples.end());
      if (partial) partial->audio(block.samples.data(), block.samples.size());
    }
    if (partial) partial->end();
    if (whole_trace) *whole_trace = trace;
    return;
  }
  const Settings& settings = service.settings();
  hm::audio::EnergyEndpointer endpointer(settings.vad, /*armed=*/mode == "always");
  const double timeout = settings.ptt_timeout_seconds;
  // Push-to-talk deadline for the onset (no std::optional: GCC -O3 reports a false maybe-uninitialized).
  bool deadline_set = false;
  double deadline = 0.0;
  double audio_time = 0.0;
  auto echo = [&](const std::string& line) { service.echo(line); };

  // Partial transcripts: stream the open utterance (pre-roll included) while it is captured.
  bool streaming = false;
  std::uint64_t streamed_activation = 0;
  std::size_t streamed_frames = 0;
  std::optional<std::string> trace;
  std::optional<std::string> press_trace;  // a remote press's trace_id becomes the utterance trace
  std::vector<float> chunk;
  auto stream_open = [&] {
    if (!partial || !endpointer.in_speech()) return;
    if (!streaming || endpointer.activations() != streamed_activation) {
      if (streaming) partial->end();
      trace = press_trace ? *press_trace : hm::new_id();
      partial->begin(*trace);
      streaming = true;
      streamed_activation = endpointer.activations();
      streamed_frames = 0;
    }
    const std::size_t frames = endpointer.active_frame_count();
    if (frames > streamed_frames) {
      chunk.clear();
      endpointer.copy_active_frames(streamed_frames, chunk);
      partial->audio(chunk.data(), chunk.size());
      streamed_frames = frames;
    }
  };
  // The open utterance ended: finished = what goes to whisper (its unsent tail is streamed), nullptr = discarded.
  auto stream_close = [&](const hm::audio::Utterance* finished) -> std::optional<std::string> {
    if (!partial || !streaming) return std::nullopt;
    if (finished) {
      const std::size_t sent = streamed_frames * endpointer.frame_samples();
      if (finished->samples.size() > sent) {
        partial->audio(finished->samples.data() + sent, finished->samples.size() - sent);
      }
    }
    partial->end();
    streaming = false;
    return std::exchange(trace, std::nullopt);
  };
  // The trace an ended utterance goes to the parser with (nullopt: the parser makes one).
  auto utterance_trace_of = [&](std::optional<std::string> streamed) {
    std::optional<std::string> chosen = streamed ? std::move(streamed) : press_trace;
    press_trace.reset();
    return chosen;
  };

  for (;;) {
    if (!ring.read(consumer, block)) {
      if (auto last = endpointer.stop("flush")) {
        const auto utterance_trace = utterance_trace_of(stream_close(&*last));
        service.on_utterance(last->samples, audio_time, utterance_trace);
      } else {
        stream_close(nullptr);
      }
      break;
    }
    if (block.samples.empty()) continue;
    audio_time = static_cast<double>(block.start_sample + static_cast<std::int64_t>(block.samples.size())) / kSr;
    while (control) {
      const auto request = control->poll_request();
      if (!request) break;
      const auto command = &request->command;
      const bool idle = !endpointer.in_speech();
      if (*command == hm::audio::PttCommand::cancel) {  // remote cancel / FIFO "cancel": discard, no whisper
        if (idle) continue;
        deadline_set = false;
        endpointer.reset();
        stream_close(nullptr);
        press_trace.reset();
        echo("듣기를 취소했습니다. Enter를 누르고 말하세요");
      } else if (idle && *command != hm::audio::PttCommand::stop) {
        press_trace.reset();
        if (canonical_uuid(request->trace_id)) press_trace = request->trace_id;
        endpointer.begin();
        deadline = audio_time + timeout;
        deadline_set = true;
        echo("듣는 중... (말이 끝나면 자동 종료, Enter = 바로 종료)");
        stream_open();
      } else if (!idle && *command != hm::audio::PttCommand::start) {
        deadline_set = false;
        auto manual = endpointer.stop("manual");
        if (manual) {
          const auto utterance_trace = utterance_trace_of(stream_close(&*manual));
          service.on_utterance(manual->samples, audio_time, utterance_trace);
        } else {
          stream_close(nullptr);
          press_trace.reset();
          echo("음성이 너무 짧습니다. Enter를 누르고 다시 말하세요");
        }
      }
    }
    const auto utterances = endpointer.feed(block.samples);
    for (const auto& utterance : utterances) {
      deadline_set = false;
      const auto utterance_trace = utterance_trace_of(stream_close(&utterance));
      service.on_utterance(utterance.samples, audio_time, utterance_trace);
      if (mode == "push_to_talk") echo("Enter를 누르고 말하세요");
    }
    if (streaming && (!endpointer.in_speech() || endpointer.activations() != streamed_activation)) {
      stream_close(nullptr);  // the open utterance was dropped (too short / no voiced frames)
    }
    stream_open();
    const bool timed_out = deadline_set && endpointer.in_speech() && !endpointer.onset_seen() && audio_time > deadline;
    if (timed_out || (deadline_set && !endpointer.in_speech() && utterances.empty())) {
      // no onset before the timeout, or the utterance hit max_ms / was too short to keep
      deadline_set = false;
      endpointer.reset();
      stream_close(nullptr);
      press_trace.reset();
      echo("음성이 감지되지 않았습니다. Enter를 누르고 다시 말하세요");
    }
  }
}

RunSummary run_with(const hm::RuntimeConfig& config, const Settings& settings, Publisher& publisher, const Hooks& hooks,
                    RemoteListen* remote) {
  const bool file_mode = !settings.wav_input.empty();
  std::vector<float> waveform;
  std::string voice_mode;
  std::unique_ptr<hm::audio::AudioRing> ring;
  if (file_mode) {
    waveform = load_inputs(settings);
    ring = std::make_unique<hm::audio::AudioRing>(30 * kSampleRate, /*lossy=*/false);
    voice_mode = settings.wav_voice;
    if (voice_mode == "auto") {
      voice_mode = static_cast<double>(waveform.size()) / kSr <= settings.max_whole_file_seconds ? "whole" : "off";
      if (voice_mode == "off") {
        hm::log(hm::LogLevel::info, "perception",
                "file longer than " + fixed(settings.max_whole_file_seconds, 0) + "s: voice path off (use --voice vad)");
      }
    } else if (voice_mode == "vad") {
      voice_mode = "always";
    } else if (voice_mode == "ptt") {  // native: push-to-talk on a real-time file (PC tests of the triggers)
      voice_mode = "push_to_talk";
    }
  } else {
    ring = std::make_unique<hm::audio::AudioRing>(30 * kSampleRate, /*lossy=*/true);  // 30 s backlog (Python 300 blocks)
    voice_mode = settings.listen_mode;
  }
  if (file_mode && settings.wav_fast && voice_mode == "push_to_talk") {
    hm::log(hm::LogLevel::warning, "perception", "--fast ignored: push-to-talk file playback runs in real time");
  }
  bool voice_enabled = voice_mode != "off";
  const bool simulated = file_mode ? true : config.simulated;
  hm::ActionMap action_map(config.contracts_dir);

  std::unique_ptr<ContextEngine> context;
  if (hooks.context) {
    context = hooks.context(settings, simulated);
  } else if (context_engine_available()) {
    context = make_context_engine(settings, simulated);  // missing model files: fail like Python
  } else {
    hm::log(hm::LogLevel::error, "perception", "context path disabled: built without libs/hm_context");
  }
  std::unique_ptr<SpeechToText> transcriber;
  std::unique_ptr<VoiceDecider> parser;
  if (voice_enabled) {
    try {
      transcriber = hooks.transcriber ? hooks.transcriber(settings) : make_transcriber(settings);
      parser = hooks.parser ? hooks.parser(action_map, settings, config, simulated)
                            : make_voice_decider(action_map, settings, config, simulated);
    } catch (const std::exception& error) {
      hm::log(hm::LogLevel::error, "perception", std::string("voice path disabled: ") + error.what());
      transcriber.reset();
      parser.reset();
    }
  }
  PerceptionService service(settings, publisher, action_map, context.get(), transcriber.get(), parser.get(), simulated,
                            echo_stdout, config.perception_node_id);
  if (file_mode) service.enable_context_sync();
  if (!file_mode && settings.sound_level.enabled) {
    service.enable_sound_level([] { return g_microphone_allowed.load(); });
    hm::log(hm::LogLevel::info, "perception",
            "sound level every " + fixed(settings.sound_level.interval_seconds, 0) + "s as " + kMicrophoneDeviceId +
                " sound_level_db (estimate: dBFS + " + fixed(settings.sound_level.offset_db, 0) +
                ", no calibration; off while privacy/state disallows the microphone)");
  }
  if (context) context->warmup();
  if (!transcriber || !parser) voice_enabled = false;

  // Partial-intent hints (Vosk partials -> partial-intent gate -> PARTIAL_INTENT_HINT), off by default.
  std::unique_ptr<PartialTranscriber> partial_stt;
  std::unique_ptr<PartialIntentModel> partial_model;
  std::unique_ptr<PartialHintWorker> partial;
  if (settings.partial_intent.enabled && !voice_enabled) {
    hm::log(hm::LogLevel::warning, "perception", "partial_intent enabled but the voice path is off: no hints");
  } else if (settings.partial_intent.enabled) {
    try {
      partial_stt = hooks.partial_transcriber ? hooks.partial_transcriber(settings) : make_partial_transcriber(settings);
      partial_model = hooks.partial_intent ? hooks.partial_intent(settings) : make_partial_intent_model(settings);
      PartialHintWorker::Options options;
      options.drain_at_end = file_mode;
      options.confirmation_updates = settings.partial_intent.confirmation_updates;
      options.max_queue_seconds = settings.partial_intent.max_queue_seconds;
      options.simulated = simulated;
      options.debug_transcripts = settings.debug_transcripts;
      partial = std::make_unique<PartialHintWorker>(*partial_stt, *partial_model, publisher, action_map, options);
      hm::log(hm::LogLevel::info, "perception",
              "partial-intent hints on: " + partial_stt->model_version() + " partials -> " +
                  partial_model->model_version() + " (preparation hints only, execution_allowed false)");
    } catch (const std::exception& error) {
      hm::log(hm::LogLevel::error, "perception", std::string("partial-intent hints disabled: ") + error.what());
      partial.reset();
      partial_model.reset();
      partial_stt.reset();
    }
  }

  // Live camera presence (homemind-vision -> presence_fifo) into context-fusion.
  std::unique_ptr<PresenceFeed> presence;
  if (settings.presence_fifo && !context) {
    hm::log(hm::LogLevel::error, "perception", "presence_fifo ignored: the context path is disabled");
  } else if (settings.presence_fifo) {
    presence = std::make_unique<PresenceFeed>([engine = context.get()](const json& event) { engine->submit_presence(event); });
    try {
      presence->start(*settings.presence_fifo);
      hm::log(hm::LogLevel::info, "perception",
              "live presence from " + settings.presence_fifo->string() + " (homemind-vision PRESENCE -> context-fusion)");
    } catch (const std::exception& error) {
      hm::log(hm::LogLevel::error, "perception", std::string("live presence disabled: ") + error.what());
      presence.reset();
    }
  }

  const int context_consumer = context ? ring->add_consumer("context") : -1;
  const int voice_consumer = voice_enabled ? ring->add_consumer("voice") : -1;
  std::unique_ptr<hm::audio::PttControl> control;
  if (voice_enabled && voice_mode == "push_to_talk") control = std::make_unique<hm::audio::PttControl>();
  struct RemoteAttachment {  // the MQTT handler may only reach the control while it exists
    RemoteListen* remote;
    RemoteAttachment(RemoteListen* r, hm::audio::PttControl* control) : remote(r) {
      if (remote) remote->attach(control);
    }
    ~RemoteAttachment() {
      if (remote) remote->attach(nullptr);
    }
  } remote_attachment(remote, control.get());
  if (remote && control) {
    hm::log(hm::LogLevel::info, "perception", std::string("remote push-to-talk: listening on ") + kVoiceListenTopic);
  } else if (remote) {
    hm::log(hm::LogLevel::warning, "perception",
            std::string(kVoiceListenTopic) + " requests will be ignored (voice path not in push_to_talk mode)");
  }
  std::vector<float> whole;
  std::string whole_trace;
  std::atomic<int> alive{0};
  std::vector<std::thread> threads;
  service.set_base_time_ms(hm::now_ms());
  if (context) {
    ++alive;
    threads.emplace_back([&] {
      try {
        context_loop(service, *ring, context_consumer, ring->lossy());
      } catch (const std::exception& error) {
        hm::log(hm::LogLevel::error, "perception", std::string("context path stopped: ") + error.what());
      }
      ring->detach(context_consumer);
      service.context_finished();
      --alive;
    });
  } else {
    service.context_finished();
  }
  if (voice_enabled) {
    ++alive;
    threads.emplace_back([&] {
      try {
        voice_loop(service, *ring, voice_consumer, voice_mode, control.get(), &whole, partial.get(), &whole_trace);
      } catch (const std::exception& error) {
        hm::log(hm::LogLevel::error, "perception", std::string("voice path stopped: ") + error.what());
      }
      ring->detach(voice_consumer);
      --alive;
    });
  }

  std::thread source;
  std::unique_ptr<hm::audio::AlsaCapture> capture;
  if (file_mode) {
    source = std::thread([&] {
      const auto began = std::chrono::steady_clock::now();
      const std::size_t block = 1600;
      for (std::size_t offset = 0; offset < waveform.size() && !g_stop; offset += block) {
        ring->push(waveform.data() + offset, std::min(block, waveform.size() - offset));
        if (!settings.wav_fast || voice_mode == "push_to_talk") {  // push-to-talk needs real time
          const auto due = began + std::chrono::microseconds(static_cast<std::int64_t>(
                                       static_cast<double>(offset + block) / kSr * 1e6));
          while (!g_stop && std::chrono::steady_clock::now() < due) {
            std::this_thread::sleep_until(std::min(due, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
          }
        }
      }
      ring->close();
    });
  } else {
    capture = std::make_unique<hm::audio::AlsaCapture>(settings.audio_device,
                                                       [&](const float* samples, std::size_t count) {
                                                         ring->push(samples, count);
                                                       });
    capture->start();
  }
  std::unique_ptr<hm::audio::StdinTrigger> console;
  std::unique_ptr<hm::audio::FifoTrigger> fifo;
  if (control) {
    console = std::make_unique<hm::audio::StdinTrigger>(*control);
    if (settings.ptt_fifo) {
      try {
        fifo = std::make_unique<hm::audio::FifoTrigger>(*settings.ptt_fifo, *control);
        hm::log(hm::LogLevel::info, "perception", "push-to-talk FIFO " + settings.ptt_fifo->string() +
                                                      " (write a line: press | start | stop)");
      } catch (const std::exception& error) {
        hm::log(hm::LogLevel::error, "perception", std::string("push-to-talk FIFO disabled: ") + error.what());
      }
    }
    service.echo("Enter를 누르고 말하세요 (Ctrl+C 종료)");
  } else if (voice_enabled && !file_mode) {
    service.echo("상시 듣기 모드 (스피커 소리도 인식될 수 있음). Ctrl+C 종료");
  }

  if (file_mode) {
    source.join();
    for (auto& thread : threads) thread.join();
    if (voice_mode == "whole" && voice_enabled && !g_stop) {
      service.on_utterance(whole, static_cast<double>(whole.size()) / kSr,
                           whole_trace.empty() ? std::nullopt : std::optional<std::string>(whole_trace));
    }
    if (partial) partial->finish();  // file mode: every queued chunk decoded
  } else {
    while (!g_stop && alive.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    hm::log(hm::LogLevel::info, "perception", "stopping");
    capture->stop();
    ring->close();
    for (auto& thread : threads) thread.join();
  }
  if (console) console->stop();
  if (fifo) fifo->stop();
  if (presence) presence->stop();
  if (transcriber) transcriber->close();
  RunSummary summary = service.summary();
  if (partial) {
    summary.partial_intent = partial->summary();
    partial.reset();  // microphone: drops what is still queued
  }
  if (presence) {
    const PresenceFeed::Counts counts = presence->counts();
    summary.presence = {{"accepted", counts.accepted}, {"not_json", counts.not_json}, {"rejected", counts.rejected},
                        {"oversized", counts.oversized}};
  }
  return summary;
}

int run(const hm::RuntimeConfig& config, const Hooks& hooks) {
  const Settings settings = resolve_settings(config.extra, config);
  std::unique_ptr<RemoteListen> remote;  // outlives the bus (its handler runs on the MQTT thread)
  std::unique_ptr<hm::Bus> bus;
  std::unique_ptr<hm::HeartbeatLoop> heartbeat;  // NODE-RPI5-02 online: MIC-INDOOR-01 and the cameras show online
  std::unique_ptr<Publisher> publisher;
  if (settings.publish == "stdout") {
    publisher = std::make_unique<StdoutPublisher>();
  } else {
    bus = std::make_unique<hm::Bus>(config.broker, config.perception_node_id + "-perception");
    // 7-inch UI push-to-talk (pfn/v1/ai/voice/listen): only when this node listens in push_to_talk mode.
    const bool push_to_talk = settings.wav_input.empty() ? settings.listen_mode == "push_to_talk"
                                                         : settings.wav_voice == "ptt";
    if (push_to_talk) {
      remote = std::make_unique<RemoteListen>(config.perception_node_id);
      bus->subscribe(kVoiceListenTopic, [listen = remote.get()](const std::string&, const json& document) {
        listen->handle(document, hm::now_ms());
      });
    }
    bus->subscribe("pfn/v1/system/privacy/state", [](const std::string&, const json& document) {
      if (!document.is_object() || !document.contains("microphone_allowed") ||
          !document["microphone_allowed"].is_boolean()) {
        return;
      }
      const bool allowed = document["microphone_allowed"].get<bool>();
      if (g_microphone_allowed.exchange(allowed) != allowed) {
        hm::log(hm::LogLevel::info, "perception",
                std::string("privacy: microphone ") + (allowed ? "allowed, sound level on" : "blocked, sound level off"));
      }
    });
    bus->start(10000);
    publisher = std::make_unique<BusPublisher>(*bus);
    if (settings.wav_input.empty()) {  // the live node only (file runs are tests and replays)
      heartbeat = std::make_unique<hm::HeartbeatLoop>(
          [b = bus.get()](const json& document) { b->publish(hm::kHeartbeatTopic, document, 0); },
          [b = bus.get()] { return b->connected(); }, config.perception_node_id, "pi5");
    }
  }
  const RunSummary summary = run_with(config, settings, *publisher, hooks, remote.get());
  if (bus) {
    heartbeat.reset();  // one "offline" heartbeat while still connected
    std::this_thread::sleep_for(std::chrono::seconds(1));  // let QoS1 publishes flush before disconnecting
    auto* outbox = static_cast<BusPublisher*>(publisher.get());
    for (int i = 0; i < 30 && outbox->pending() > 0 && bus->connected(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    publisher.reset();
    bus->stop();
  }
  if (!settings.wav_input.empty()) {
    json transitions = json::array();
    for (const auto& item : summary.perceptions) {
      if (item.value("transition", "") != "UNCHANGED") transitions.push_back(item);
    }
    json report = {{"windows", summary.windows}, {"transitions", transitions}, {"voice", summary.voice}};
    if (!summary.partial_intent.is_null()) report["partial_intent"] = summary.partial_intent;
    if (!summary.presence.is_null()) report["presence"] = summary.presence;
    std::fprintf(stderr, "%s\n", hm::dump_compact(report).c_str());
    std::fflush(stderr);
  }
  return 0;
}

namespace {

void usage(const Hooks& hooks, std::FILE* out) {
  std::fprintf(out,
               "usage: %s [--config PATH] [--list-devices] [--device NAME] [--listen-mode {push_to_talk,always,off}]\n"
               "       [--presence-zone {BED,DESK,BATHROOM,ENTRANCE,COMMON,OUTDOOR,UNKNOWN}] [--wav PATH ...] [--fast]\n"
               "       [--voice {auto,whole,vad,off,ptt}] [--offset S] [--seconds S] [--stdout] [--debug-transcripts]\n"
               "       [--ptt-fifo PATH] [--presence-fifo PATH] [--partial-intent] [--log-level LEVEL]%s\n",
               hooks.program.c_str(), hooks.extra_usage.c_str());
}

}  // namespace

int run_cli(int argc, char** argv, const Hooks& hooks) {
  struct sigaction action {};
  action.sa_handler = handle_stop_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  ::signal(SIGPIPE, SIG_IGN);

  std::optional<std::string> config_path;
  std::string log_level = "INFO";
  bool list_devices = false;
  json overrides = json::object();
  json wavs = json::array();
  bool partial_intent = false;
  const std::map<std::string, std::set<std::string>> choices = {
      {"--listen-mode", {"push_to_talk", "always", "off"}},
      {"--presence-zone", {kAiZones.begin(), kAiZones.end()}},
      {"--voice", {"auto", "whole", "vad", "off", "ptt"}},
  };
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    std::optional<std::string> inline_value;
    if (const auto eq = arg.find('='); arg.rfind("--", 0) == 0 && eq != std::string::npos) {
      inline_value = arg.substr(eq + 1);
      arg = arg.substr(0, eq);
    }
    auto value = [&]() -> std::string {
      if (inline_value) return *inline_value;
      if (i + 1 >= argc) throw std::invalid_argument("argument " + arg + ": expected one argument");
      return argv[++i];
    };
    auto number = [&](const std::string& text) {
      std::size_t used = 0;
      const double parsed = std::stod(text, &used);
      if (used != text.size()) throw std::invalid_argument("argument " + arg + ": invalid float value: '" + text + "'");
      return parsed;
    };
    try {
      if (arg == "-h" || arg == "--help") {
        usage(hooks, stdout);
        return 0;
      } else if (arg == "--config") {
        config_path = value();
      } else if (arg == "--log-level") {
        log_level = value();
      } else if (arg == "--list-devices") {
        list_devices = true;
      } else if (arg == "--device") {
        overrides["audio_device"] = value();
      } else if (choices.count(arg)) {
        const std::string chosen = value();
        if (!choices.at(arg).count(chosen)) throw std::invalid_argument("argument " + arg + ": invalid choice: '" + chosen + "'");
        overrides[arg == "--listen-mode" ? "listen_mode" : arg == "--voice" ? "wav_voice" : "presence_zone"] = chosen;
      } else if (arg == "--wav") {
        wavs.push_back(value());
      } else if (arg == "--fast") {
        overrides["wav_fast"] = true;
      } else if (arg == "--offset") {
        overrides["wav_offset_seconds"] = number(value());
      } else if (arg == "--seconds") {
        overrides["wav_max_seconds"] = number(value());
      } else if (arg == "--stdout") {
        overrides["publish"] = "stdout";
      } else if (arg == "--debug-transcripts") {
        overrides["debug_transcripts"] = true;
      } else if (arg == "--ptt-fifo") {
        overrides["ptt_fifo"] = value();
      } else if (arg == "--presence-fifo") {
        overrides["presence_fifo"] = value();
      } else if (arg == "--partial-intent") {
        partial_intent = true;
      } else {
        throw std::invalid_argument("unrecognized arguments: " + arg);
      }
    } catch (const std::invalid_argument& error) {
      usage(hooks, stderr);
      std::fprintf(stderr, "%s: error: %s\n", hooks.program.c_str(), error.what());
      return 2;
    }
  }
  if (!wavs.empty()) overrides["wav_input"] = wavs;
  bool enable_partial = partial_intent;
  hm::set_log_level(hm::parse_log_level(log_level));
  if (list_devices) {
    if (!hm::audio::alsa_available()) std::fprintf(stderr, "built without ALSA\n");
    int index = 0;
    for (const auto& device : hm::audio::list_capture_devices()) {
      std::printf("%c%3d  %s  [ALSA, %s]\n", device.is_default ? '*' : ' ', index++, device.name.c_str(),
                  device.description.c_str());
    }
    return 0;
  }
  try {
    hm::RuntimeConfig config = hm::RuntimeConfig::load(config_path ? std::filesystem::path(*config_path) : std::filesystem::path());
    for (const auto& [key, item] : overrides.items()) config.extra[key] = item;
    if (enable_partial) {  // --partial-intent: the configured block (or the defaults) with enabled = true
      json& block = config.extra["partial_intent"];
      if (!block.is_object()) block = json::object();
      block["enabled"] = true;
    }
    return run(config, hooks);
  } catch (const std::exception& error) {
    hm::log(hm::LogLevel::error, "perception", error.what());
    return 1;
  }
}

}  // namespace hm::perception
