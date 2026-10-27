"""Build a leak-resistant HomeMind training manifest from AI Hub corpora."""

from __future__ import annotations

import csv
import heapq
import hashlib
import json
import sys
import wave
import zipfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable


ACTIVITY_WINDOW_SECONDS = 8.0
EVENT_MAX_SECONDS = 5.0
MANIFEST_FIELDS = (
    "path",
    "head",
    "label",
    "split",
    "start_seconds",
    "end_seconds",
    "group_id",
    "dataset",
    "source",
)

KNOWN_EXCLUDED_ARCHIVES = (
    "464_meeting/Training/TS3.zip",
    "464_meeting/Validation/VS1.zip",
    "71405_noisy_command/Training/TS_01.다화자_소음_환경_003.가정환경_02.근거리.zip",
    "71698_environment_sound/Training/TS_대화음성데이터_03.문화및라이프_01.주거지역.zip",
    "71698_environment_sound/Training/TS_대화음성데이터_04.게임및과학_01.주거지역.zip",
)


@dataclass(frozen=True)
class ManifestCandidate:
    path: Path
    head: str
    label: str
    split: str
    start_seconds: float
    end_seconds: float
    group_id: str
    dataset: str
    source: str

    def row(self) -> dict[str, str]:
        return {
            "path": str(self.path.resolve()),
            "head": self.head,
            "label": self.label,
            "split": self.split,
            "start_seconds": f"{self.start_seconds:.3f}",
            "end_seconds": f"{self.end_seconds:.3f}",
            "group_id": self.group_id,
            "dataset": self.dataset,
            "source": self.source,
        }


def stable_split(
    group_id: str,
    *,
    validation_fraction: float = 0.2,
    seed: int = 42,
) -> str:
    """Assign every segment from one original recording to the same split."""

    if not 0.0 < validation_fraction < 1.0:
        raise ValueError("validation_fraction must be between 0 and 1")
    digest = hashlib.sha256(f"{seed}:{group_id}".encode()).digest()
    value = int.from_bytes(digest[:8], "big") / 2**64
    return "validation" if value < validation_fraction else "train"


def _rank(candidate: ManifestCandidate, seed: int) -> bytes:
    key = (
        f"{seed}:{candidate.dataset}:{candidate.group_id}:"
        f"{candidate.start_seconds:.3f}:{candidate.label}"
    )
    return hashlib.sha256(key.encode()).digest()


def _source_split(path: Path) -> str:
    parts = {part.lower() for part in path.parts}
    return "validation" if "validation" in parts else "train"


def _wav_duration(path: Path) -> float:
    with wave.open(str(path), "rb") as handle:
        return handle.getnframes() / handle.getframerate()


