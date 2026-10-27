"""ONNX runtime and streaming stability gate for partial intent."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from typing import Any
from uuid import uuid4

import numpy as np
import onnxruntime as ort

from .features import normalize_text, vectorize_one
from .policy import apply_safety_policy, requires_final_transcript
from .taxonomy import HINTS


class IntentRuntime:
    def __init__(self, model_path: str | Path, metadata_path: str | Path | None = None):
        self.model_path = Path(model_path).resolve()
        metadata_file = (
            Path(metadata_path).resolve()
            if metadata_path is not None
            else self.model_path.with_suffix(".json")
        )
        self.metadata = json.loads(metadata_file.read_text(encoding="utf-8"))
        self.classes = tuple(self.metadata["classes"])
        self.dimensions = int(self.metadata["feature_dimensions"])
        self.threshold = float(self.metadata["confidence_threshold"])
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        self.session = ort.InferenceSession(
            str(self.model_path), sess_options=options, providers=["CPUExecutionProvider"]
        )

    def probabilities(self, text: str) -> dict[str, float]:
        features = vectorize_one(text, dimensions=self.dimensions)[None, :]
        values = self.session.run(["probabilities"], {"features": features})[0][0]
        return {label: float(value) for label, value in zip(self.classes, values)}

    def predict(
        self,
        text: str,
        *,
        is_final: bool = False,
        trace_id: str | None = None,
        threshold: float | None = None,
    ) -> dict[str, Any]:
        normalized = normalize_text(text)
        malformed_text = any(0xD800 <= ord(character) <= 0xDFFF for character in normalized)
        if malformed_text:
            intent = "NO_ACTION"
            confidence = 1.0
        else:
            probabilities = self.probabilities(normalized)
            intent = max(probabilities, key=probabilities.get)
            confidence = probabilities[intent]
            intent = apply_safety_policy(normalized, intent)
            if intent == "NO_ACTION":
                confidence = max(confidence, probabilities.get("NO_ACTION", 0.0))
        active_threshold = self.threshold if threshold is None else threshold
        if intent == "NO_ACTION":
            result = "SUPPRESSED"
        elif (
            len(normalized) < 4
            or confidence < active_threshold
            or (not is_final and requires_final_transcript(normalized))
        ):
            result = "INSUFFICIENT_EVIDENCE"
        else:
            result = "DETECTED"
        hints = HINTS[intent] if result == "DETECTED" else HINTS["NO_ACTION"]
        return {
            "schema_version": "1.0",
            "event_id": str(uuid4()),
            "trace_id": trace_id or str(uuid4()),
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "source": "pi5.partial-intent",
            "source_node_id": "NODE-RPI5-02",
            "text_sha256": hashlib.sha256(
                normalized.encode("utf-8", errors="replace")
            ).hexdigest(),
            "text_length": len(normalized),
            "is_final": bool(is_final),
            "intent": intent,
            "confidence": round(confidence, 6),
            "result": result,
            "candidate_devices": hints["candidate_devices"],
            "safe_preparations": hints["safe_preparations"],
            "requires_confirmation": result == "DETECTED",
            "execution_allowed": False,
            "model": {
                "name": "homemind-partial-intent",
                "version": self.metadata["model_version"],
                "runtime": "ONNX_RUNTIME",
            },
        }


class StreamIntentGate:
    """Require repeated matching updates before emitting an early intent."""

    def __init__(
        self,
        runtime: IntentRuntime,
        *,
        confirmation_updates: int = 2,
        threshold: float | None = None,
    ):
        if confirmation_updates < 1:
            raise ValueError("confirmation_updates must be positive")
        self.runtime = runtime
        self.confirmation_updates = confirmation_updates
        self.threshold = (
            float(runtime.metadata.get("streaming_threshold", runtime.threshold))
            if threshold is None
            else threshold
        )
        self._candidate: str | None = None
        self._count = 0
        self._emitted: str | None = None

    def update(self, text: str, *, is_final: bool = False, trace_id: str | None = None) -> dict[str, Any]:
        event = self.runtime.predict(
            text, is_final=is_final, trace_id=trace_id, threshold=self.threshold
        )
        candidate = event["intent"] if event["result"] == "DETECTED" else None
        if candidate is None:
            self._candidate = None
            self._count = 0
            return event
        if candidate == self._candidate:
            self._count += 1
        else:
            self._candidate = candidate
            self._count = 1
        if candidate == self._emitted:
            event["result"] = "SUPPRESSED"
            event["candidate_devices"] = []
            event["safe_preparations"] = []
            event["requires_confirmation"] = False
        elif self._count < self.confirmation_updates and not is_final:
            event["result"] = "INSUFFICIENT_EVIDENCE"
            event["candidate_devices"] = []
            event["safe_preparations"] = []
            event["requires_confirmation"] = False
        else:
            self._emitted = candidate
        return event
