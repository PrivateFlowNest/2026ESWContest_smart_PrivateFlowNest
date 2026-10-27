"""NODE-RPI5-02 perception service: one microphone, two consumers.

  context path  4 s / 1 s windows -> YAMNet + activity head -> AUDIO_CONTEXT -> context-fusion (v6
                policy) -> pfn/v1/ai/perception ACTIVITY_CONTEXT (on every transition + every 10 s)
  voice path    energy VAD endpointing (push-to-talk by default) -> temp WAV -> whisper.cpp ->
                VoiceParser -> core/task/request or ai/llm/request (temp WAV deleted immediately)

Run:  homemind-runtime perception --config cfg.json          (settings in config "extra" keys)
      python -m homemind_runtime.perception --config cfg.json --wav clip.wav --fast --stdout
      python -m homemind_runtime.perception --list-devices
"""

from __future__ import annotations

import argparse
import json
import logging
import queue
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable, Mapping, Protocol

import numpy as np

from . import pfn
from .action_map import ActionMap
from .config import REPOSITORY_ROOT, RuntimeConfig

log = logging.getLogger("homemind_runtime.perception")

SAMPLE_RATE = 16_000
AI_ZONES = ("BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN")
_AUDIO_CONTEXT = REPOSITORY_ROOT / "services" / "edge-ai" / "audio-context" / "artifacts"
_STT = REPOSITORY_ROOT / "services" / "edge-ai" / "speech-to-text"
DEFAULTS: dict[str, Any] = {
    "audio_device": None,
    "listen_mode": "push_to_talk",  # push_to_talk | always | off
    "presence_zone": "UNKNOWN",
    "presence_confidence": 0.9,
    "activity_model": str(_AUDIO_CONTEXT / "public-balanced-v6-ensemble-50-50" / "activity.joblib"),
    "yamnet_model": str(_AUDIO_CONTEXT / "yamnet-tfhub-1-savedmodel"),
    "fusion_policy": str(REPOSITORY_ROOT / "services" / "edge-ai" / "context-fusion" / "config"
                         / "audio-policy-v6.3-hold15.json"),
    "window_seconds": 4.0,
    "hop_seconds": 1.0,
    "republish_seconds": 10.0,
    "perception_ttl_seconds": 15.0,
    "vad": {},
    "ptt_timeout_seconds": 6.0,
    "whisper_cli_path": str(_STT / "models" / "whisper-bin-x64-b4938" / "Release" / "whisper-cli.exe"),
    "whisper_model": str(_STT / "artifacts" / "training" / "whisper-small-homemind-lora-v4"
                         / "ggml-whisper-small-homemind-lora-v4-q5_0.bin"),
    "whisper_threads": 8,
    "whisper_server_url": None,
    "stt_model_version": "whisper-small-homemind-lora-v4-q5_0",
    "debug_transcripts": False,
    "publish": "mqtt",  # mqtt | stdout
    "wav_input": None,
    "wav_fast": False,
    "wav_voice": "auto",  # auto | whole | vad | off
    "wav_offset_seconds": 0.0,
    "wav_max_seconds": None,
    "max_whole_file_seconds": 30.0,
}
_PATH_KEYS = ("activity_model", "yamnet_model", "fusion_policy", "whisper_cli_path", "whisper_model")


class Publisher(Protocol):
    def publish(self, topic: str, document: Mapping[str, Any], qos: int = 1) -> None: ...


class StdoutPublisher:
    def __init__(self, stream=None) -> None:
        self.stream = stream or sys.stdout
        self._lock = threading.Lock()

    def publish(self, topic: str, document: Mapping[str, Any], qos: int = 1) -> None:
        del qos
        with self._lock:
            print(f"{topic} {json.dumps(document, ensure_ascii=False, separators=(',', ':'))}",
                  file=self.stream, flush=True)


class CapturePublisher:
    """In-memory publisher for tests and offline checks."""

    def __init__(self) -> None:
        self.messages: list[tuple[str, dict[str, Any]]] = []
        self._lock = threading.Lock()

    def publish(self, topic: str, document: Mapping[str, Any], qos: int = 1) -> None:
        del qos
        with self._lock:
            self.messages.append((topic, dict(document)))


