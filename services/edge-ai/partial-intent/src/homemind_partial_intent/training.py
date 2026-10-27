"""Train, export, and evaluate the lightweight partial-intent classifier."""

from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import time
from typing import Any

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix, f1_score

from .dataset import load_rows
from .features import vectorize
from .policy import apply_safety_policy, requires_final_transcript
from .taxonomy import INTENTS


FEATURE_DIMENSIONS = 4096
MODEL_VERSION = "partial-intent-v2"


def _xy(rows: list[dict[str, Any]]) -> tuple[np.ndarray, np.ndarray]:
    return (
        vectorize([row["text"] for row in rows], dimensions=FEATURE_DIMENSIONS),
        np.asarray([row["intent"] for row in rows]),
    )


def _choose_threshold(
    labels: np.ndarray, predictions: np.ndarray, confidences: np.ndarray
) -> tuple[float, dict[str, float]]:
    """Maximize correctly detected positive coverage with <=5% NO_ACTION FPR."""
    positive = labels != "NO_ACTION"
    negative = ~positive
    best: tuple[float, float, float] | None = None
    best_metrics: dict[str, float] = {}
    for threshold in np.arange(0.35, 0.951, 0.01):
        emitted = (predictions != "NO_ACTION") & (confidences >= threshold)
        correct_positive = emitted & positive & (predictions == labels)
        positive_recall = float(correct_positive.sum() / max(1, positive.sum()))
        false_positive_rate = float((emitted & negative).sum() / max(1, negative.sum()))
        precision = float(
            (correct_positive.sum() / emitted.sum()) if emitted.sum() else 1.0
        )
        if false_positive_rate <= 0.05:
            score = (positive_recall, precision, -float(threshold))
            if best is None or score > best:
                best = score
                best_metrics = {
                    "positive_recall": positive_recall,
                    "proactive_false_positive_rate": false_positive_rate,
                    "detected_precision": precision,
                }
                chosen = float(threshold)
    if best is None:
        return 0.95, {
            "positive_recall": 0.0,
            "proactive_false_positive_rate": 0.0,
            "detected_precision": 1.0,
        }
    return round(chosen, 2), best_metrics


def _apply_policy(texts: list[str], predictions: np.ndarray) -> np.ndarray:
    return np.asarray(
        [apply_safety_policy(text, str(prediction)) for text, prediction in zip(texts, predictions)]
    )


def _export_onnx(classifier: LogisticRegression, output: Path) -> None:
    weights = np.asarray(classifier.coef_.T, dtype=np.float32)
    bias = np.asarray(classifier.intercept_, dtype=np.float32)
    graph = helper.make_graph(
        [
            helper.make_node("MatMul", ["features", "weights"], ["logits_raw"]),
            helper.make_node("Add", ["logits_raw", "bias"], ["logits"]),
            helper.make_node("Softmax", ["logits"], ["probabilities"], axis=1),
        ],
        "HomeMindPartialIntent",
        [helper.make_tensor_value_info("features", TensorProto.FLOAT, [None, FEATURE_DIMENSIONS])],
        [helper.make_tensor_value_info("probabilities", TensorProto.FLOAT, [None, len(classifier.classes_)])],
        [numpy_helper.from_array(weights, "weights"), numpy_helper.from_array(bias, "bias")],
    )
    model = helper.make_model(
        graph,
        producer_name="homemind-partial-intent",
        opset_imports=[helper.make_opsetid("", 13)],
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    output.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, output)


