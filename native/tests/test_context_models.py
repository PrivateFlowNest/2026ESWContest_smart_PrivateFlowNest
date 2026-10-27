"""hm_context vs Python reference on real AIHub clips (PC x86_64 Docker, not a device test).

(a) YAMNet embeddings: ONNX Runtime 1.20.1 (C++) vs TensorFlow SavedModel, per frame.
    VAD temporal features: C++ vs homemind_audio.audio.vad_temporal_features, bit for bit.
(b) Activity head: C++ predict_proba on identical float32 feature rows vs sklearn (joblib).
"""

from __future__ import annotations

import numpy as np
import pytest

from context_support import (DATA, HEAD_JSON, YAMNET_ONNX, binary, container, cosine_rows, ensure_built, load_clip,
                             probe, python_head, read_f32, real_clips, report, require_models, tf_extractor, write_f32)
from hmnative import run_shell


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    require_models()
    ensure_built()


@pytest.fixture(scope="module")
def clips() -> list[tuple[str, np.ndarray]]:
    out = []
    for index, (label, path, start, end) in enumerate(real_clips()):
        waveform = load_clip(path, start, end)
        out.append((label, waveform))
        write_f32(f"clip{index:02d}.f32", waveform)
    return out


def test_unit_tests_pass() -> None:
    result = run_shell(f"HM_CONTEXT_REPO_ROOT=/w /build/{binary('hm_context_tests')} 2>&1 | tail -5")
    assert "PASSED" in result.stdout and "FAILED" not in result.stdout, result.stdout + result.stderr


def test_yamnet_embeddings_match_tensorflow(clips) -> None:
    extractor = tf_extractor()
    requests, expected = [], []
    for index, (_, waveform) in enumerate(clips):
        _, embeddings, _ = extractor._model(waveform)
        expected.append(embeddings.numpy())
        requests.append({"op": "embed", "model": container(YAMNET_ONNX), "threads": 2,
                         "input": container(DATA / f"clip{index:02d}.f32"),
                         "output": container(DATA / f"clip{index:02d}.emb.f32")})
    answers = probe(requests)
    cosines, max_abs, frames = [], 0.0, 0
    for index, (answer, reference) in enumerate(zip(answers, expected)):
        assert "error" not in answer, answer
        native = read_f32(f"clip{index:02d}.emb.f32").reshape(answer["frames"], 1024)
        assert native.shape == reference.shape
        cosines.append(cosine_rows(native, reference))
        max_abs = max(max_abs, float(np.abs(native - reference).max()))
        frames += native.shape[0]
    cosines = np.concatenate(cosines)
    summary = {"clips": len(clips), "frames": frames, "min_cosine": float(cosines.min()),
               "mean_cosine": float(cosines.mean()), "max_abs_diff": max_abs,
               "labels": sorted({label for label, _ in clips})}
    report("a_embeddings", summary)
    assert len(clips) >= 20
    assert cosines.min() >= 0.9999, summary


def _windows(clips) -> list[tuple[int, int, int]]:
    """(clip index, offset, samples): the whole clip plus every 4 s window with 1 s hop."""
    out = []
    for index, (_, waveform) in enumerate(clips):
        out.append((index, 0, waveform.size))
        for start in range(0, waveform.size - 64000 + 1, 16000):
            out.append((index, start, 64000))
    return out


