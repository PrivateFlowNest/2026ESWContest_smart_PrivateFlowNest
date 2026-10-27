"""Shared helpers for the hm_context parity tests (test_context_*.py). PC x86_64 Docker only, not a device test.

Models come from native/libs/hm_context/tools/export_*.py (native/.deps/models, never committed). Real audio
comes from the AIHub test split listed in services/edge-ai/audio-context/data/public-balanced-v6-split.csv
(E:\\HomeMind-Datasets); tests that need it skip when the drive is absent. Arrays travel to the probe as raw
float32 files under native/.deps/tmp/context-parity-<builder> (visible in the container as /w/native/.deps/tmp/...).
"""

from __future__ import annotations

import csv
import json
import os
import sys
from functools import lru_cache
from pathlib import Path
from typing import Any

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import BUILDER, REPO, build, run_probe, run_shell  # noqa: E402

# Build tree used by the tests: "" = the shared /build tree (all libs/services), or the isolated
# "hm_context-only/" tree (hm_common + hm_context via a symlinked source root) when another folder's WIP
# breaks the shared configure. Set by ensure_built().
_TREE = {"prefix": None}
ISOLATED = "hm_context-only"
MODELS = REPO / "native/.deps/models"
YAMNET_ONNX = MODELS / "yamnet/yamnet-embeddings.onnx"
HEAD_JSON = MODELS / "activity-head/public-balanced-v6-ensemble-50-50.json"
AUDIO_CONTEXT = REPO / "services/edge-ai/audio-context"
ACTIVITY_JOBLIB = AUDIO_CONTEXT / "artifacts/public-balanced-v6-ensemble-50-50/activity.joblib"
YAMNET_SAVED_MODEL = AUDIO_CONTEXT / "artifacts/yamnet-tfhub-1-savedmodel"
FUSION = REPO / "services/edge-ai/context-fusion"
POLICY = FUSION / "config/audio-policy-v6-ensemble-50-50.json"
POLICY_HOLD = FUSION / "config/audio-policy-v6.3-hold15.json"  # deployed default (release 15)
MANIFEST = AUDIO_CONTEXT / "data/public-balanced-v6-split.csv"
DATA = REPO / f"native/.deps/tmp/context-parity-{BUILDER}"  # per builder (x86 / arm64 runs)
ACTIVITY_LABELS = ("IN_PERSON_MEETING", "SELF_TALK", "TV_MEDIA", "UNKNOWN", "ZOOM_SPEAKER")


def container(path: Path) -> str:
    """Host path inside the repo -> path in the build container (/w = repo)."""
    return "/w/" + path.resolve().relative_to(REPO).as_posix()


def _isolated_build() -> None:
    command = (
        "set -e; mkdir -p /ov/native/libs; ln -sfn /w/contracts /ov/contracts; ln -sfn /w/services /ov/services; "
        "cp /w/native/CMakeLists.txt /ov/native/; ln -sfn /w/native/.deps /ov/native/.deps; "
        "ln -sfn /w/native/libs/hm_common /ov/native/libs/hm_common; "
        "ln -sfn /w/native/libs/hm_context /ov/native/libs/hm_context; "
        f"cmake -S /ov/native -B /build/{ISOLATED} -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null; "
        f"cmake --build /build/{ISOLATED} --target hm_context_probe hm_context_tests 2>&1 | tail -40")
    result = run_shell(command, timeout=1800)
    if result.returncode != 0 or "FAILED" in result.stdout or "error:" in result.stdout:
        raise RuntimeError(f"isolated hm_context build failed\n{result.stdout}\n{result.stderr}")


def ensure_built() -> str:
    """Build hm_context_probe/tests (Release). Returns the tree prefix under /build."""

    if _TREE["prefix"] is None:
        try:
            build(build_type="Release", target="hm_context_probe hm_context_tests")
            _TREE["prefix"] = ""
        except RuntimeError as error:
            own = [line for line in str(error).splitlines()
                   if "hm_context" in line and ("error" in line.lower() or "FAILED" in line)]
            if own:
                raise
            print(f"\n[hm_context] shared configure/build broken outside hm_context, using /build/{ISOLATED}:\n"
                  + str(error)[:600])
            _isolated_build()
            _TREE["prefix"] = ISOLATED + "/"
    return _TREE["prefix"]


