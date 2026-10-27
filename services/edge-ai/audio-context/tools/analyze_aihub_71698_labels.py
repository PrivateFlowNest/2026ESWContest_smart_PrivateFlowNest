from __future__ import annotations

import argparse
import json
import zipfile
from collections import Counter
from pathlib import Path
from typing import Any


DEFAULT_TERMS = (
    "초인종",
    "도어벨",
    "현관벨",
    "벨소리",
    "세탁",
    "완료음",
)


def analyze(root: Path, terms: tuple[str, ...]) -> dict[str, Any]:
    taxonomy: Counter[str] = Counter()
    matches: list[dict[str, str]] = []
    errors: list[str] = []
    archives = sorted(root.rglob("*환경소리데이터_*_01.주거지역.zip"))

    for path in archives:
        with zipfile.ZipFile(path) as archive:
            for entry in archive.infolist():
                if not entry.filename.lower().endswith(".json"):
                    continue
                try:
                    document = json.loads(archive.read(entry).decode("utf-8-sig"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    if len(errors) < 20:
                        errors.append(f"{path.name}:{entry.filename}: {exc}")
                    continue
                category = str(document.get("Category", "")).strip()
                subcategory = str(document.get("SubCategory", "")).strip()
                comment = str(document.get("SoundComment", "")).strip()
                taxonomy[f"{category}/{subcategory}"] += 1
                searchable = " ".join((category, subcategory, comment))
                matched = [term for term in terms if term in searchable]
                if matched:
                    matches.append(
                        {
                            "archive": path.name,
                            "entry": entry.filename,
                            "category": category,
                            "subcategory": subcategory,
                            "comment": comment,
                            "matched_terms": ",".join(matched),
                        }
                    )

    return {
        "root": str(root.resolve()),
        "archives": len(archives),
        "terms": list(terms),
        "match_count": len(matches),
        "matches": matches,
        "taxonomy": dict(taxonomy.most_common()),
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect residential AI-Hub 71698 environmental labels."
    )
    parser.add_argument("root", type=Path)
    parser.add_argument("--term", action="append", dest="terms")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    report = analyze(args.root, tuple(args.terms or DEFAULT_TERMS))
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
