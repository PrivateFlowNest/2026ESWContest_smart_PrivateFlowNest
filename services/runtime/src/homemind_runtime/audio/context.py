"""Context path: 4 s window -> YAMNet + VAD features -> activity head -> AUDIO_CONTEXT -> fusion.

Reuses audio-context (YamnetEmbeddingExtractor, the joblib head loader, prediction_to_feature_event)
and context-fusion (FeatureStreamFusion + versioned policy) exactly as their own CLIs do. The YAMNet
SavedModel is loaded from the local artifact copy; nothing is fetched from tfhub.dev.
"""

from __future__ import annotations

import json
import logging
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any

import numpy as np

log = logging.getLogger(__name__)


@dataclass(frozen=True)
class ContextUpdate:
    raw_label: str
    raw_confidence: float
    speech_ratio: float
    feature_event: dict[str, Any]
    context_event: dict[str, Any]
    latency_ms: float


class AudioContextPipeline:
    def __init__(
        self,
        *,
        activity_model: str | Path,
        yamnet_model: str | Path,
        policy_path: str | Path,
        presence_zone: str = "UNKNOWN",
        presence_confidence: float = 0.9,
        simulated: bool = False,
        feature_ttl_seconds: int = 10,
        extractor: Any | None = None,
        head: Any | None = None,
    ) -> None:
        from homemind_context_fusion.policy_config import ContextFusionPolicy
        from homemind_context_fusion.stream_fusion import FeatureStreamFusion

        if head is None:
            from homemind_audio.realtime import _LoadedHead

            head = _LoadedHead(activity_model)
        if head.head != "activity":
            raise ValueError(f"{activity_model} is not an activity head")
        self.head = head
        if extractor is None:
            from homemind_audio.yamnet import YamnetEmbeddingExtractor

            local = Path(yamnet_model)
            if not (local / "saved_model.pb").is_file():
                raise FileNotFoundError(f"local YAMNet SavedModel not found: {local}")
            extractor = YamnetEmbeddingExtractor(str(local.resolve()), feature_version=head.feature_version)
        self.extractor = extractor
        policy_text = Path(policy_path).read_text(encoding="utf-8-sig")
        self.policy = ContextFusionPolicy.from_json(policy_path)
        self.policy_version = str(json.loads(policy_text).get("config_version", Path(policy_path).stem))
        self.fusion = FeatureStreamFusion(engine=self.policy.engine())
        self.presence_zone = presence_zone
        self.presence_confidence = presence_confidence
        self.simulated = simulated
        self.feature_ttl_seconds = feature_ttl_seconds
        self.trace_id = f"trace-{uuid.uuid4()}"
        self.model_version = str(head.model_version)

    def warmup(self) -> None:
        self.extractor.extract_waveform(np.zeros(4 * 16_000, dtype=np.float32))

    def _presence_event(self, timestamp: datetime) -> dict[str, Any]:
        # Configured override (no camera in this demo) -> marked as a PC scenario input.
        return {
            "schema_version": "1.0",
            "event_id": f"presence-{uuid.uuid4()}",
            "trace_id": self.trace_id,
            "timestamp": timestamp.isoformat(),
            "valid_until": (timestamp + timedelta(seconds=self.feature_ttl_seconds)).isoformat(),
            "source": "pc.scenario",
            "source_node_id": "PC-SIMULATION",
            "event_type": "PRESENCE",
            "simulated": True,
            "payload": {"presence_zone": self.presence_zone, "confidence": self.presence_confidence},
        }

    def process(self, window: np.ndarray, *, timestamp: datetime, elapsed_seconds: float) -> ContextUpdate:
        from homemind_audio.audio import vad_temporal_features
        from homemind_audio.models import prediction_to_feature_event

        started = time.perf_counter()
        features = self.extractor.extract_waveform(window)
        prediction = self.head.predict(features)
        speech_ratio = float(np.clip(vad_temporal_features(window)[0], 0.0, 1.0))
        feature_event = prediction_to_feature_event(
            prediction,
            trace_id=self.trace_id,
            speech_ratio=speech_ratio,
            duration_seconds=elapsed_seconds,
            now=timestamp,
            ttl_seconds=self.feature_ttl_seconds,
            simulated=self.simulated,
        )
        if self.presence_zone != "UNKNOWN":
            self.fusion.ingest(self._presence_event(timestamp))
        result = self.fusion.ingest(feature_event)
        if result.context_event is None:
            raise RuntimeError(f"fusion did not fuse the audio window: {result.reason}")
        return ContextUpdate(
            prediction.label,
            float(prediction.confidence),
            speech_ratio,
            feature_event,
            result.context_event,
            (time.perf_counter() - started) * 1000.0,
        )
