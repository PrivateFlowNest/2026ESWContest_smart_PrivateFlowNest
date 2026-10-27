"""Small probability ensembles for classifiers sharing one YAMNet embedding."""

from __future__ import annotations

from pathlib import Path
from typing import Iterable

import joblib
import numpy as np


class WeightedProbabilityEnsemble:
    """Average compatible classifier probabilities without rerunning YAMNet."""

    def __init__(self, estimators: Iterable[object], weights: Iterable[float]) -> None:
        self.estimators = tuple(estimators)
        raw_weights = np.asarray(tuple(weights), dtype=np.float64)
        if len(self.estimators) < 2 or len(self.estimators) != raw_weights.size:
            raise ValueError("at least two estimators and one weight per estimator are required")
        if np.any(raw_weights <= 0):
            raise ValueError("ensemble weights must be positive")
        first_classes = np.asarray(self.estimators[0].classes_)
        first_features = int(self.estimators[0].n_features_in_)
        for estimator in self.estimators[1:]:
            if not np.array_equal(first_classes, np.asarray(estimator.classes_)):
                raise ValueError("ensemble estimators must use identical classes")
            if int(estimator.n_features_in_) != first_features:
                raise ValueError("ensemble estimators must use identical feature dimensions")
        self.classes_ = first_classes
        self.n_features_in_ = first_features
        self.weights = raw_weights / raw_weights.sum()

    def predict_proba(self, features: np.ndarray) -> np.ndarray:
        probabilities = [
            float(weight) * np.asarray(estimator.predict_proba(features))
            for estimator, weight in zip(self.estimators, self.weights)
        ]
        return np.sum(probabilities, axis=0)

    def predict(self, features: np.ndarray) -> np.ndarray:
        probabilities = self.predict_proba(features)
        return self.classes_[np.argmax(probabilities, axis=1)]


def build_activity_ensemble(
    model_paths: Iterable[str | Path],
    weights: Iterable[float],
    output_path: str | Path,
    *,
    model_version: str = "yamnet-temporal-cost-ensemble-v0.8.0",
) -> dict[str, object]:
    paths = tuple(Path(path) for path in model_paths)
    bundles = [joblib.load(path) for path in paths]
    if len(bundles) < 2:
        raise ValueError("at least two activity model bundles are required")
    if any(str(bundle.get("head")) != "activity" for bundle in bundles):
        raise ValueError("every ensemble member must be an activity model")
    handles = {str(bundle.get("yamnet_handle")) for bundle in bundles}
    versions = {str(bundle.get("feature_version")) for bundle in bundles}
    if len(handles) != 1 or len(versions) != 1:
        raise ValueError("ensemble members must share YAMNet and feature versions")

    pipeline = WeightedProbabilityEnsemble(
        (bundle["pipeline"] for bundle in bundles), weights
    )
    normalized_weights = [float(value) for value in pipeline.weights]
    bundle = {
        "pipeline": pipeline,
        "head": "activity",
        "model_version": model_version,
        "yamnet_handle": next(iter(handles)),
        "feature_version": next(iter(versions)),
        "ensemble": {
            "method": "weighted_probability_average",
            "weights": normalized_weights,
            "member_model_versions": [
                str(item.get("model_version", "unknown")) for item in bundles
            ],
        },
    }
    target = Path(output_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    joblib.dump(bundle, target)
    return {
        "output": str(target.resolve()),
        "model_version": model_version,
        "weights": normalized_weights,
        "member_model_versions": bundle["ensemble"]["member_model_versions"],
    }
