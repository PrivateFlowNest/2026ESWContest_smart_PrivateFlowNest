import json
from pathlib import Path


from homemind_exaone.validation import (
    validate_activity_context,
    validate_proposal,
)


DATA_DIR = Path(__file__).parents[1] / "data" / "finetune-v1"


def _read_jsonl(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def test_generated_splits_are_large_valid_and_disjoint():
    manifest = json.loads((DATA_DIR / "manifest.json").read_text(encoding="utf-8"))

    assert manifest["splits"]["train"]["count"] >= 1000
    assert manifest["splits"]["validation"]["count"] >= 300
    assert manifest["splits"]["test"]["count"] >= 300

    fingerprints = set()
    for split in ("train", "validation", "test"):
        for row in _read_jsonl(DATA_DIR / f"{split}.jsonl"):
            assert row["split"] == split
            validate_activity_context(row["context"])
            validate_proposal(row["expected"])
            assert json.loads(row["completion"][0]["content"]) == row["expected"]
            fingerprint = json.dumps(
                [row["utterance"], row["context"]],
                ensure_ascii=False,
                sort_keys=True,
            )
            assert fingerprint not in fingerprints
            fingerprints.add(fingerprint)