def _activity_segments(
    duration: float,
    *,
    window_seconds: float = ACTIVITY_WINDOW_SECONDS,
    max_chunks_per_file: int = 4,
) -> list[tuple[float, float]]:
    if duration <= 0:
        return []
    if duration <= window_seconds:
        return [(0.0, duration)]
    available = max(1, int(duration // window_seconds))
    count = min(max_chunks_per_file, available)
    if count == 1:
        starts = [max(0.0, (duration - window_seconds) / 2)]
    else:
        last = duration - window_seconds
        starts = [last * index / (count - 1) for index in range(count)]
    return [(start, start + window_seconds) for start in starts]


def _json_documents(path: Path) -> Iterable[tuple[str, dict[str, Any]]]:
    with zipfile.ZipFile(path) as archive:
        for entry in archive.infolist():
            if not entry.filename.lower().endswith(".json"):
                continue
            try:
                document = json.loads(archive.read(entry).decode("utf-8-sig"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            yield Path(entry.filename).stem, document


def _find_label_zip(labels_root: Path, expected_name: str) -> Path | None:
    matches = sorted(labels_root.rglob(expected_name))
    return matches[0] if matches else None


def _sample_wav_paths(
    folder: Path,
    *,
    limit: int | None,
    seed: int,
    salt: str,
    recursive: bool = True,
) -> list[Path]:
    """Select reproducible paths without opening every WAV on a slow HDD."""

    paths = folder.rglob("*.wav") if recursive else folder.glob("*.wav")
    if limit is None:
        return sorted(paths)
    heap: list[tuple[int, str, Path]] = []
    for path in paths:
        digest = hashlib.sha256(f"{seed}:{salt}:{path}".encode()).digest()
        score = int.from_bytes(digest[:8], "big")
        item = (-score, str(path), path)
        if len(heap) < limit:
            heapq.heappush(heap, item)
        elif score < -heap[0][0]:
            heapq.heapreplace(heap, item)
    return sorted((item[2] for item in heap), key=str)


def _sample_grouped_wav_paths(
    folder: Path,
    *,
    limit: int,
    seed: int,
    salt: str,
) -> list[Path]:
    """Sample speaker/session folders before visiting their many short clips."""

    group_dirs = (
        child
        for archive_dir in folder.iterdir()
        if archive_dir.is_dir()
        for child in archive_dir.iterdir()
        if child.is_dir()
    )
    ranked_groups: list[tuple[bytes, Path]] = []
    for group_dir in group_dirs:
        rank = hashlib.sha256(f"{seed}:{salt}:{group_dir}".encode()).digest()
        ranked_groups.append((rank, group_dir))
    selected_groups = [
        path for _, path in sorted(ranked_groups, key=lambda item: item[0])[:limit]
    ]

    selected: list[Path] = []
    for group_dir in selected_groups:
        wavs = group_dir.glob("*.wav")
        try:
            chosen = min(
                wavs,
                key=lambda path: hashlib.sha256(
                    f"{seed}:{salt}:{path.name}".encode()
                ).digest(),
            )
        except ValueError:
            continue
        selected.append(chosen)
    return sorted(selected, key=str)


def _iter_activity_wavs(
    folder: Path,
    *,
    label: str,
    dataset: str,
    group_from_parent: bool = False,
    source: str,
    file_cap: int | None = None,
    seed: int = 42,
    group_stable_split: bool = False,
) -> Iterable[ManifestCandidate]:
    salt = f"{dataset}:{source}:{label}"
    if group_from_parent and file_cap is not None:
        wav_paths = _sample_grouped_wav_paths(
            folder, limit=file_cap, seed=seed, salt=salt
        )
    else:
        wav_paths = _sample_wav_paths(
            folder, limit=file_cap, seed=seed, salt=salt
        )
    for wav_path in wav_paths:
        try:
            duration = _wav_duration(wav_path)
        except (OSError, EOFError, wave.Error):
            continue
        group_name = wav_path.parent.name if group_from_parent else wav_path.stem
        group_id = f"{dataset}:{group_name}"
        split = (
            stable_split(group_id, seed=seed)
            if group_stable_split
            else _source_split(folder)
        )
        for start, end in _activity_segments(duration):
            yield ManifestCandidate(
                wav_path,
                "activity",
                label,
                split,
                start,
                end,
                group_id,
                dataset,
                source,
            )


def _meeting_candidates(
    dataset_root: Path,
    *,
    file_cap_per_label: int | None = None,
    seed: int = 42,
) -> Iterable[ManifestCandidate]:
    labels_root = dataset_root / "archives" / "labels"
    extracted = dataset_root / "extracted"
    for folder in sorted((extracted / "training").glob("TS*")):
        suffix = folder.name[2:]
        label_zip = _find_label_zip(labels_root, f"TL{suffix}.zip")
        if not label_zip:
            continue
        metadata_by_stem = {
            stem: document.get("metadata", {})
            for stem, document in _json_documents(label_zip)
        }
        paths_by_label: dict[str, list[Path]] = {}
        for wav_path in folder.rglob("*.wav"):
            metadata = metadata_by_stem.get(wav_path.stem)
            if not metadata:
                continue
            communication = str(metadata.get("communication", "")).strip()
            if communication == "대면":
                label = "IN_PERSON_MEETING"
            elif communication == "비대면":
                label = "ZOOM_SPEAKER"
            else:
                continue
            paths_by_label.setdefault(label, []).append(wav_path)
        selected_paths: list[Path] = []
        for label, paths in paths_by_label.items():
            ranked = sorted(
                paths,
                key=lambda path: hashlib.sha256(
                    f"{seed}:464:{folder.name}:{label}:{path}".encode()
                ).digest(),
            )
            selected_paths.extend(
                ranked if file_cap_per_label is None else ranked[:file_cap_per_label]
            )
        for wav_path in sorted(selected_paths):
            metadata = metadata_by_stem[wav_path.stem]
            communication = str(metadata.get("communication", "")).strip()
            label = (
                "IN_PERSON_MEETING"
                if communication == "대면"
                else "ZOOM_SPEAKER"
            )
            try:
                duration = _wav_duration(wav_path)
            except (OSError, EOFError, wave.Error):
                continue
            group_id = f"464:{wav_path.stem}"
            split = stable_split(group_id)
            for start, end in _activity_segments(duration):
                yield ManifestCandidate(
                    wav_path,
                    "activity",
                    label,
                    split,
                    start,
                    end,
                    group_id,
                    "464_meeting",
                    f"{folder.name}/{communication}",
                )


def _conversation_group(stem: str) -> str:
    if stem.endswith(("_VN", "_VV")):
        return stem[:-3]
    return stem


def _environment_event_label(document: dict[str, Any]) -> str:
    text = " ".join(
        str(document.get(field, ""))
        for field in ("Category", "SubCategory", "SoundComment")
    )
    return "ALARM" if any(word in text for word in ("경보", "알람", "사이렌")) else "OTHER"


def _event_bounds(document: dict[str, Any], duration: float) -> tuple[float, float]:
    spans = document.get("AudioInfo") or []
    starts = [float(span.get("StartTime", 0)) for span in spans]
    ends = [float(span.get("EndTime", 0)) for span in spans]
    if not starts or not ends:
        return 0.0, min(duration, EVENT_MAX_SECONDS)
    start = max(0.0, min(starts) - 0.5)
    end = min(duration, max(ends) + 0.5)
    if end - start > EVENT_MAX_SECONDS:
        center = (start + end) / 2
        start = max(0.0, center - EVENT_MAX_SECONDS / 2)
        end = min(duration, start + EVENT_MAX_SECONDS)
    return start, end


def _environment_candidates(
    dataset_root: Path,
    *,
    file_cap_per_label: int | None = None,
    seed: int = 42,
) -> Iterable[ManifestCandidate]:
    extracted = dataset_root / "extracted"
    labels_root = dataset_root / "archives" / "labels"
    for split_name in ("training", "validation"):
        split_root = extracted / split_name
        if not split_root.is_dir():
            continue
        for folder in sorted(split_root.glob("*환경소리데이터*")):
            label_prefix = "VL" if split_name == "validation" else "TL"
            source_prefix = "VS" if split_name == "validation" else "TS"
            expected = label_prefix + folder.name[len(source_prefix) :] + ".zip"
            label_zip = _find_label_zip(labels_root, expected)
            if not label_zip:
                continue
            metadata_by_stem = dict(_json_documents(label_zip))
            paths_by_label: dict[str, list[Path]] = {}
            for wav_path in folder.glob("*.wav"):
                document = metadata_by_stem.get(wav_path.stem)
                if not document:
                    continue
                label = _environment_event_label(document)
                paths_by_label.setdefault(label, []).append(wav_path)
            selected_paths: list[Path] = []
            for label, paths in paths_by_label.items():
                ranked = sorted(
                    paths,
                    key=lambda path: hashlib.sha256(
                        f"{seed}:{folder.name}:{label}:{path}".encode()
                    ).digest(),
                )
                selected_paths.extend(
                    ranked
                    if file_cap_per_label is None
                    else ranked[:file_cap_per_label]
                )
            for wav_path in sorted(selected_paths):
                document = metadata_by_stem[wav_path.stem]
                try:
                    duration = _wav_duration(wav_path)
                except (OSError, EOFError, wave.Error):
                    continue
                start, end = _event_bounds(document, duration)
                if end <= start:
                    continue
                yield ManifestCandidate(
                    wav_path,
                    "sound_event",
                    _environment_event_label(document),
                    "validation" if split_name == "validation" else "train",
                    start,
                    end,
                    f"71698:event:{wav_path.stem}",
                    "71698_environment_sound",
                    folder.name,
                )


def iter_aihub_candidates(
    root: str | Path,
    *,
    file_cap_per_source: int | None = None,
    seed: int = 42,
    progress: Callable[[str], None] | None = None,
) -> Iterable[ManifestCandidate]:
    root_path = Path(root)
    report_progress = progress or (lambda _message: None)

    meeting = root_path / "464_meeting"
    if meeting.is_dir():
        report_progress("Indexing 464 meeting audio")
        yield from _meeting_candidates(
            meeting, file_cap_per_label=file_cap_per_source, seed=seed
        )

    broadcast = root_path / "463_broadcast" / "extracted"
    if broadcast.is_dir():
        report_progress("Indexing 463 broadcast audio")
        for split_name in ("training", "validation"):
            folder = broadcast / split_name
            if folder.is_dir():
                yield from _iter_activity_wavs(
                    folder,
                    label="TV_MEDIA",
                    dataset="463_broadcast",
                    source=f"broadcast/{split_name}",
                    file_cap=file_cap_per_source,
                    seed=seed,
                )

    conversation = root_path / "109_free_conversation" / "extracted"
    if conversation.is_dir():
        report_progress("Indexing 109 free-conversation audio")
        for split_name in ("training", "validation"):
            folder = conversation / split_name
            if folder.is_dir():
                yield from _iter_activity_wavs(
                    folder,
                    label="SELF_TALK",
                    dataset="109_free_conversation",
                    group_from_parent=True,
                    source=f"free_conversation/{split_name}",
                    file_cap=file_cap_per_source,
                    seed=seed,
                    group_stable_split=True,
                )

    environment = root_path / "71698_environment_sound"
    if environment.is_dir():
        report_progress("Indexing 71698 dialogue and environmental audio")
        dialogue_root = environment / "extracted"
        for split_name in ("training", "validation"):
            split_root = dialogue_root / split_name
            if not split_root.is_dir():
                continue
            for folder in sorted(split_root.glob("*대화음성데이터*")):
                wav_paths = _sample_wav_paths(
                    folder,
                    limit=file_cap_per_source,
                    seed=seed,
                    salt=f"71698:dialogue:{split_name}:{folder.name}",
                    recursive=False,
                )
                for wav_path in wav_paths:
                    try:
                        duration = _wav_duration(wav_path)
                    except (OSError, EOFError, wave.Error):
                        continue
                    group_id = f"71698:dialogue:{_conversation_group(wav_path.stem)}"
                    for start, end in _activity_segments(duration):
                        yield ManifestCandidate(
                            wav_path,
                            "activity",
                            "UNKNOWN",
                            "validation" if split_name == "validation" else "train",
                            start,
                            end,
                            group_id,
                            "71698_environment_sound",
                            folder.name,
                        )
        yield from _environment_candidates(
            environment,
            file_cap_per_label=file_cap_per_source,
            seed=seed,
        )


def collect_aihub_candidates(
    root: str | Path,
    *,
    file_cap_per_source: int | None = None,
    seed: int = 42,
    progress: Callable[[str], None] | None = None,
) -> list[ManifestCandidate]:
    return list(
        iter_aihub_candidates(
            root,
            file_cap_per_source=file_cap_per_source,
            seed=seed,
            progress=progress,
        )
    )


def _balanced_selection(
    candidates: list[ManifestCandidate],
    *,
    max_per_label: int | None,
    seed: int,
) -> list[ManifestCandidate]:
    if max_per_label is None:
        return sorted(candidates, key=lambda item: (item.head, item.label, _rank(item, seed)))
    if max_per_label < 1:
        raise ValueError("max_per_label must be positive")
    buckets: dict[tuple[str, str, str], list[ManifestCandidate]] = {}
    for candidate in candidates:
        buckets.setdefault(
            (candidate.head, candidate.label, candidate.split), []
        ).append(candidate)
    selected: list[ManifestCandidate] = []
    for bucket in buckets.values():
        selected.extend(sorted(bucket, key=lambda item: _rank(item, seed))[:max_per_label])
    return sorted(selected, key=lambda item: (item.head, item.label, item.split, _rank(item, seed)))


def build_aihub_manifest(
    root: str | Path,
    output: str | Path,
    *,
    max_per_label: int | None = 500,
    seed: int = 42,
    verbose: bool = False,
) -> dict[str, Any]:
    root_path = Path(root).resolve()
    output_path = Path(output).resolve()
    file_cap = None if max_per_label is None else max(100, max_per_label * 2)
    progress = (
        (lambda message: print(message, file=sys.stderr, flush=True))
        if verbose
        else None
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if max_per_label is None:
        candidate_counts: Counter[str] = Counter()
        candidate_count = 0
        temporary_path = output_path.with_name(f"{output_path.name}.tmp")
        with temporary_path.open("w", encoding="utf-8-sig", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=MANIFEST_FIELDS)
            writer.writeheader()
            for candidate in iter_aihub_candidates(
                root_path,
                file_cap_per_source=None,
                seed=seed,
                progress=progress,
            ):
                writer.writerow(candidate.row())
                candidate_count += 1
                candidate_counts[
                    f"{candidate.head}/{candidate.label}/{candidate.split}"
                ] += 1
                if verbose and candidate_count % 10_000 == 0:
                    print(
                        f"Indexed {candidate_count:,} manifest rows",
                        file=sys.stderr,
                        flush=True,
                    )
        temporary_path.replace(output_path)
        selected_count = candidate_count
        selected_counts = candidate_counts.copy()
    else:
        candidates = collect_aihub_candidates(
            root_path,
            file_cap_per_source=file_cap,
            seed=seed,
            progress=progress,
        )
        selected = _balanced_selection(
            candidates, max_per_label=max_per_label, seed=seed
        )
        with output_path.open("w", encoding="utf-8-sig", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=MANIFEST_FIELDS)
            writer.writeheader()
            writer.writerows(candidate.row() for candidate in selected)
        candidate_count = len(candidates)
        selected_count = len(selected)
        candidate_counts = Counter(
            f"{item.head}/{item.label}/{item.split}" for item in candidates
        )
        selected_counts = Counter(
            f"{item.head}/{item.label}/{item.split}" for item in selected
        )
    report: dict[str, Any] = {
        "root": str(root_path),
        "manifest": str(output_path),
        "seed": seed,
        "max_per_label_per_split": max_per_label,
        "candidate_count": candidate_count,
        "selected_count": selected_count,
        "candidate_counts": dict(sorted(candidate_counts.items())),
        "selected_counts": dict(sorted(selected_counts.items())),
        "known_excluded_archives": list(KNOWN_EXCLUDED_ARCHIVES),
        "reserved_for_partial_intent": ["71405_noisy_command"],
        "uncovered_activity_labels": ["QUIET", "ZOOM_HEADPHONES"],
        "uncovered_sound_event_labels": ["DOORBELL", "WASHER_COMPLETE"],
        "split_rule": (
            "official split; datasets 464 and 109 use SHA-256 recording/speaker "
            "group holdout to prevent leakage"
        ),
        "indexing": (
            "full corpus"
            if file_cap is None
            else f"deterministic file pre-sample, up to {file_cap} per source"
        ),
    }
    report_path = output_path.with_suffix(".report.json")
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report
