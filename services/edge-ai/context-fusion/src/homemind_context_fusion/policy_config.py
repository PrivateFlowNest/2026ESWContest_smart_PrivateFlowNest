"""Versionable runtime settings for the context-fusion state machine."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import json
from pathlib import Path
from typing import Mapping, Any

from .engine import ContextFusionEngine


@dataclass(frozen=True)
class ContextFusionPolicy:
    quiet_audio_confidence: float = 0.0
    meeting_tv_margin: float = 0.0
    activation_updates: int = 3
    release_updates: int = 2
    change_updates: int = 2
    ttl_seconds: float = 10.0

    def __post_init__(self) -> None:
        if not 0.0 <= self.quiet_audio_confidence <= 1.0:
            raise ValueError("quiet_audio_confidence must be between 0 and 1")
        if not 0.0 <= self.meeting_tv_margin <= 1.0:
            raise ValueError("meeting_tv_margin must be between 0 and 1")
        if min(
            self.activation_updates,
            self.release_updates,
            self.change_updates,
        ) < 1:
            raise ValueError("policy update counts must be positive")
        if self.ttl_seconds <= 0:
            raise ValueError("ttl_seconds must be positive")

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "ContextFusionPolicy":
        settings = value.get("engine", value)
        if not isinstance(settings, Mapping):
            raise ValueError("policy config engine must be an object")
        allowed = {
            "quiet_audio_confidence",
            "meeting_tv_margin",
            "activation_updates",
            "release_updates",
            "change_updates",
            "ttl_seconds",
        }
        unknown = set(settings) - allowed
        if unknown:
            raise ValueError(f"unknown policy config fields: {sorted(unknown)}")
        return cls(**{name: settings[name] for name in allowed if name in settings})

    @classmethod
    def from_json(
        cls,
        path: str | Path,
        *,
        require_accepted: bool = True,
    ) -> "ContextFusionPolicy":
        value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
        if not isinstance(value, Mapping):
            raise ValueError("policy config must be a JSON object")
        acceptance = value.get("acceptance")
        if require_accepted and isinstance(acceptance, Mapping):
            failed = [
                name
                for name in ("calibration_passed", "holdout_passed")
                if acceptance.get(name) is not True
            ]
            if failed:
                raise ValueError(
                    "policy config is not accepted for runtime: " + ", ".join(failed)
                )
        return cls.from_mapping(value)

    def engine(self) -> ContextFusionEngine:
        return ContextFusionEngine(**asdict(self))

    def as_dict(self) -> dict[str, object]:
        return asdict(self)
