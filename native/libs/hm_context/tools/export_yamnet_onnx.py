"""Export the local YAMNet SavedModel (TF Hub google/yamnet/1 copy) to ONNX for ONNX Runtime 1.20.1.

The exported graph keeps YAMNet's own frontend (padding, STFT, mel, log, 0.96 s patches), so the C++ side
feeds the 16 kHz float32 waveform exactly like `model(waveform)` in Python and gets the [frames, 1024]
embeddings. Only the embeddings output is kept (scores/log-mel are pruned).

Run with the export venv (TF 2.20 + tf2onnx 1.17 + onnx + onnxruntime 1.20.1, see README):
    native/.deps/venv-export/Scripts/python native/libs/hm_context/tools/export_yamnet_onnx.py
Output (never committed): native/.deps/models/yamnet/yamnet-embeddings.onnx + .json report (SHA-256, size,
parity vs TensorFlow on deterministic synthetic waveforms).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[4]
DEFAULT_SAVED_MODEL = REPO / "services/edge-ai/audio-context/artifacts/yamnet-tfhub-1-savedmodel"
DEFAULT_OUTPUT = REPO / "native/.deps/models/yamnet/yamnet-embeddings.onnx"
EMBEDDINGS_OUTPUT = "output_1"  # serving_default: output_0 scores, output_1 embeddings, output_2 log-mel


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def convert(saved_model: Path, target: Path, opset: int) -> None:
    command = [sys.executable, "-m", "tf2onnx.convert", "--saved-model", str(saved_model), "--signature_def",
               "serving_default", "--tag", "serve", "--opset", str(opset), "--output", str(target)]
    # tf2onnx's optimizer iterates sets: a fixed hash seed makes the output byte-reproducible.
    env = {**os.environ, "PYTHONHASHSEED": "0"}
    result = subprocess.run(command, capture_output=True, text=True, env=env)
    if result.returncode != 0 or not target.is_file():
        raise RuntimeError(f"tf2onnx failed\n{result.stdout[-4000:]}\n{result.stderr[-4000:]}")


def canonicalize(model) -> None:
    """Rename tensors/nodes by a name-independent traversal so repeated exports are byte-identical.

    tf2onnx numbers its helper tensors (const_axes__15, ...) and orders nodes differently from run to run;
    a depth-first walk from the graph outputs along each node's ordered inputs only depends on structure.
    """

    graph = model.graph
    producer = {name: node for node in graph.node for name in node.output}
    order, visited = [], set()

    def visit(tensor: str) -> None:
        node = producer.get(tensor)
        if node is None or id(node) in visited:
            return
        visited.add(id(node))
        for name in node.input:
            if name:
                visit(name)
        order.append(node)

    for output in graph.output:
        visit(output.name)
    keep = {item.name for item in graph.input} | {item.name for item in graph.output}
    mapping: dict[str, str] = {name: name for name in keep}

    def rename(name: str) -> str:
        if name and name not in mapping:
            mapping[name] = f"t{len(mapping) - len(keep)}"
        return mapping.get(name, name)

    nodes = []
    for index, node in enumerate(order):
        node.input[:] = [rename(name) for name in node.input]
        node.output[:] = [rename(name) for name in node.output]
        node.name = f"n{index}_{node.op_type}"
        nodes.append(node)
    initializers = []
    for item in graph.initializer:
        if item.name in mapping:
            item.name = mapping[item.name]
            initializers.append(item)
    initializers.sort(key=lambda item: int(item.name[1:]))
    del graph.node[:]
    graph.node.extend(nodes)
    del graph.initializer[:]
    graph.initializer.extend(initializers)
    del graph.value_info[:]
    graph.name = "yamnet_embeddings"


def prune_to_embeddings(source: Path, target: Path) -> None:
    import onnx
    from onnx.utils import Extractor

    full = onnx.load(str(source))
    model = Extractor(full).extract_model(["waveform"], [EMBEDDINGS_OUTPUT])
    # Keep the IR/opset of the converter output (ORT 1.20.1 supports IR <= 10).
    model.ir_version = full.ir_version
    del model.opset_import[:]
    model.opset_import.extend(full.opset_import)
    for node in model.graph.node:
        node.output[:] = ["embeddings" if name == EMBEDDINGS_OUTPUT else name for name in node.output]
    model.graph.output[0].name = "embeddings"
    canonicalize(model)
    model.producer_name = "hm_context export_yamnet_onnx (tf2onnx)"
    model.doc_string = "YAMNet (TF Hub google/yamnet/1) waveform[16 kHz float32] -> embeddings[frames, 1024]"
    onnx.checker.check_model(model)
    onnx.save(model, str(target))


def synthetic_waveforms() -> list[np.ndarray]:
    rng = np.random.default_rng(20261022)
    out = []
    t = np.arange(64000) / 16000.0
    out.append((0.3 * np.sin(2 * np.pi * 440 * t) + 0.05 * rng.standard_normal(t.size)).astype(np.float32))
    out.append((0.2 * rng.standard_normal(64000)).astype(np.float32))
    out.append(np.zeros(64000, dtype=np.float32))
    for size in (1000, 15600, 16000, 48000, 70000):
        out.append(np.clip(0.25 * rng.standard_normal(size), -1, 1).astype(np.float32))
    return out


def verify(onnx_path: Path, saved_model: Path) -> dict[str, float]:
    import onnxruntime as ort
    import tensorflow as tf

    reference = tf.saved_model.load(str(saved_model))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 2
    session = ort.InferenceSession(str(onnx_path), options, providers=["CPUExecutionProvider"])
    min_cos, max_abs = 1.0, 0.0
    for waveform in synthetic_waveforms():
        _, expected, _ = reference(waveform)
        expected = expected.numpy()
        (actual,) = session.run(["embeddings"], {"waveform": waveform})
        if actual.shape != expected.shape:
            raise AssertionError(f"shape {actual.shape} != {expected.shape} for {waveform.size} samples")
        max_abs = max(max_abs, float(np.abs(actual - expected).max()))
        for row_a, row_e in zip(actual.astype(np.float64), expected.astype(np.float64)):
            na, ne = np.linalg.norm(row_a), np.linalg.norm(row_e)
            cos = 1.0 if na == 0 and ne == 0 else float(row_a @ row_e / (na * ne))
            min_cos = min(min_cos, cos)
    if min_cos < 0.9999:
        raise AssertionError(f"ONNX export diverges from TensorFlow: min cosine {min_cos}")
    return {"min_cosine": min_cos, "max_abs_diff": max_abs}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--saved-model", type=Path, default=DEFAULT_SAVED_MODEL)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--opset", type=int, default=17)
    args = parser.parse_args()
    if not (args.saved_model / "saved_model.pb").is_file():
        raise SystemExit(f"local YAMNet SavedModel not found: {args.saved_model}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        full = Path(scratch) / "yamnet-full.onnx"
        convert(args.saved_model, full, args.opset)
        prune_to_embeddings(full, args.output)
    parity = verify(args.output, args.saved_model)

    import onnx
    import onnxruntime
    import tensorflow as tf
    import tf2onnx

    report = {
        "file": args.output.name,
        "sha256": sha256(args.output),
        "size_bytes": args.output.stat().st_size,
        "source": "services/edge-ai/audio-context/artifacts/yamnet-tfhub-1-savedmodel (TF Hub google/yamnet/1)",
        "source_saved_model_pb_sha256": sha256(args.saved_model / "saved_model.pb"),
        "source_variables_sha256": sha256(args.saved_model / "variables" / "variables.data-00000-of-00001"),
        "input": "waveform float32[samples] 16 kHz mono in [-1, 1]",
        "output": "embeddings float32[frames, 1024] (0.96 s patches, 0.48 s hop)",
        "opset": args.opset,
        "tools": {"tensorflow": tf.__version__, "tf2onnx": tf2onnx.__version__, "onnx": onnx.__version__,
                  "onnxruntime": onnxruntime.__version__},
        "parity_vs_tensorflow_synthetic": parity,
    }
    report_path = args.output.with_suffix(".onnx.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