def train_model(data_directory: str | Path, output_model: str | Path) -> dict[str, Any]:
    data_directory = Path(data_directory).resolve()
    output_model = Path(output_model).resolve()
    train_rows = load_rows(data_directory, "train")
    validation_rows = load_rows(data_directory, "validation")
    x_train, y_train = _xy(train_rows)
    x_validation, y_validation = _xy(validation_rows)

    candidates: list[tuple[float, LogisticRegression, float]] = []
    for regularization in (1.0, 3.0, 10.0):
        classifier = LogisticRegression(
            C=regularization,
            class_weight="balanced",
            max_iter=2000,
            random_state=2026,
            solver="lbfgs",
        )
        classifier.fit(x_train, y_train)
        raw_predictions = classifier.predict(x_validation)
        final_predictions = _apply_policy(
            [row["text"] for row in validation_rows], raw_predictions
        )
        score = float(f1_score(y_validation, final_predictions, average="macro"))
        candidates.append((score, classifier, regularization))
    validation_macro_f1, classifier, regularization = max(candidates, key=lambda value: value[0])
    validation_probabilities = classifier.predict_proba(x_validation)
    validation_indices = validation_probabilities.argmax(axis=1)
    validation_raw_predictions = classifier.classes_[validation_indices]
    validation_predictions = _apply_policy(
        [row["text"] for row in validation_rows], validation_raw_predictions
    )
    validation_confidences = validation_probabilities.max(axis=1)
    threshold, gate_metrics = _choose_threshold(
        y_validation, validation_predictions, validation_confidences
    )

    _export_onnx(classifier, output_model)
    digest = hashlib.sha256(output_model.read_bytes()).hexdigest()
    metadata = {
        "model_version": MODEL_VERSION,
        "classes": classifier.classes_.tolist(),
        "feature_dimensions": FEATURE_DIMENSIONS,
        "feature_version": "fnv1a32-char-ngram-plus-korean-semantic-cues-l2-v3",
        "confidence_threshold": threshold,
        "streaming_threshold": 0.30,
        "streaming_confirmation_updates": 2,
        "training_source": "AUTHORED_SYNTHETIC_V2",
        "model_sha256": digest,
        "training_rows": len(train_rows),
        "validation_rows": len(validation_rows),
        "regularization_c": regularization,
        "validation_macro_f1": validation_macro_f1,
        "validation_gate_metrics": gate_metrics,
    }
    output_model.with_suffix(".json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return metadata


def _gate_metrics(
    labels: np.ndarray, predictions: np.ndarray, confidences: np.ndarray, threshold: float
) -> dict[str, float]:
    positive = labels != "NO_ACTION"
    negative = ~positive
    emitted = (predictions != "NO_ACTION") & (confidences >= threshold)
    correct = emitted & (predictions == labels)
    return {
        "detected_coverage": float(emitted.sum() / max(1, len(labels))),
        "detected_precision": float(correct.sum() / emitted.sum()) if emitted.sum() else 1.0,
        "positive_recall": float((correct & positive).sum() / max(1, positive.sum())),
        "proactive_false_positive_rate": float((emitted & negative).sum() / max(1, negative.sum())),
    }


def _stream_gate_metrics(
    rows: list[dict[str, Any]],
    model_path: Path,
    confirmation_updates: int,
    threshold: float | None = None,
) -> dict[str, Any]:
    from .runtime import IntentRuntime, StreamIntentGate

    runtime = IntentRuntime(model_path)
    expected: list[str] = []
    predicted: list[str] = []
    for row in rows:
        words = row["text"].split()
        cut_points = sorted(
            {
                max(1, int(np.ceil(len(words) * fraction)))
                for fraction in (0.6, 0.8, 1.0)
            }
        )
        gate = StreamIntentGate(
            runtime, confirmation_updates=confirmation_updates, threshold=threshold
        )
        detections = []
        for cut_point in cut_points:
            event = gate.update(" ".join(words[:cut_point]), is_final=False)
            if event["result"] == "DETECTED":
                detections.append(event["intent"])
        expected.append(row["intent"])
        predicted.append(detections[-1] if detections else "NO_ACTION")
    expected_array = np.asarray(expected)
    predicted_array = np.asarray(predicted)
    direct_commands = np.asarray(
        [requires_final_transcript(row["text"]) for row in rows], dtype=bool
    )
    positive = (expected_array != "NO_ACTION") & ~direct_commands
    negative = expected_array == "NO_ACTION"
    evaluated = positive | negative
    false_positive = negative & (predicted_array != "NO_ACTION")
    correct_positive = positive & (predicted_array == expected_array)
    false_positive_texts = [
        row["text"] for row, is_false_positive in zip(rows, false_positive) if is_false_positive
    ]
    missed_positive_texts = [
        row["text"]
        for row, is_positive, is_correct in zip(rows, positive, correct_positive)
        if is_positive and not is_correct
    ]
    return {
        "simulation": (
            "non-final word prefixes at 60%, 80%, and 100%; "
            f"{confirmation_updates} matching updates required"
        ),
        "confirmation_updates": confirmation_updates,
        "threshold": runtime.threshold if threshold is None else threshold,
        "eligible_accuracy": float(
            accuracy_score(expected_array[evaluated], predicted_array[evaluated])
        ),
        "positive_recall": float(correct_positive.sum() / max(1, positive.sum())),
        "proactive_false_positive_rate": float(false_positive.sum() / max(1, negative.sum())),
        "false_positive_count": int(false_positive.sum()),
        "negative_count": int(negative.sum()),
        "early_eligible_positive_count": int(positive.sum()),
        "deferred_direct_command_count": int(
            ((expected_array != "NO_ACTION") & direct_commands).sum()
        ),
        "false_positive_texts": false_positive_texts,
        "missed_positive_texts": missed_positive_texts,
    }


def evaluate_model(
    data_directory: str | Path,
    model_path: str | Path,
    report_path: str | Path | None = None,
    input_file: str | Path | None = None,
) -> dict[str, Any]:
    import onnxruntime as ort

    data_directory = Path(data_directory).resolve()
    model_path = Path(model_path).resolve()
    metadata = json.loads(model_path.with_suffix(".json").read_text(encoding="utf-8"))
    if input_file is None:
        rows = load_rows(data_directory, "test")
        dataset_source = "AUTHORED_SYNTHETIC_V2"
        acceptance_scope = "PC synthetic seed baseline; not Pi 5 or live-ASR acceptance"
    else:
        input_path = Path(input_file).resolve()
        rows = [
            json.loads(line)
            for line in input_path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        dataset_source = input_path.name
        acceptance_scope = "PC independent Korean text challenge set; not Pi 5 or live-ASR acceptance"
    features, labels = _xy(rows)
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    probabilities = session.run(["probabilities"], {"features": features})[0]
    indices = probabilities.argmax(axis=1)
    classes = np.asarray(metadata["classes"])
    raw_predictions = classes[indices]
    predictions = _apply_policy([row["text"] for row in rows], raw_predictions)
    confidences = probabilities.max(axis=1)
    threshold = float(metadata["confidence_threshold"])
    report = {
        "dataset_source": dataset_source,
        "acceptance_scope": acceptance_scope,
        "test_rows": len(rows),
        "class_counts": dict(Counter(labels.tolist())),
        "raw_model_accuracy": float(accuracy_score(labels, raw_predictions)),
        "accuracy": float(accuracy_score(labels, predictions)),
        "macro_f1": float(f1_score(labels, predictions, average="macro")),
        "gate": _gate_metrics(labels, predictions, confidences, threshold),
        "stream_gate": _stream_gate_metrics(
            rows,
            model_path,
            int(metadata["streaming_confirmation_updates"]),
            float(metadata["streaming_threshold"]),
        ),
        "execution_safety": {
            "execution_allowed": False,
            "executable_false_positive_rate": 0.0,
            "reason": "partial-intent contract forbids executable commands",
        },
        "threshold": threshold,
        "classification_report": classification_report(
            labels, predictions, labels=list(INTENTS), output_dict=True, zero_division=0
        ),
        "confusion_matrix": confusion_matrix(labels, predictions, labels=list(INTENTS)).tolist(),
        "labels": list(INTENTS),
    }
    if report_path is not None:
        destination = Path(report_path).resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
    return report


def benchmark_model(
    data_directory: str | Path, model_path: str | Path, *, runs: int = 1000
) -> dict[str, float | int | str]:
    from .runtime import IntentRuntime

    if runs < 10:
        raise ValueError("runs must be at least 10")
    rows = load_rows(Path(data_directory).resolve(), "test")
    runtime = IntentRuntime(model_path)
    runtime.predict(rows[0]["text"])
    timings = []
    for index in range(runs):
        started = time.perf_counter_ns()
        runtime.predict(rows[index % len(rows)]["text"])
        timings.append((time.perf_counter_ns() - started) / 1_000_000)
    return {
        "scope": "PC end-to-end text normalization, feature extraction, ONNX inference, event creation",
        "runs": runs,
        "p50_ms": float(np.percentile(timings, 50)),
        "p95_ms": float(np.percentile(timings, 95)),
        "maximum_ms": float(np.max(timings)),
    }
