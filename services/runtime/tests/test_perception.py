"""NODE-RPI5-02 perception service tests (no microphone, no MQTT broker)."""

from __future__ import annotations

import wave
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
import pytest

from homemind_runtime import pfn
from homemind_runtime.action_map import ActionMap
from homemind_runtime.audio.stt import SttResult, WhisperTranscriber, clean_transcript, write_pcm16_wav
from homemind_runtime.audio.vad import EndpointConfig, EnergyEndpointer
from homemind_runtime.config import REPOSITORY_ROOT, RuntimeConfig
from homemind_runtime.perception import (
    DEFAULTS,
    CapturePublisher,
    PerceptionService,
    context_event_to_perception,
    resolve_settings,
    run_with,
)
from homemind_runtime.voice import VoiceParser

SR = 16_000


def tone(seconds: float, amplitude: float = 0.3, freq: float = 220.0) -> np.ndarray:
    t = np.arange(int(seconds * SR)) / SR
    return (amplitude * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def silence(seconds: float, amplitude: float = 0.0005, seed: int = 0) -> np.ndarray:
    return (amplitude * np.random.default_rng(seed).standard_normal(int(seconds * SR))).astype(np.float32)


def feed_blocks(endpointer: EnergyEndpointer, audio: np.ndarray, block: int = 1600):
    found = []
    for offset in range(0, audio.size, block):
        found.extend(endpointer.feed(audio[offset : offset + block]))
    return found


# ---- VAD endpointing ----------------------------------------------------------------------------
def test_vad_finds_one_utterance_after_silence():
    audio = np.concatenate([silence(1.0), tone(1.2), silence(1.5)])
    utterances = feed_blocks(EnergyEndpointer(EndpointConfig()), audio)
    assert len(utterances) == 1
    utt = utterances[0]
    assert utt.reason == "silence"
    # speech 1.0..2.2 s, pre-roll 300 ms, tail 200 ms
    assert 0.6 * SR <= utt.start_sample <= 1.0 * SR
    assert 1.2 <= utt.seconds <= 1.9


def test_vad_ignores_short_click_and_below_min():
    audio = np.concatenate([silence(1.0), tone(0.1), silence(1.0), tone(0.3), silence(1.2)])
    assert feed_blocks(EnergyEndpointer(EndpointConfig()), audio) == []


def test_vad_splits_two_utterances_and_caps_max_length():
    audio = np.concatenate([silence(0.5), tone(0.8), silence(1.0), tone(0.8), silence(1.0)])
    assert len(feed_blocks(EnergyEndpointer(EndpointConfig()), audio)) == 2
    long = np.concatenate([silence(0.5), tone(10.0)])
    capped = feed_blocks(EnergyEndpointer(EndpointConfig(max_ms=3000)), long)
    assert capped[0].reason == "max_length" and capped[0].seconds == pytest.approx(3.0, abs=0.05)


def test_vad_short_pause_does_not_end_utterance():
    audio = np.concatenate([silence(0.5), tone(0.6), silence(0.4), tone(0.6), silence(1.0)])
    utterances = feed_blocks(EnergyEndpointer(EndpointConfig()), audio)
    assert len(utterances) == 1 and utterances[0].seconds > 1.5


def test_vad_push_to_talk_waits_for_begin():
    endpointer = EnergyEndpointer(EndpointConfig(), armed=False)
    assert feed_blocks(endpointer, np.concatenate([silence(0.5), tone(1.0), silence(1.0)])) == []
    endpointer.begin()
    # 1 s of silence after the key press must not end the utterance before speech starts
    found = feed_blocks(endpointer, np.concatenate([silence(1.0), tone(0.8), silence(1.0)]))
    assert len(found) == 1 and found[0].reason == "silence"
    endpointer.begin()
    feed_blocks(endpointer, tone(0.6))
    manual = endpointer.stop("manual")
    assert manual is not None and manual.reason == "manual"


# ---- fusion -> perception -----------------------------------------------------------------------
def fused_event(**overrides):
    event = {
        "schema_version": "1.0", "event_id": "ctx-1", "trace_id": "trace-1", "timestamp": "2026-10-22T00:00:00.000Z",
        "source": "pi5.context-fusion", "source_node_id": "NODE-RPI5-02", "event_type": "ACTIVITY_CONTEXT",
        "context_type": "ZOOM_SPEAKER", "presence_zone": "DESK", "activity": "MEETING", "quiet_required": True,
        "confidence": 0.87,
        "evidence": ["audio:ZOOM_SPEAKER:0.91", "audio_model:" + "x" * 80, "margin:0.300", "margin:0.300"],
        "transition": "ACTIVATED", "valid_until": "2026-10-22T00:00:10.000Z", "model_version": "context-fusion-rules-v1",
    }
    event.update(overrides)
    return event


def test_fusion_event_to_perception_is_schema_valid():
    doc = context_event_to_perception(fused_event(), action_map=ActionMap(),
                                      model_version="yamnet-temporal-cost-ensemble-v0.8.0-50-50",
                                      policy_version="audio-policy-v6.2.1-ensemble-50-50", simulated=True)
    pfn.validate(doc, "perception.schema.json")
    attrs = doc["attributes"]
    assert doc["zone"] == "desk" and doc["source_node_id"] == "NODE-RPI5-02" and doc["simulated"] is True
    assert attrs["activity"] == "MEETING" and attrs["audio_context"] == "ZOOM_SPEAKER"
    assert attrs["quiet_required"] is True and attrs["transition"] == "ACTIVATED"
    assert all(1 <= len(item) <= 64 for item in attrs["evidence"]) and len(set(attrs["evidence"])) == len(attrs["evidence"])
    assert attrs["evidence"][:2] == ["AUDIO_CONTEXT:ZOOM_SPEAKER", "PRESENCE:DESK"]
    assert attrs["valid_until_ms"] > pfn.now_ms()


def test_unknown_presence_maps_to_unknown_zone_and_heartbeat_override():
    doc = context_event_to_perception(fused_event(presence_zone="UNKNOWN", evidence=[]), action_map=ActionMap(),
                                      model_version="m", transition="UNCHANGED")
    pfn.validate(doc, "perception.schema.json")
    assert doc["zone"] == "unknown" and doc["attributes"]["transition"] == "UNCHANGED"
    assert "simulated" not in doc


def make_service(publisher, **settings):
    merged = {**resolve_settings({}), **settings}
    return PerceptionService(merged, publisher=publisher, action_map=ActionMap(), context_pipeline=None,
                             transcriber=None, parser=None, simulated=True, echo=lambda line: None)


def test_service_publishes_transitions_and_republishes_every_10s():
    publisher = CapturePublisher()
    service = make_service(publisher)
    timeline = [(1, "UNCHANGED"), (2, "UNCHANGED"), (3, "ACTIVATED"), (4, "UNCHANGED"), (12, "UNCHANGED"),
                (13, "UNCHANGED"), (14, "DEACTIVATED"), (15, "UNCHANGED"), (24, "UNCHANGED")]
    for t, transition in timeline:
        quiet = transition == "ACTIVATED" or (3 <= t < 14)
        service.on_context_event(fused_event(transition=transition, quiet_required=quiet), float(t))
    transitions = [doc["attributes"]["transition"] for _, doc in publisher.messages]
    assert transitions == ["UNCHANGED", "ACTIVATED", "UNCHANGED", "DEACTIVATED", "UNCHANGED"]
    assert [record["t"] for record in service.summary.perceptions] == [1.0, 3.0, 13.0, 14.0, 24.0]
    assert all(topic == "pfn/v1/ai/perception" for topic, _ in publisher.messages)


# ---- voice parser messages ----------------------------------------------------------------------
@pytest.fixture(scope="module")
def parser():
    return VoiceParser(ActionMap(), simulated=True)


def test_voice_light_on(parser):
    decision = parser.handle("불 켜줘", context=None, stt_model_version="whisper-test", stt_confidence=None)
    assert decision.kind == "task_request"
    topic, request = decision.messages[0]
    assert topic == "pfn/v1/core/task/request"
    assert request["intent"] == "device_control" and request["target_device"] == "LED-LIVING-01"
    params = request["parameters"]
    assert params["action"] == "set_switch" and params["parameters"] == {"on": True}
    assert params["origin"] == "voice_parser"


def test_voice_open_door_is_caution(parser):
    decision = parser.handle("현관문 열어줘", stt_model_version="whisper-test")
    assert decision.kind == "task_request"
    request = decision.messages[0][1]
    assert request["parameters"]["action"] == "set_position"
    assert request["parameters"]["parameters"]["position_degrees"] == 90
    assert request["safety_level"] == "caution"


def test_voice_vague_goes_to_llm_with_context(parser):
    assert parser.handle("아 좀 덥네", context=None, stt_model_version="whisper-test").kind == "llm_request"
    context = {"presence_zone": "UNKNOWN", "activity": "UNKNOWN", "audio_context": "SELF_TALK",
               "quiet_required": False, "confidence": 0.8}
    decision = parser.handle("아 좀 덥네", context=context, stt_model_version="whisper-test", stt_confidence=None)
    assert decision.kind == "llm_request"
    topic, request = decision.messages[0]
    assert topic == "pfn/v1/ai/llm/request"
    assert request["context"] == context and request["transcript"]["stt_confidence"] is None
    pfn.validate(request, "llm-request.schema.json")


def test_voice_during_meeting(parser):
    # quiet context: only quiet requests become quiet mode; a hidden intent still goes to EXAONE.
    context = {"presence_zone": "UNKNOWN", "activity": "MEETING", "audio_context": "ZOOM_SPEAKER",
               "quiet_required": True, "confidence": 0.9}
    assert parser.handle("아 좀 덥네", context=context, stt_model_version="whisper-test").kind == "llm_request"
    quiet = parser.handle("조용히 해줘", context=context, stt_model_version="whisper-test")
    assert quiet.kind == "task_request" and quiet.messages[0][1]["intent"] == "quiet_mode_on"
    assert parser.handle("불 켜줘", context=context).messages[0][1]["target_device"] == "LED-LIVING-01"


# ---- STT helpers --------------------------------------------------------------------------------
def test_clean_transcript_drops_markers_and_hallucinations():
    assert clean_transcript("[BLANK_AUDIO]") == ""
    assert clean_transcript(" 감사합니다.") == ""
    assert clean_transcript("- 불 켜줘\n") == "불 켜줘"
    assert clean_transcript("(박수) 에어컨 켜") == "에어컨 켜"


class FakeBackend:
    model_version = "fake-whisper"

    def __init__(self, text: str) -> None:
        self.text = text
        self.seen: list[Path] = []

    def transcribe(self, path):
        path = Path(path)
        with wave.open(str(path), "rb") as handle:
            assert handle.getframerate() == SR and handle.getsampwidth() == 2 and handle.getnchannels() == 1
        self.seen.append(path)
        return SttResult(self.text, len(self.text), 0.0, self.model_version)


def test_transcriber_deletes_temp_wav():
    backend = FakeBackend("불 켜줘")
    transcriber = WhisperTranscriber({}, backend=backend)
    result = transcriber.transcribe(tone(1.0))
    assert result.text == "불 켜줘" and result.model_version == "fake-whisper"
    assert backend.seen and not backend.seen[0].exists() and not backend.seen[0].parent.exists()


# ---- file mode (fake context model + fake whisper, real fusion + parser) -------------------------
class FakePipeline:
    """Replays a scripted raw label per window through the real FeatureStreamFusion (default policy, v6.3)."""

    model_version = "fake-audio-head"
    policy_version = "audio-policy-v6.3.0-ensemble-50-50-hold15"

    def __init__(self) -> None:
        from homemind_audio.models import Prediction
        from homemind_context_fusion.policy_config import ContextFusionPolicy
        from homemind_context_fusion.stream_fusion import FeatureStreamFusion

        self.Prediction = Prediction
        self.fusion = FeatureStreamFusion(engine=ContextFusionPolicy.from_json(DEFAULTS["fusion_policy"]).engine())

    def warmup(self):
        pass

    def process(self, window, *, timestamp, elapsed_seconds):
        from homemind_audio.models import prediction_to_feature_event
        from homemind_runtime.audio.context import ContextUpdate

        loud = float(np.sqrt(np.mean(window ** 2))) > 0.05
        label = "ZOOM_SPEAKER" if loud else "UNKNOWN"
        prediction = self.Prediction("activity", label, 0.9 if loud else 0.6, self.model_version)
        speech_ratio = 0.7 if loud else 0.0
        event = prediction_to_feature_event(prediction, trace_id="trace-test", speech_ratio=speech_ratio,
                                            duration_seconds=elapsed_seconds, now=timestamp, simulated=True)
        result = self.fusion.ingest(event)
        return ContextUpdate(label, prediction.confidence, speech_ratio, event, result.context_event, 1.0)


def test_file_mode_runs_both_paths(tmp_path):
    audio = np.concatenate([silence(5.0), tone(14.0, 0.3), silence(22.0)])  # v6.3 releases after 15 updates
    wav = tmp_path / "scene.wav"
    write_pcm16_wav(wav, audio)
    publisher = CapturePublisher()
    settings = resolve_settings({"wav_input": str(wav), "wav_fast": True, "wav_voice": "whole",
                                 "max_whole_file_seconds": 60})
    backend = FakeBackend("불 켜줘")
    service = PerceptionService(settings, publisher=publisher, action_map=ActionMap(), context_pipeline=FakePipeline(),
                                transcriber=WhisperTranscriber({}, backend=backend),
                                parser=VoiceParser(ActionMap(), simulated=True), simulated=True,
                                echo=lambda line: None)
    run_with(RuntimeConfig(), settings, publisher=publisher, service=service)
    perceptions = [doc for topic, doc in publisher.messages if topic == pfn.TOPICS["perception"]]
    for doc in perceptions:
        pfn.validate(doc, "perception.schema.json")
    transitions = [doc["attributes"]["transition"] for doc in perceptions]
    assert "ACTIVATED" in transitions and "DEACTIVATED" in transitions
    activated = next(doc for doc in perceptions if doc["attributes"]["transition"] == "ACTIVATED")
    assert activated["attributes"]["quiet_required"] is True and activated["attributes"]["activity"] == "MEETING"
    assert service.summary.windows == 38  # 41 s, 4 s window, 1 s hop
    tasks = [doc for topic, doc in publisher.messages if topic == pfn.TOPICS["task_request"]]
    assert len(tasks) == 1 and tasks[0]["target_device"] == "LED-LIVING-01"
    assert service.summary.voice[0]["decision"] == "task_request"
    assert len(backend.seen) == 1 and not backend.seen[0].exists()


def test_file_mode_vad_voice_path(tmp_path):
    audio = np.concatenate([silence(1.0), tone(1.0), silence(1.5), tone(1.0), silence(1.5)])
    wav = tmp_path / "two.wav"
    write_pcm16_wav(wav, audio)
    publisher = CapturePublisher()
    settings = resolve_settings({"wav_input": str(wav), "wav_fast": True, "wav_voice": "vad"})
    service = PerceptionService(settings, publisher=publisher, action_map=ActionMap(), context_pipeline=FakePipeline(),
                                transcriber=WhisperTranscriber({}, backend=FakeBackend("현관문 열어줘")),
                                parser=VoiceParser(ActionMap(), simulated=True), simulated=True,
                                echo=lambda line: None)
    run_with(RuntimeConfig(), settings, publisher=publisher, service=service)
    tasks = [doc for topic, doc in publisher.messages if topic == pfn.TOPICS["task_request"]]
    assert len(tasks) == 2 and all(doc["safety_level"] == "caution" for doc in tasks)


def test_example_config_has_no_secrets_and_loads():
    path = REPOSITORY_ROOT / "services" / "runtime" / "config" / "runtime.pc.example.json"
    config = RuntimeConfig.load(path)
    assert config.broker.password_file is None and config.broker.host == "127.0.0.1"
    settings = resolve_settings(config.extra)
    assert settings["listen_mode"] == "push_to_talk" and settings["presence_zone"] == "UNKNOWN"
    assert "password" not in path.read_text(encoding="utf-8").replace("password_file", "")


# ---- real models (skipped when artifacts are missing) -------------------------------------------
REAL = all(Path(DEFAULTS[key]).exists() for key in ("activity_model", "yamnet_model", "fusion_policy"))


@pytest.mark.skipif(not REAL, reason="audio-context artifacts not on disk")
def test_real_context_pipeline_on_synthetic_silence():
    from homemind_runtime.audio.context import AudioContextPipeline

    pipeline = AudioContextPipeline(activity_model=DEFAULTS["activity_model"], yamnet_model=DEFAULTS["yamnet_model"],
                                    policy_path=DEFAULTS["fusion_policy"], simulated=True)
    base = datetime.now(timezone.utc)
    events = [pipeline.process(silence(4.0, seed=i), timestamp=base + timedelta(seconds=i), elapsed_seconds=4.0 + i)
              for i in range(4)]
    last = events[-1].context_event
    assert last["quiet_required"] is False and last["context_type"] in {"QUIET", "UNKNOWN"}
    doc = context_event_to_perception(last, action_map=ActionMap(), model_version=pipeline.model_version,
                                      policy_version=pipeline.policy_version, simulated=True)
    pfn.validate(doc, "perception.schema.json")


def test_push_to_talk_loop_records_only_after_enter():
    import threading

    from homemind_runtime.audio.capture import AudioFanout
    from homemind_runtime.perception import VoiceControl, voice_loop

    publisher = CapturePublisher()
    backend = FakeBackend("불 켜줘")
    lines: list[str] = []
    service = PerceptionService(resolve_settings({}), publisher=publisher, action_map=ActionMap(),
                                context_pipeline=None, transcriber=WhisperTranscriber({}, backend=backend),
                                parser=VoiceParser(ActionMap(), simulated=True), simulated=True, echo=lines.append)
    fanout = AudioFanout(lossy=False)
    subscriber = fanout.subscribe("voice")
    control = VoiceControl()
    thread = threading.Thread(target=voice_loop, args=(service, subscriber),
                              kwargs={"mode": "push_to_talk", "control": control})
    thread.start()
    speaker_audio = np.concatenate([silence(0.5), tone(1.5), silence(1.0)])  # no Enter -> ignored
    for offset in range(0, speaker_audio.size, 1600):
        fanout.push(speaker_audio[offset : offset + 1600])
    fanout.close()
    thread.join(5)
    assert backend.seen == []
    fanout = AudioFanout(lossy=False)
    subscriber = fanout.subscribe("voice")
    thread = threading.Thread(target=voice_loop, args=(service, subscriber),
                              kwargs={"mode": "push_to_talk", "control": control})
    thread.start()
    control.press()
    command = np.concatenate([silence(0.8), tone(1.0), silence(1.2)])
    for offset in range(0, command.size, 1600):
        fanout.push(command[offset : offset + 1600])
    fanout.close()
    thread.join(5)
    assert len(backend.seen) == 1 and publisher.messages[0][1]["target_device"] == "LED-LIVING-01"
    assert lines[0].startswith("듣는 중")
