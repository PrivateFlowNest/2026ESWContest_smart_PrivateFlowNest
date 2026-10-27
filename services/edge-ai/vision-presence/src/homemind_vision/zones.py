"""Map one detected figure to a named room zone without identity inference."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Iterable, Mapping, Sequence


ALLOWED_ZONES = frozenset(
    {"BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR"}
)


@dataclass(frozen=True)
class BoundingBox:
    x1: float
    y1: float
    x2: float
    y2: float
    confidence: float

    def __post_init__(self) -> None:
        if not (0.0 <= self.x1 < self.x2 <= 1.0 and 0.0 <= self.y1 < self.y2 <= 1.0):
            raise ValueError("bounding box coordinates must be normalized and ordered")
        if not 0.0 <= self.confidence <= 1.0:
            raise ValueError("confidence must be between 0 and 1")

    @property
    def foot_point(self) -> tuple[float, float]:
        """Bottom-center is more stable than box center at zone boundaries."""

        return ((self.x1 + self.x2) / 2.0, self.y2)


def _point_in_polygon(point: tuple[float, float], polygon: Sequence[Sequence[float]]) -> bool:
    x, y = point
    inside = False
    previous = polygon[-1]
    for current in polygon:
        x1, y1 = float(previous[0]), float(previous[1])
        x2, y2 = float(current[0]), float(current[1])
        if (y1 > y) != (y2 > y):
            crossing_x = (x2 - x1) * (y - y1) / (y2 - y1) + x1
            if x <= crossing_x:
                inside = not inside
        previous = current
    return inside


class ZoneMap:
    def __init__(self, camera_id: str, zones: Mapping[str, Sequence[Sequence[float]]]) -> None:
        if not camera_id:
            raise ValueError("camera_id is required")
        normalized: dict[str, tuple[tuple[float, float], ...]] = {}
        for name, polygon in zones.items():
            if name not in ALLOWED_ZONES:
                raise ValueError(f"unsupported zone: {name}")
            points = tuple((float(point[0]), float(point[1])) for point in polygon)
            if len(points) < 3:
                raise ValueError(f"zone {name} requires at least three points")
            if any(not (0.0 <= x <= 1.0 and 0.0 <= y <= 1.0) for x, y in points):
                raise ValueError(f"zone {name} coordinates must be normalized")
            normalized[name] = points
        self.camera_id = camera_id
        self.zones = normalized

    @classmethod
    def load(cls, path: str | Path) -> "ZoneMap":
        value = json.loads(Path(path).read_text(encoding="utf-8"))
        if not isinstance(value, dict) or not isinstance(value.get("zones"), dict):
            raise ValueError("zone configuration must contain a zones object")
        return cls(str(value.get("camera_id", "")), value["zones"])

    def locate(self, box: BoundingBox) -> str:
        for name, polygon in self.zones.items():
            if _point_in_polygon(box.foot_point, polygon):
                return name
        return "UNKNOWN"


class ZoneStabilizer:
    """Emit a change only after the same zone is observed repeatedly."""

    def __init__(self, *, confirmation_count: int = 3, lost_count: int = 3) -> None:
        if confirmation_count < 1 or lost_count < 1:
            raise ValueError("confirmation_count and lost_count must be positive")
        self.confirmation_count = confirmation_count
        self.lost_count = lost_count
        self.current = "UNKNOWN"
        self._candidate = "UNKNOWN"
        self._candidate_count = 0
        self._lost_count = 0

    def update(self, zone: str | None) -> str | None:
        observed = zone or "UNKNOWN"
        if observed != "UNKNOWN":
            self._lost_count = 0
        elif self.current != "UNKNOWN":
            self._lost_count += 1
            if self._lost_count < self.lost_count:
                return None
            self.current = "UNKNOWN"
            self._candidate = "UNKNOWN"
            self._candidate_count = 0
            return self.current

        if observed == self.current:
            self._candidate = observed
            self._candidate_count = 0
            return None
        if observed != self._candidate:
            self._candidate = observed
            self._candidate_count = 1
        else:
            self._candidate_count += 1
        if self._candidate_count < self.confirmation_count:
            return None
        self.current = observed
        self._candidate_count = 0
        return self.current