def resolve_settings(extra: Mapping[str, Any]) -> dict[str, Any]:
    unknown = set(extra) - set(DEFAULTS)
    if unknown:
        log.warning("ignoring unknown perception settings: %s", sorted(unknown))
    settings = {**DEFAULTS, **{key: value for key, value in extra.items() if key in DEFAULTS}}
    for key in _PATH_KEYS:
        path = Path(str(settings[key]))
        settings[key] = str(path if path.is_absolute() else (REPOSITORY_ROOT / path))
    zone = str(settings["presence_zone"] or "UNKNOWN").upper()
    if zone not in AI_ZONES:
        raise ValueError(f"presence_zone must be one of {AI_ZONES}, got {zone!r}")
    settings["presence_zone"] = zone
    if settings["listen_mode"] not in {"push_to_talk", "always", "off"}:
        raise ValueError("listen_mode must be push_to_talk, always or off")
    if settings["wav_voice"] not in {"auto", "whole", "vad", "off"}:
        raise ValueError("wav_voice must be auto, whole, vad or off")
    if settings["publish"] not in {"mqtt", "stdout"}:
        raise ValueError("publish must be mqtt or stdout")
    if isinstance(settings["wav_input"], str):
        settings["wav_input"] = [settings["wav_input"]]
    return settings


def _clip(value: str, limit: int = 64) -> str:
    return value if len(value) <= limit else value[:limit]


def context_event_to_perception(
    event: Mapping[str, Any],
    *,
    action_map: ActionMap,
    model_version: str,
    policy_version: str | None = None,
    ttl_ms: int = 15000,
    transition: str | None = None,
    simulated: bool = False,
) -> dict[str, Any]:
    """Fused ACTIVITY_CONTEXT (rpi5-internal) -> schema-valid pfn.v1 perception document."""

    evidence = [f"AUDIO_CONTEXT:{event['context_type']}", f"PRESENCE:{event['presence_zone']}"]
    if policy_version:
        evidence.append(f"policy:{policy_version}")
    evidence.extend(str(item) for item in event.get("evidence", []) if str(item))
    evidence = list(dict.fromkeys(_clip(item) for item in evidence))[:16]
    return pfn.perception(
        observation_type="ACTIVITY_CONTEXT",
        zone=action_map.zone(str(event["presence_zone"])),
        confidence=float(event["confidence"]),
        attributes={
            "activity": event["activity"],
            "audio_context": event["context_type"],
            "quiet_required": bool(event["quiet_required"]),
            "transition": transition or event["transition"],
            "evidence": evidence,
            "valid_until_ms": pfn.now_ms() + int(ttl_ms),
            "model_version": _clip(model_version),
        },
        simulated=simulated,
    )


def sanitized_context(event: Mapping[str, Any]) -> dict[str, Any]:
    """Same projection as ContextFusionEngine.sanitized_context (activity-context schema)."""

    return {
        "presence_zone": event["presence_zone"],
        "activity": event["activity"],
        "audio_context": event["context_type"],
        "quiet_required": bool(event["quiet_required"]),
        "confidence": float(event["confidence"]),
    }


@dataclass
class RunSummary:
    windows: int = 0
    perceptions: list[dict[str, Any]] = field(default_factory=list)
    voice: list[dict[str, Any]] = field(default_factory=list)


