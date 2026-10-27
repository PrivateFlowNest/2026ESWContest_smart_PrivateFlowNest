"""Inspect AI Hub label archive structure without printing transcript values."""

from __future__ import annotations

import argparse
import json
import zipfile
from collections import Counter
from pathlib import Path
from typing import Any


def outline(value: Any, depth: int = 0, max_depth: int = 4) -> Any:
    if depth >= max_depth:
        return type(value).__name__
    if isinstance(value, dict):
        return {key: outline(item, depth + 1, max_depth) for key, item in value.items()}
    if isinstance(value, list):
        return [outline(value[0], depth + 1, max_depth)] if value else []
    return type(value).__name__


def inspect(path: Path) -> dict[str, object]:
    with zipfile.ZipFile(path) as archive:
        names = [item.filename for item in archive.infolist() if not item.is_dir()]
        extensions = Counter(Path(name).suffix.lower() or "<none>" for name in names)
        json_name = next((name for name in names if name.lower().endswith(".json")), None)
        schema = None
        if json_name:
            with archive.open(json_name) as handle:
                schema = outline(json.load(handle))
        return {
            "archive": str(path),
            "members": len(names),
            "extensions": dict(extensions),
            "sample_member": json_name,
            "schema": schema,
        }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("archives", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([inspect(path) for path in args.archives], ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