def binary(name: str) -> str:
    return f"{ensure_built()}libs/hm_context/{name}"


def probe(requests: list[dict[str, Any]], timeout: float = 1800) -> list[dict[str, Any]]:
    return run_probe(binary("hm_context_probe"), requests, timeout=timeout)


def require_models() -> None:
    missing = [str(p) for p in (YAMNET_ONNX, HEAD_JSON) if not p.is_file()]
    if missing:
        pytest.skip(f"export the models first (native/libs/hm_context/tools): {missing}")


def write_f32(name: str, values: np.ndarray) -> str:
    DATA.mkdir(parents=True, exist_ok=True)
    path = DATA / name
    np.asarray(values, dtype="<f4").tofile(path)
    return container(path)


def read_f32(name: str, dtype: str = "<f4") -> np.ndarray:
    return np.fromfile(DATA / name, dtype=dtype)


@lru_cache(maxsize=None)
def real_clips(per_label: int = 6) -> tuple[tuple[str, str, float, float], ...]:
    """Deterministic AIHub test-split activity clips: (label, path, start, end)."""

    if not MANIFEST.is_file():
        pytest.skip("audio-context manifest missing")
    rows = list(csv.DictReader(MANIFEST.open(encoding="utf-8-sig")))
    chosen = []
    for label in ACTIVITY_LABELS:
        candidates = sorted((r for r in rows if r["head"] == "activity" and r["split"] == "test" and r["label"] == label),
                            key=lambda r: (r["path"], float(r["start_seconds"])))
        seen_files: set[str] = set()
        for row in candidates:
            if row["path"] in seen_files or not os.path.exists(row["path"]):
                continue  # one segment per source file -> more variety
            seen_files.add(row["path"])
            chosen.append((label, row["path"], float(row["start_seconds"]), float(row["end_seconds"])))
            if len(seen_files) == per_label:
                break
    if len(chosen) < 20:
        pytest.skip(f"only {len(chosen)} real clips available (AIHub data on E: not mounted?)")
    return tuple(chosen)


def load_clip(path: str, start: float, end: float) -> np.ndarray:
    from homemind_audio.audio import load_wav_segment_16k_mono

    return load_wav_segment_16k_mono(path, start, end)


@lru_cache(maxsize=None)
def tf_extractor():
    from homemind_audio.yamnet import YamnetEmbeddingExtractor

    head = python_head()
    return YamnetEmbeddingExtractor(str(YAMNET_SAVED_MODEL.resolve()), feature_version=head.feature_version)


@lru_cache(maxsize=None)
def python_head():
    from homemind_audio.realtime import _LoadedHead

    return _LoadedHead(ACTIVITY_JOBLIB)


def cosine_rows(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    a = a.astype(np.float64)
    b = b.astype(np.float64)
    na, nb = np.linalg.norm(a, axis=1), np.linalg.norm(b, axis=1)
    out = np.ones(a.shape[0])
    nonzero = (na > 0) & (nb > 0)
    out[nonzero] = (a[nonzero] * b[nonzero]).sum(1) / (na[nonzero] * nb[nonzero])
    return out


def strip_ids(value: Any) -> Any:
    """Drop random ids (event_id, trace-* ids) before comparing Python and C++ events."""

    if isinstance(value, dict):
        return {k: strip_ids(v) for k, v in value.items() if k not in ("event_id",)}
    if isinstance(value, list):
        return [strip_ids(v) for v in value]
    return value


def report(name: str, payload: dict[str, Any]) -> None:
    """Print a parity summary (visible with pytest -s) and keep it next to the probe data."""

    DATA.mkdir(parents=True, exist_ok=True)
    (DATA / f"{name}.json").write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\n[{name}] " + json.dumps(payload, ensure_ascii=False))