class PerceptionService:
    """State shared by both consumers; publishes through any Publisher (MQTT Bus, stdout, capture)."""

    def __init__(
        self,
        settings: Mapping[str, Any],
        *,
        publisher: Publisher,
        action_map: ActionMap,
        context_pipeline: Any | None,
        transcriber: Any | None,
        parser: Any | None,
        simulated: bool,
        echo: Callable[[str], None] | None = None,
    ) -> None:
        self.settings = dict(settings)
        self.publisher = publisher
        self.action_map = action_map
        self.context_pipeline = context_pipeline
        self.transcriber = transcriber
        self.parser = parser
        self.simulated = simulated
        self.echo = echo or (lambda line: print(line, flush=True))
        self.base_time = datetime.now(timezone.utc)
        self.summary = RunSummary()
        self._lock = threading.Lock()
        self._latest: dict[str, Any] | None = None
        self._latest_audio_time = 0.0
        self._last_publish_audio_time: float | None = None
        self.whole_input = np.zeros(0, dtype=np.float32)

    # ---- context path -------------------------------------------------------------------------
    def timestamp(self, audio_seconds: float) -> datetime:
        return self.base_time + timedelta(seconds=audio_seconds)

    def process_window(self, window: np.ndarray, end_sample: int) -> None:
        audio_time = end_sample / SAMPLE_RATE
        update = self.context_pipeline.process(window, timestamp=self.timestamp(audio_time),
                                               elapsed_seconds=audio_time)
        self.summary.windows += 1
        log.debug("window t=%.1fs raw=%s(%.2f) speech=%.2f fused=%s/%s %s %.0fms", audio_time,
                  update.raw_label, update.raw_confidence, update.speech_ratio,
                  update.context_event["context_type"], update.context_event["quiet_required"],
                  update.context_event["transition"], update.latency_ms)
        self.on_context_event(update.context_event, audio_time)

    def on_context_event(self, event: Mapping[str, Any], audio_time: float) -> dict[str, Any] | None:
        transition = str(event["transition"])
        with self._lock:
            self._latest = dict(event)
            self._latest_audio_time = audio_time
            last = self._last_publish_audio_time
            due = (transition != "UNCHANGED" or last is None
                   or audio_time - last >= float(self.settings["republish_seconds"]))
            if due:
                self._last_publish_audio_time = audio_time
        if not due:
            return None
        pipeline = self.context_pipeline
        document = context_event_to_perception(
            event,
            action_map=self.action_map,
            model_version=getattr(pipeline, "model_version", None) or str(event.get("model_version")),
            policy_version=getattr(pipeline, "policy_version", None),
            ttl_ms=int(float(self.settings["perception_ttl_seconds"]) * 1000),
            simulated=self.simulated,
        )
        self.publisher.publish(pfn.TOPICS["perception"], document)
        attributes = document["attributes"]
        record = {"t": round(audio_time, 1), "activity": attributes["activity"],
                  "audio_context": attributes["audio_context"], "quiet_required": attributes["quiet_required"],
                  "transition": attributes["transition"], "confidence": document["confidence"],
                  "zone": document["zone"]}
        self.summary.perceptions.append(record)
        log.info("perception t=%.1fs %s/%s quiet=%s %s conf=%.2f zone=%s", audio_time, record["activity"],
                 record["audio_context"], record["quiet_required"], record["transition"],
                 record["confidence"], record["zone"])
        return document

    def latest_context(self, audio_time: float | None = None) -> dict[str, Any] | None:
        with self._lock:
            if self._latest is None:
                return None
            if audio_time is not None and audio_time - self._latest_audio_time > float(
                    self.settings["perception_ttl_seconds"]):
                return None
            return sanitized_context(self._latest)

    # ---- voice path ---------------------------------------------------------------------------
    def on_utterance(self, samples: np.ndarray, audio_time: float | None = None) -> dict[str, Any] | None:
        if self.transcriber is None or self.parser is None:
            return None
        seconds = samples.size / SAMPLE_RATE
        try:
            result = self.transcriber.transcribe(samples)
        except Exception:  # noqa: BLE001 - one failed STT call must not stop the node
            log.exception("whisper failed for a %.1fs utterance", seconds)
            return None
        if self.settings.get("debug_transcripts"):
            log.info("transcript (debug_transcripts) %r", result.text)
        record: dict[str, Any] = {"utterance_seconds": round(seconds, 2), "stt_ms": round(result.latency_ms),
                                  "chars": len(result.text)}
        if not result.text:
            record["decision"] = "empty"
            self.summary.voice.append(record)
            self.echo(f"[voice] {seconds:.1f}s, STT {result.latency_ms:.0f} ms → 인식된 문장 없음")
            return record
        context = self.latest_context(audio_time)
        try:
            decision = self.parser.handle(result.text, context=context, stt_model_version=result.model_version,
                                          stt_confidence=None, utterance_id=str(uuid.uuid4()))
        except ValueError:
            log.exception("voice parser produced an invalid message")
            return None
        for topic, message in decision.messages:
            self.publisher.publish(topic, message)
        proposal = decision.proposal or {}
        record.update({"decision": decision.kind, "device": proposal.get("device"),
                       "action": proposal.get("action"), "topics": [topic for topic, _ in decision.messages],
                       "context": (context or {}).get("audio_context")})
        self.summary.voice.append(record)
        log.info("voice %.1fs stt=%.0fms chars=%d decision=%s %s/%s", seconds, result.latency_ms,
                 len(result.text), decision.kind, proposal.get("device"), proposal.get("action"))
        shown = f" \"{result.text}\"" if self.settings.get("debug_transcripts") else ""
        self.echo(f"[voice] {seconds:.1f}s, STT {result.latency_ms:.0f} ms{shown} → {decision.kind} "
                  f"{proposal.get('device') or ''} {proposal.get('action') or ''} {decision.note}".rstrip())
        return record


