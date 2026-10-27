from pathlib import Path

import joblib
import numpy as np
from sklearn.linear_model import LogisticRegression

from homemind_audio.ensemble import build_activity_ensemble


def _write_bundle(path: Path, x: np.ndarray, y: np.ndarray, c_value: float) -> None:
    pipeline = LogisticRegression(C=c_value, random_state=42).fit(x, y)
    joblib.dump(
        {
            "pipeline": pipeline,
            "head": "activity",
            "model_version": f"member-{c_value}",
            "yamnet_handle": "test-yamnet",
            "feature_version": "test-features",
        },
        path,
    )


def test_build_activity_ensemble_averages_probabilities_and_round_trips(tmp_path):
    x = np.asarray([[-2.0], [-1.0], [1.0], [2.0]], dtype=np.float32)
    y = np.asarray(["MEETING", "MEETING", "TV", "TV"])
    first = tmp_path / "first.joblib"
    second = tmp_path / "second.joblib"
    output = tmp_path / "ensemble.joblib"
    _write_bundle(first, x, y, 0.1)
    _write_bundle(second, x, y, 10.0)

    summary = build_activity_ensemble((first, second), (1.0, 3.0), output)
    bundle = joblib.load(output)
    features = np.asarray([[-0.5], [0.5]], dtype=np.float32)
    expected = (
        0.25 * joblib.load(first)["pipeline"].predict_proba(features)
        + 0.75 * joblib.load(second)["pipeline"].predict_proba(features)
    )

    assert summary["weights"] == [0.25, 0.75]
    assert np.allclose(bundle["pipeline"].predict_proba(features), expected)
    assert bundle["pipeline"].predict(features).shape == (2,)
