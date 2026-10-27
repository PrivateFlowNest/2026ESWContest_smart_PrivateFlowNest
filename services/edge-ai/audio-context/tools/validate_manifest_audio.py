"""Validate that every manifest audio file has a readable header."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import soundfile

from homemind_audio.dataset import iter_manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()

    invalid: list[dict[str, str]] = []
    count = 0
    for record in iter_manifest(args.manifest, check_files=True):
        count += 1
        try:
            info = soundfile.info(record.path)
            if info.frames <= 0 or info.samplerate <= 0 or info.channels <= 0:
                raise ValueError("non-positive frames, sample rate, or channels")
        except Exception as exc:  # report all source-file failures together
            invalid.append({"path": str(record.path), "error": str(exc)})

    result = {
        "manifest": str(args.manifest.resolve()),
        "checked": count,
        "invalid_count": len(invalid),
        "invalid": invalid,
    }
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    if invalid:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
