"""Export an audio-context activity head (activity.joblib) to plain JSON for the C++ hm_context library.

Supported bundles: a sklearn Pipeline(StandardScaler, LogisticRegression) or homemind_audio's
WeightedProbabilityEnsemble of such pipelines (e.g. public-balanced-v6-ensemble-50-50). Floats are written
with Python repr (exact round trip). The script re-implements the C++ arithmetic in numpy and checks it
against `pipeline.predict_proba` before writing.

Run with the runtime venv (sklearn + homemind_audio installed, read-only use):
    services/runtime/.venv/Scripts/python native/libs/hm_context/tools/export_activity_head.py
Output (never committed): native/.deps/models/activity-head/<artifact>.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[4]
DEFAULT_JOBLIB = REPO / "services/edge-ai/audio-context/artifacts/public-balanced-v6-ensemble-50-50/activity.joblib"
DEFAULT_OUTPUT_DIR = REPO / "native/.deps/models/activity-head"
FORMAT = "hm-activity-head/1"

try:  # unpickling the ensemble needs homemind_audio; fall back to the source tree
    import homemind_audio  # noqa: F401
except ImportError:
    sys.path.insert(0, str(REPO / "services/edge-ai/audio-context/src"))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def export_member(pipeline, classes: np.ndarray) -> dict:
    from sklearn.linear_model import LogisticRegression
    from sklearn.pipeline import Pipeline
    from sklearn.preprocessing import StandardScaler

    if not isinstance(pipeline, Pipeline) or len(pipeline.steps) != 2:
        raise TypeError(f"expected Pipeline(StandardScaler, LogisticRegression), got {pipeline!r}")
    scaler, classifier = pipeline.steps[0][1], pipeline.steps[1][1]
    if not isinstance(scaler, StandardScaler) or not isinstance(classifier, LogisticRegression):
        raise TypeError(f"unsupported pipeline steps: {pipeline.steps}")
    if not np.array_equal(np.asarray(classifier.classes_), classes):
        raise ValueError("member classes differ from the bundle classes")
    n = int(scaler.n_features_in_)
    mean = np.asarray(scaler.mean_, dtype=np.float64) if scaler.with_mean else np.zeros(n)
    scale = np.asarray(scaler.scale_, dtype=np.float64) if scaler.with_std else np.ones(n)
    multi_class = getattr(classifier, "multi_class", "auto")
    ovr = multi_class in ("ovr", "warn") or (
        multi_class in ("auto", "deprecated") and (classes.size <= 2 or classifier.solver == "liblinear"))
    if classes.size == 2:
        proba = "binary_logistic"  # expit(d) -> [1 - p, p]
    elif not ovr:
        proba = "softmax"
    else:
        raise ValueError("one-vs-rest multiclass logistic regression is not supported")
    return {
        "scaler": {"mean": mean.tolist(), "scale": scale.tolist()},
        "logistic": {
            "coef": np.asarray(classifier.coef_, dtype=np.float64).tolist(),
            "intercept": np.asarray(classifier.intercept_, dtype=np.float64).tolist(),
            "proba": proba,
        },
    }


def reference_predict_proba(document: dict, rows: np.ndarray) -> np.ndarray:
    """Same arithmetic as hm::ctx::ActivityHead (float32 scaling like sklearn on float32 input)."""

    total = np.zeros((rows.shape[0], len(document["classes"])))
    for weight, member in zip(document["ensemble"]["weights"], document["members"]):
        mean = np.asarray(member["scaler"]["mean"])
        scale = np.asarray(member["scaler"]["scale"])
        z = (rows.astype(np.float64) - mean).astype(np.float32)
        z = (z.astype(np.float64) / scale).astype(np.float32)
        logits = z.astype(np.float64) @ np.asarray(member["logistic"]["coef"]).T + np.asarray(
            member["logistic"]["intercept"])
        if member["logistic"]["proba"] == "softmax":
            logits = logits - logits.max(axis=1, keepdims=True)
            p = np.exp(logits)
            p /= p.sum(axis=1, keepdims=True)
        else:
            positive = 1.0 / (1.0 + np.exp(-logits[:, 0]))
            p = np.stack([1.0 - positive, positive], axis=1)
        total += float(weight) * p
    return total


def main() -> None:
    import joblib

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--joblib", type=Path, default=DEFAULT_JOBLIB)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()
    output = args.output or DEFAULT_OUTPUT_DIR / f"{args.joblib.parent.name}.json"

    bundle = joblib.load(args.joblib)
    if str(bundle.get("head")) != "activity":
        raise SystemExit(f"{args.joblib} is not an activity head")
    pipeline = bundle["pipeline"]
    classes = np.asarray(pipeline.classes_)
    if hasattr(pipeline, "estimators"):  # WeightedProbabilityEnsemble
        estimators, weights = list(pipeline.estimators), [float(w) for w in pipeline.weights]
        ensemble = dict(bundle.get("ensemble", {}))
        ensemble["weights"] = weights
    else:
        estimators, weights = [pipeline], [1.0]
        ensemble = {"method": "single", "weights": weights}
    document = {
        "format": FORMAT,
        "head": str(bundle["head"]),
        "model_version": str(bundle["model_version"]),
        "feature_version": str(bundle.get("feature_version", "mean-vad-v1")),
        "yamnet_handle": str(bundle.get("yamnet_handle", "https://tfhub.dev/google/yamnet/1")),
        "n_features": int(pipeline.n_features_in_),
        "classes": [str(label) for label in classes],
        "ensemble": ensemble,
        "members": [export_member(estimator, classes) for estimator in estimators],
        "source": {"joblib": args.joblib.relative_to(REPO).as_posix() if args.joblib.is_relative_to(REPO)
                   else str(args.joblib), "joblib_sha256": sha256(args.joblib)},
    }

    rng = np.random.default_rng(7)
    mean0 = np.asarray(document["members"][0]["scaler"]["mean"])
    scale0 = np.asarray(document["members"][0]["scaler"]["scale"])
    rows = (mean0 + scale0 * rng.standard_normal((256, mean0.size)) * 1.5).astype(np.float32)
    expected = pipeline.predict_proba(rows)
    actual = reference_predict_proba(document, rows)
    max_diff = float(np.abs(expected - actual).max())
    if max_diff > 1e-9 or not np.array_equal(expected.argmax(1), actual.argmax(1)):
        raise AssertionError(f"exported head diverges from sklearn: max diff {max_diff}")
    document["export_check"] = {"rows": int(rows.shape[0]), "max_abs_diff_vs_sklearn": max_diff}

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")
    print(json.dumps({"file": str(output.relative_to(REPO).as_posix()), "sha256": sha256(output),
                      "size_bytes": output.stat().st_size, "model_version": document["model_version"],
                      "feature_version": document["feature_version"], "classes": document["classes"],
                      "weights": weights, "max_abs_diff_vs_sklearn": max_diff}, indent=2))


if __name__ == "__main__":
    main()
