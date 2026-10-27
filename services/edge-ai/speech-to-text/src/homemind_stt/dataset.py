"""Create a deterministic local-only ASR evaluation manifest from AI Hub labels."""

from __future__ import annotations

import json
from pathlib import Path
import random
import zipfile


def build_aihub_manifest(
    label_zip: str | Path,
    audio_root: str | Path,
    *,
    limit: int | None,
    seed: int = 20260903,
    keywords: tuple[str, ...] = (),
    audio_variant: str = "S",
    domains: tuple[str, ...] = (),
    split_name: str = "validation",
) -> list[dict[str, object]]:
    if limit is not None and limit < 1:
        raise ValueError("limit must be at least 1 or null for all rows")
    if audio_variant not in {"S", "N"}:
        raise ValueError("audio_variant must be S or N")
    root = Path(audio_root)
    if not root.is_dir():
        raise FileNotFoundError(f"audio root does not exist: {root}")

    # AI Hub validation folders can contain tens of thousands of files on an
    # HDD. Enumerating once is much faster than issuing one metadata lookup for
    # every label in the archive.
    available_audio = {
        path.stem[:-2]: path
        for path in root.glob(f"*-{audio_variant}.wav")
        if path.stem.endswith(f"-{audio_variant}")
    }
    candidates: list[dict[str, object]] = []
    with zipfile.ZipFile(label_zip) as archive:
        for entry in archive.infolist():
            if not entry.filename.lower().endswith(".json"):
                continue
            with archive.open(entry) as handle:
                payload = json.load(handle)
            file_info = payload.get("file", {})
            command = payload.get("command", {})
            name = file_info.get("name")
            text = command.get("text")
            if not isinstance(name, str) or not isinstance(text, str) or not text.strip():
                continue
            if keywords and not any(keyword in text for keyword in keywords):
                continue
            if domains and command.get("domain") not in domains:
                continue
            audio = available_audio.get(name)
            if audio is None:
                continue
            candidates.append(
                {
                    "id": name,
                    "audio_path": str(audio),
                    "reference": text.strip(),
                    "duration_seconds": float(file_info.get("length", 0.0)),
                    "domain": command.get("domain"),
                    "sub_domain": command.get("subDomain"),
                    "snr": command.get("snr"),
                    "source": f"AIHUB-71405-{split_name.upper()}-HOME-NEAR",
                    "audio_variant": audio_variant,
                    "privacy_scope": "LOCAL_EVALUATION_ONLY",
                }
            )
    random.Random(seed).shuffle(candidates)
    return candidates if limit is None else candidates[:limit]


def write_jsonl(rows: list[dict[str, object]], output: str | Path) -> None:
    target = Path(output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows),
        encoding="utf-8",
    )


def load_jsonl(path: str | Path) -> list[dict[str, object]]:
    return [
        json.loads(line)
        for line in Path(path).read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