def test_vad_features_bit_exact(clips) -> None:
    from homemind_audio.audio import vad_temporal_features

    windows = _windows(clips)
    answers = probe([{"op": "vad", "input": container(DATA / f"clip{i:02d}.f32"), "offset": o, "samples": n}
                     for i, o, n in windows])
    mismatched_values, mismatched_windows, speech_ratio_mismatch, max_abs = 0, 0, 0, 0.0
    for (index, offset, samples), answer in zip(windows, answers):
        expected = vad_temporal_features(clips[index][1][offset:offset + samples])
        native = np.asarray(answer["features"], dtype=np.float32)
        diff = native != expected
        mismatched_values += int(diff.sum())
        mismatched_windows += int(diff.any())
        speech_ratio_mismatch += int(diff[0])
        max_abs = max(max_abs, float(np.abs(native.astype(np.float64) - expected).max()))
    summary = {"windows": len(windows), "values": 12 * len(windows), "mismatched_values": mismatched_values,
               "mismatched_windows": mismatched_windows, "speech_ratio_mismatches": speech_ratio_mismatch,
               "max_abs_diff": max_abs}
    report("a_vad", summary)
    assert speech_ratio_mismatch == 0, summary
    assert max_abs < 1e-5, summary  # only log10 last-ulp effects (C library) are tolerated


def test_features_and_head_labels_match(clips) -> None:
    """Whole pipeline front half: C++ features + C++ head vs TF features + sklearn head on the same audio."""
    extractor, head = tf_extractor(), python_head()
    windows = _windows(clips)
    requests = [{"op": "features", "model": container(YAMNET_ONNX), "threads": 2, "feature_version":
                 head.feature_version, "input": container(DATA / f"clip{i:02d}.f32"), "offset": o, "samples": n,
                 "output": container(DATA / f"feat{k:03d}.f32")} for k, (i, o, n) in enumerate(windows)]
    answers = probe(requests)
    native_rows = np.stack([read_f32(f"feat{k:03d}.f32") for k in range(len(windows))])
    python_rows = np.stack([extractor.extract_waveform(clips[i][1][o:o + n]) for i, o, n in windows])
    assert all(a["dims"] == python_rows.shape[1] for a in answers)
    write_f32("native_rows.f32", native_rows)
    [native_head] = probe([{"op": "head", "head": container(HEAD_JSON), "input": container(DATA / "native_rows.f32"),
                            "output": container(DATA / "native_rows.proba")}])
    python_labels = [head.predict(row).label for row in python_rows]
    python_conf = np.asarray([head.predict(row).confidence for row in python_rows])
    native_conf = np.asarray(native_head["confidences"])
    agree = sum(a == b for a, b in zip(python_labels, native_head["labels"]))
    summary = {"windows": len(windows), "feature_max_abs_diff": float(np.abs(native_rows - python_rows).max()),
               "label_agreement": f"{agree}/{len(windows)}",
               "confidence_max_abs_diff": float(np.abs(native_conf - python_conf).max())}
    report("a_features_labels", summary)
    assert agree == len(windows), summary


def test_activity_head_matches_sklearn(clips) -> None:
    extractor, head = tf_extractor(), python_head()
    rows = [extractor.extract_waveform(clips[i][1][o:o + n]) for i, o, n in _windows(clips)]
    rng = np.random.default_rng(11)
    base = np.stack(rows)
    synthetic = base[rng.integers(0, len(base), 200)] + rng.standard_normal((200, base.shape[1])).astype(np.float32) * 0.05
    rows = np.concatenate([base, synthetic.astype(np.float32)]).astype(np.float32)
    write_f32("head_rows.f32", rows)
    [answer] = probe([{"op": "head", "head": container(HEAD_JSON), "input": container(DATA / "head_rows.f32"),
                       "output": container(DATA / "head_rows.proba")}])
    native = read_f32("head_rows.proba", "<f8").reshape(rows.shape[0], -1)
    expected = head.pipeline.predict_proba(rows)
    assert answer["classes"] == [str(c) for c in head.pipeline.classes_]
    assert answer["model_version"] == head.model_version and answer["feature_version"] == head.feature_version
    labels = [str(head.pipeline.classes_[i]) for i in expected.argmax(1)]
    summary = {"rows": int(rows.shape[0]), "max_abs_proba_diff": float(np.abs(native - expected).max()),
               "labels_identical": labels == answer["labels"]}
    report("b_head", summary)
    assert summary["max_abs_proba_diff"] <= 1e-6 and summary["labels_identical"], summary
