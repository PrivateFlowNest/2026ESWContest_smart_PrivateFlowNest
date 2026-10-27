"""Evaluate ASR without persisting raw transcripts in reports."""

from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
from time import perf_counter
from typing import Callable, Iterable, Mapping

from .metrics import character_error_counts, keyword_group_recall, keyword_group_results


HOMEMIND_KEYWORD_GROUPS = {
    # A bare "불" creates false matches such as "불고기", so light aliases
    # use command-shaped fragments in addition to unambiguous nouns.
    "light": ("조명", "전등", "불을", "불켜", "불꺼"),
    "tv": ("TV", "티비", "티브이", "텔레비전", "텔레비젼"),
    "aircon": ("에어컨", "에어콘", "냉방"),
    "washer": ("세탁기",),
    "window": ("창문",),
    "front_door": ("현관문",),
    "bathroom": ("화장실",),
    "ventilation": ("환기팬", "환풍기"),
    "notification": ("알림",),
    "meeting": ("회의", "미팅"),
    "sound": ("소리", "볼륨", "음량"),
}


def evaluate(
    backend,
    rows: Iterable[Mapping[str, object]],
    *,
    progress: Callable[[str], None] | None = None,
) -> dict[str, object]:
    results: list[dict[str, object]] = []
    total_edits = total_chars = 0
    keyword_hits = keyword_total = 0
    keyword_groups = {
        name: {"hits": 0, "occurrences": 0}
        for name in HOMEMIND_KEYWORD_GROUPS
    }
    total_audio = total_runtime = 0.0
    for index, row in enumerate(rows, start=1):
        started = perf_counter()
        transcript = backend.transcribe(Path(str(row["audio_path"])))
        runtime = perf_counter() - started
        edits, chars = character_error_counts(str(row["reference"]), transcript.text)
        hits, count = keyword_group_recall(
            str(row["reference"]), transcript.text, HOMEMIND_KEYWORD_GROUPS
        )
        group_results = keyword_group_results(
            str(row["reference"]), transcript.text, HOMEMIND_KEYWORD_GROUPS
        )
        for name, hit in group_results.items():
            keyword_groups[name]["hits"] += int(hit)
            keyword_groups[name]["occurrences"] += 1
        duration = float(row.get("duration_seconds", 0.0))
        total_edits += edits
        total_chars += chars
        keyword_hits += hits
        keyword_total += count
        total_audio += duration
        total_runtime += runtime
        results.append(
            {
                "id": row["id"], "edits": edits, "reference_chars": chars,
                "keyword_hits": hits, "keyword_total": count,
                "audio_seconds": duration, "runtime_seconds": round(runtime, 4),
                "partial_update_count": len(transcript.partials),
            }
        )
        if progress:
            progress(f"[{index}] {row['id']} cer={edits/chars:.3f} rtf={runtime/max(duration, 0.001):.3f}")
    cer = total_edits / max(1, total_chars)
    rtf = total_runtime / max(0.001, total_audio)
    keyword_rate = keyword_hits / max(1, keyword_total)
    return {
        "schema_version": "1.0",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "model_version": backend.model_version,
        "total": len(results),
        "character_error_rate": cer,
        "character_accuracy": max(0.0, 1.0 - cer),
        "real_time_factor": rtf,
        "audio_seconds": total_audio,
        "runtime_seconds": total_runtime,
        "homemind_keyword_recall": keyword_rate,
        "keyword_occurrences": keyword_total,
        "keyword_group_metrics": {
            name: {
                **counts,
                "recall": counts["hits"] / max(1, counts["occurrences"]),
            }
            for name, counts in keyword_groups.items()
            if counts["occurrences"] > 0
        },
        "privacy": {
            "raw_audio_in_report": False,
            "references_in_report": False,
            "hypotheses_in_report": False,
            "audio_paths_in_report": False,
        },
        "results": results,
    }


def evaluate_acceptance(
    report: Mapping[str, object], thresholds: Mapping[str, object]
) -> dict[str, object]:
    checks = {
        "character_error_rate": float(report["character_error_rate"])
        <= float(thresholds["character_error_rate_max"]),
        "real_time_factor": float(report["real_time_factor"])
        <= float(thresholds["real_time_factor_max"]),
        "homemind_keyword_recall": float(report["homemind_keyword_recall"])
        >= float(thresholds["homemind_keyword_recall_min"]),
        "raw_transcripts_absent": report["privacy"]
        == {
            "raw_audio_in_report": False,
            "references_in_report": False,
            "hypotheses_in_report": False,
            "audio_paths_in_report": False,
        },
    }
    return {"passed": all(checks.values()), "checks": checks}