# ---- consumers --------------------------------------------------------------------------------
def context_loop(service: PerceptionService, subscriber, *, lossy: bool) -> None:
    window = int(round(float(service.settings["window_seconds"]) * SAMPLE_RATE))
    hop = int(round(float(service.settings["hop_seconds"]) * SAMPLE_RATE))
    buffer = np.empty(0, dtype=np.float32)
    start = 0  # sample index of buffer[0]
    processed = 0
    while True:
        block = subscriber.get()
        if block is None:
            break
        if buffer.size == 0:
            start = block.start_sample
        buffer = np.concatenate([buffer, block.samples])
        if lossy and buffer.size > window + 3 * hop:
            skip = ((buffer.size - window) // hop) * hop
            log.warning("context path behind by %.1fs; skipping ahead", skip / SAMPLE_RATE)
            buffer, start = buffer[skip:], start + skip
        while buffer.size >= window:
            try:
                service.process_window(buffer[:window], start + window)
            except Exception:  # noqa: BLE001 - one bad window must not stop the node
                log.exception("context window failed")
            processed += 1
            buffer, start = buffer[hop:], start + hop
    if processed == 0 and buffer.size:
        padded = np.pad(buffer, (0, window - buffer.size))
        service.process_window(padded, start + buffer.size)


class VoiceControl:
    """Push-to-talk commands from the console thread to the voice consumer thread."""

    def __init__(self) -> None:
        self.commands: queue.Queue[str] = queue.Queue()

    def press(self) -> None:
        self.commands.put("enter")


def voice_loop(service: PerceptionService, subscriber, *, mode: str, control: VoiceControl | None = None) -> None:
    """mode: push_to_talk | always | whole (one utterance = whole input, file mode).

    push_to_talk: Enter opens an utterance (with 300 ms pre-roll); it ends ~700 ms after speech stops,
    at max_ms, or on a second Enter. No speech onset within ptt_timeout_seconds -> cancelled.
    """

    from .audio.vad import EndpointConfig, EnergyEndpointer

    if mode == "whole":
        parts = []
        while (block := subscriber.get()) is not None:
            parts.append(block.samples)
        service.whole_input = np.concatenate(parts) if parts else np.zeros(0, dtype=np.float32)
        return
    endpointer = EnergyEndpointer(EndpointConfig.from_mapping(service.settings.get("vad")),
                                  armed=(mode == "always"))
    timeout = float(service.settings["ptt_timeout_seconds"])
    deadline: float | None = None
    audio_time = 0.0
    while True:
        block = subscriber.get()
        if block is None:
            last = endpointer.stop("flush")
            if last is not None:
                service.on_utterance(last.samples, audio_time)
            break
        audio_time = (block.start_sample + block.samples.size) / SAMPLE_RATE
        while control is not None:
            try:
                control.commands.get_nowait()
            except queue.Empty:
                break
            if not endpointer.in_speech:
                endpointer.begin()
                deadline = audio_time + timeout
                service.echo("듣는 중... (말이 끝나면 자동 종료, Enter = 바로 종료)")
            else:
                deadline = None
                manual = endpointer.stop("manual")
                if manual is not None:
                    service.on_utterance(manual.samples, audio_time)
                else:
                    service.echo("음성이 너무 짧습니다. Enter를 누르고 다시 말하세요")
        utterances = endpointer.feed(block.samples)
        for utterance in utterances:
            deadline = None
            service.on_utterance(utterance.samples, audio_time)
            if mode == "push_to_talk":
                service.echo("Enter를 누르고 말하세요")
        timed_out = (deadline is not None and endpointer.in_speech and not endpointer.onset_seen
                     and audio_time > deadline)
        if timed_out or (deadline is not None and not endpointer.in_speech and not utterances):
            # no onset before the timeout, or the utterance hit max_ms / was too short to keep
            deadline = None
            endpointer.reset()
            service.echo("음성이 감지되지 않았습니다. Enter를 누르고 다시 말하세요")


def _console_reader(control: VoiceControl) -> None:
    for _ in sys.stdin:
        control.press()


# ---- wiring -----------------------------------------------------------------------------------
def build_service(config: RuntimeConfig, settings: Mapping[str, Any], *, publisher: Publisher, file_mode: bool,
                  voice_enabled: bool) -> PerceptionService:
    from .audio.context import AudioContextPipeline

    simulated = True if file_mode else bool(config.simulated)
    action_map = ActionMap(config.contracts_dir)
    pipeline = AudioContextPipeline(
        activity_model=settings["activity_model"], yamnet_model=settings["yamnet_model"],
        policy_path=settings["fusion_policy"], presence_zone=settings["presence_zone"],
        presence_confidence=float(settings["presence_confidence"]), simulated=simulated,
    )
    transcriber = parser = None
    if voice_enabled:
        from .audio.stt import WhisperTranscriber
        from .voice import VoiceParser

        try:
            transcriber = WhisperTranscriber(settings)
            parser = VoiceParser(action_map, node_id=config.perception_node_id,
                                 llm_ttl_ms=config.llm_request_ttl_ms, simulated=simulated)
        except FileNotFoundError as error:
            log.error("voice path disabled: %s", error)
            transcriber = parser = None
    return PerceptionService(settings, publisher=publisher, action_map=action_map, context_pipeline=pipeline,
                             transcriber=transcriber, parser=parser, simulated=simulated)


def run_with(config: RuntimeConfig, settings: Mapping[str, Any], *, publisher: Publisher,
             service: PerceptionService | None = None) -> PerceptionService:
    """Run file mode to completion, or microphone mode until Ctrl+C. Returns the service (summary)."""

    from .audio.capture import AudioFanout, MicrophoneSource, WavFileSource

    file_mode = bool(settings.get("wav_input"))
    if file_mode:
        fanout = AudioFanout(lossy=False)
        source = WavFileSource(fanout, settings["wav_input"], realtime=not settings["wav_fast"],
                               offset_seconds=float(settings["wav_offset_seconds"] or 0.0),
                               max_seconds=settings["wav_max_seconds"])
        voice_mode = settings["wav_voice"]
        if voice_mode == "auto":
            voice_mode = "whole" if source.total_samples / SAMPLE_RATE <= float(
                settings["max_whole_file_seconds"]) else "off"
            if voice_mode == "off":
                log.info("file longer than %.0fs: voice path off (use --voice vad)", settings["max_whole_file_seconds"])
        elif voice_mode == "vad":
            voice_mode = "always"
    else:
        fanout = AudioFanout(lossy=True)
        source = MicrophoneSource(fanout, settings["audio_device"])
        voice_mode = settings["listen_mode"]
    voice_enabled = voice_mode != "off"
    if service is None:
        service = build_service(config, settings, publisher=publisher, file_mode=file_mode,
                                voice_enabled=voice_enabled)
    if service.context_pipeline is not None and hasattr(service.context_pipeline, "warmup"):
        service.context_pipeline.warmup()
    if service.transcriber is None:
        voice_enabled = False
    context_sub = fanout.subscribe("context")
    threads = [threading.Thread(target=context_loop, args=(service, context_sub), kwargs={"lossy": fanout.lossy},
                                name="context-path", daemon=True)]
    control = VoiceControl() if voice_mode == "push_to_talk" else None
    if voice_enabled:
        voice_sub = fanout.subscribe("voice")
        threads.append(threading.Thread(target=voice_loop, args=(service, voice_sub),
                                        kwargs={"mode": voice_mode, "control": control},
                                        name="voice-path", daemon=True))
    service.base_time = datetime.now(timezone.utc)
    for thread in threads:
        thread.start()
    source.start()
    if control is not None:
        threading.Thread(target=_console_reader, args=(control,), name="ptt-console", daemon=True).start()
        service.echo("Enter를 누르고 말하세요 (Ctrl+C 종료)")
    elif voice_enabled and not file_mode:
        service.echo("상시 듣기 모드 (스피커 소리도 인식될 수 있음). Ctrl+C 종료")
    try:
        if file_mode:
            source.join()
            for thread in threads:
                thread.join()
            if voice_mode == "whole" and voice_enabled:
                samples = service.whole_input
                service.on_utterance(samples, samples.size / SAMPLE_RATE)
        else:
            while any(thread.is_alive() for thread in threads):
                time.sleep(0.5)
    except KeyboardInterrupt:
        log.info("stopping")
    finally:
        source.stop()
        if service.transcriber is not None and hasattr(service.transcriber, "close"):
            service.transcriber.close()
    return service


def run(config: RuntimeConfig) -> int:
    settings = resolve_settings(config.extra)
    if settings["publish"] == "stdout":
        publisher: Any = StdoutPublisher()
        bus = None
    else:
        from .mqtt import Bus

        bus = Bus(config.broker, f"{config.perception_node_id}-perception")
        bus.start()
        publisher = bus
    try:
        service = run_with(config, settings, publisher=publisher)
    finally:
        if bus is not None:
            time.sleep(1.0)  # let QoS1 publishes flush before disconnecting
            bus.stop()
    if settings.get("wav_input"):
        transitions = [item for item in service.summary.perceptions if item["transition"] != "UNCHANGED"]
        print(json.dumps({"windows": service.summary.windows, "transitions": transitions,
                          "voice": service.summary.voice}, ensure_ascii=False), file=sys.stderr, flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m homemind_runtime.perception")
    parser.add_argument("--config", default=None)
    parser.add_argument("--list-devices", action="store_true", help="print input devices and exit")
    parser.add_argument("--device", help="input device index or name substring")
    parser.add_argument("--listen-mode", choices=["push_to_talk", "always", "off"])
    parser.add_argument("--presence-zone", choices=list(AI_ZONES))
    parser.add_argument("--wav", action="append", help="file mode input (repeat to concatenate)")
    parser.add_argument("--fast", action="store_true", help="file mode as fast as possible")
    parser.add_argument("--voice", choices=["auto", "whole", "vad", "off"], help="file mode voice path")
    parser.add_argument("--offset", type=float, help="file mode start offset seconds")
    parser.add_argument("--seconds", type=float, help="file mode max seconds")
    parser.add_argument("--stdout", action="store_true", help="print messages instead of MQTT")
    parser.add_argument("--debug-transcripts", action="store_true", help="rehearsal only: show transcripts")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args(argv)
    logging.basicConfig(level=args.log_level, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    if args.list_devices:
        from .audio.capture import list_input_devices

        for row in list_input_devices():
            mark = "*" if row["default"] else " "
            print(f"{mark}{row['index']:3d}  {row['name']}  [{row['hostapi']}, {row['default_samplerate']:.0f} Hz]")
        return 0
    config = RuntimeConfig.load(args.config)
    overrides = {
        "audio_device": args.device, "listen_mode": args.listen_mode, "presence_zone": args.presence_zone,
        "wav_input": args.wav, "wav_fast": True if args.fast else None, "wav_voice": args.voice,
        "wav_offset_seconds": args.offset, "wav_max_seconds": args.seconds,
        "publish": "stdout" if args.stdout else None, "debug_transcripts": True if args.debug_transcripts else None,
    }
    config.extra.update({key: value for key, value in overrides.items() if value is not None})
    return run(config)


if __name__ == "__main__":
    raise SystemExit(main())
