"""Safe, resumable processing for large AI-Hub source ZIP archives."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import zipfile
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable


DELETE_CONFIRMATION = "DELETE_VERIFIED_ORIGINALS"
MARKER_NAME = ".homemind-extracted.json"


class ArchiveIntegrityError(Exception):
    """The archive or one of its members failed content verification."""


@dataclass(frozen=True)
class ArchiveInfo:
    path: str
    dataset: str
    split: str
    archive_bytes: int
    members: int
    wav_members: int
    json_members: int
    uncompressed_bytes: int
    valid: bool
    error: str | None = None


def _relative_member_name(name: str) -> Path:
    normalized = name.replace("\\", "/").lstrip("/")
    parts = PurePosixPath(normalized).parts
    if not parts or any(part in {"", ".", ".."} for part in parts):
        raise ValueError(f"Unsafe ZIP member path: {name!r}")
    return Path(*parts)


def _split_for(path: Path) -> str:
    # AI-Hub downloads can be placed below an outer staging directory whose
    # name also contains ``training`` or ``validation``.  The archive's own
    # nearest split directory (for example ``1.Training``) is authoritative.
    for part in reversed(path.parts):
        lowered = part.lower()
        if "validation" in lowered:
            return "validation"
        if "training" in lowered:
            return "training"
    return "unknown"


def _dataset_for(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).parts[0]
    except (ValueError, IndexError):
        return path.parent.name


def discover_source_archives(root: str | Path) -> list[Path]:
    root_path = Path(root).resolve()
    archives: list[Path] = []
    for path in root_path.rglob("*.zip"):
        lowered_parts = {part.lower() for part in path.parts}
        if "source" in lowered_parts:
            archives.append(path.resolve())
    return sorted(archives, key=lambda item: str(item).casefold())


def inspect_archive(path: str | Path, *, root: str | Path) -> ArchiveInfo:
    archive_path = Path(path).resolve()
    root_path = Path(root).resolve()
    try:
        with zipfile.ZipFile(archive_path) as archive:
            members = [entry for entry in archive.infolist() if not entry.is_dir()]
            for entry in members:
                _relative_member_name(entry.filename)
            return ArchiveInfo(
                path=str(archive_path),
                dataset=_dataset_for(archive_path, root_path),
                split=_split_for(archive_path),
                archive_bytes=archive_path.stat().st_size,
                members=len(members),
                wav_members=sum(
                    entry.filename.lower().endswith((".wav", ".wave"))
                    for entry in members
                ),
                json_members=sum(
                    entry.filename.lower().endswith(".json") for entry in members
                ),
                uncompressed_bytes=sum(entry.file_size for entry in members),
                valid=True,
            )
    except Exception as exc:
        return ArchiveInfo(
            path=str(archive_path),
            dataset=_dataset_for(archive_path, root_path),
            split=_split_for(archive_path),
            archive_bytes=archive_path.stat().st_size if archive_path.exists() else 0,
            members=0,
            wav_members=0,
            json_members=0,
            uncompressed_bytes=0,
            valid=False,
            error=f"{type(exc).__name__}: {exc}",
        )


def scan_archives(root: str | Path) -> dict[str, object]:
    root_path = Path(root).resolve()
    archives = [
        inspect_archive(path, root=root_path)
        for path in discover_source_archives(root_path)
    ]
    return {
        "root": str(root_path),
        "archive_count": len(archives),
        "valid_count": sum(item.valid for item in archives),
        "invalid_count": sum(not item.valid for item in archives),
        "archive_bytes": sum(item.archive_bytes for item in archives),
        "uncompressed_bytes": sum(item.uncompressed_bytes for item in archives),
        "archives": [asdict(item) for item in archives],
    }


def verify_archive_crc(path: str | Path, *, root: str | Path) -> dict[str, object]:
    """Read every compressed member and verify its CRC without extracting it."""
    archive_path = Path(path).resolve()
    root_path = Path(root).resolve()
    result: dict[str, object] = {
        "archive": str(archive_path),
        "dataset": _dataset_for(archive_path, root_path),
        "split": _split_for(archive_path),
        "archive_bytes": archive_path.stat().st_size if archive_path.exists() else 0,
        "status": "invalid",
        "bad_member": None,
        "error": None,
    }
    try:
        with zipfile.ZipFile(archive_path) as archive:
            for entry in archive.infolist():
                if not entry.is_dir():
                    _relative_member_name(entry.filename)
            bad_member = archive.testzip()
        if bad_member is not None:
            result["bad_member"] = bad_member
            result["error"] = f"Bad CRC-32 for file {bad_member!r}"
        else:
            result["status"] = "verified_crc"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    return result


def verify_selected_archives(
    archives: Iterable[Path], *, root: str | Path
) -> dict[str, object]:
    """Fully CRC-check all selected archives, continuing after failures."""
    selected = list(archives)
    results: list[dict[str, object]] = []
    for index, archive in enumerate(selected, start=1):
        print(
            f"[{index}/{len(selected)}] CRC checking {archive.name}",
            file=sys.stderr,
            flush=True,
        )
        result = verify_archive_crc(archive, root=root)
        results.append(result)
        print(
            f"[{index}/{len(selected)}] {result['status']}: {archive.name}",
            file=sys.stderr,
            flush=True,
        )
    invalid = [item for item in results if item["status"] != "verified_crc"]
    return {
        "processed": len(results),
        "verified_count": len(results) - len(invalid),
        "invalid_count": len(invalid),
        "invalid_archives": invalid,
        "results": results,
    }


def select_archives(
    root: str | Path,
    *,
    dataset: str | None = None,
    names: Iterable[str] = (),
    all_archives: bool = False,
) -> list[Path]:
    root_path = Path(root).resolve()
    candidates = discover_source_archives(root_path)
    if dataset:
        candidates = [
            path for path in candidates if _dataset_for(path, root_path) == dataset
        ]

    requested = set(names)
    if requested:
        selected = [path for path in candidates if path.name in requested]
        found = {path.name for path in selected}
        missing = sorted(requested - found)
        if missing:
            raise FileNotFoundError(f"Archive names not found: {missing}")
        return selected
    if all_archives:
        return candidates
    raise ValueError("Specify at least one --archive name or pass --all")


def destination_for(path: Path, *, root: str | Path) -> Path:
    root_path = Path(root).resolve()
    dataset = _dataset_for(path, root_path)
    split = _split_for(path)
    return root_path / dataset / "extracted" / split / path.stem


def _marker_matches(marker: Path, archive_path: Path) -> bool:
    try:
        payload = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return (
        payload.get("archive") == str(archive_path)
        and payload.get("archive_bytes") == archive_path.stat().st_size
        and payload.get("status") in {"verified", "verified_archive_deleted"}
    )


def _check_wav_headers(paths: list[Path], limit: int) -> tuple[int, list[str]]:
    if not paths or limit <= 0:
        return 0, []
    if len(paths) <= limit:
        selected = paths
    else:
        selected = [
            paths[round(index * (len(paths) - 1) / (limit - 1))]
            for index in range(limit)
        ] if limit > 1 else [paths[0]]

    errors: list[str] = []
    for path in selected:
        try:
            with path.open("rb") as handle:
                header = handle.read(12)
            if len(header) != 12 or header[:4] not in {b"RIFF", b"RF64"} or header[8:] != b"WAVE":
                errors.append(f"Invalid WAV header: {path}")
        except OSError as exc:
            errors.append(f"{path}: {exc}")
    return len(selected), errors


def _resolve_bandizip(path: str | Path | None) -> Path:
    candidates = []
    if path is not None:
        candidates.append(Path(path))
    discovered = shutil.which("bz")
    if discovered:
        candidates.append(Path(discovered))
    candidates.extend(
        [
            Path(r"C:\Program Files\Bandizip\bz.exe"),
            Path(r"C:\Program Files (x86)\Bandizip\bz.exe"),
        ]
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError(
        "Bandizip console tool bz.exe was not found; pass --bandizip-path"
    )


def extract_archive(
    archive: str | Path,
    *,
    root: str | Path,
    wav_check_limit: int = 100,
    minimum_free_bytes: int = 20 * 1024**3,
    delete_archive_after_verify: bool = False,
    delete_confirmation: str | None = None,
    staging_root: str | Path | None = None,
    extractor: str = "python",
    bandizip_path: str | Path | None = None,
) -> dict[str, object]:
    archive_path = Path(archive).resolve()
    root_path = Path(root).resolve()
    if extractor not in {"python", "bandizip"}:
        raise ValueError(f"Unsupported extractor: {extractor}")
    if delete_archive_after_verify and delete_confirmation != DELETE_CONFIRMATION:
        raise ValueError(
            "Deleting originals requires "
            f"--confirm-delete-originals {DELETE_CONFIRMATION}"
        )

    info = inspect_archive(archive_path, root=root_path)
    if not info.valid:
        raise ArchiveIntegrityError(info.error or f"Invalid ZIP: {archive_path}")

    destination = destination_for(archive_path, root=root_path)
    marker = destination / MARKER_NAME
    if destination.exists():
        if marker.is_file() and _marker_matches(marker, archive_path):
            result = {
                "archive": str(archive_path),
                "destination": str(destination),
                "status": "already_verified",
                "archive_deleted": False,
            }
            if delete_archive_after_verify:
                archive_path.unlink()
                result["status"] = "verified_archive_deleted"
                result["archive_deleted"] = True
                marker.write_text(
                    json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
            return result
        raise FileExistsError(
            f"Destination exists without a matching verification marker: {destination}"
        )

    external_staging = staging_root is not None
    if external_staging:
        staging_base = Path(staging_root).resolve()
        staging_base.mkdir(parents=True, exist_ok=True)
        staging = (
            staging_base
            / info.dataset
            / info.split
            / f"{destination.name}.partial"
        )
        transfer = destination.with_name(f"{destination.name}.transfer-partial")
    else:
        staging = destination.with_name(f"{destination.name}.partial")
        transfer = None
    if staging.exists():
        raise FileExistsError(
            f"Partial extraction already exists; inspect it before retrying: {staging}"
        )
    required_bytes = info.uncompressed_bytes + minimum_free_bytes
    destination_free_bytes = shutil.disk_usage(root_path).free
    if destination_free_bytes < required_bytes:
        raise OSError(
            f"Not enough destination space for {archive_path.name}: "
            f"need {required_bytes} bytes including reserve, "
            f"have {destination_free_bytes} bytes"
        )
    if external_staging:
        staging_free_bytes = shutil.disk_usage(staging_base).free
        if staging_free_bytes < required_bytes:
            raise OSError(
                f"Not enough staging space for {archive_path.name}: "
                f"need {required_bytes} bytes including reserve, "
                f"have {staging_free_bytes} bytes"
            )
        if transfer is not None and transfer.exists():
            raise FileExistsError(
                f"Partial transfer already exists; inspect it before retrying: {transfer}"
            )
    staging.mkdir(parents=True)
    if external_staging:
        print(
            f"  extracting_to_staging: {archive_path.name} -> {staging}",
            file=sys.stderr,
            flush=True,
        )

    extracted_paths: list[Path] = []
    seen_names: set[str] = set()
    extracted_bytes = 0
    try:
        try:
            with zipfile.ZipFile(archive_path) as source:
                entries = [entry for entry in source.infolist() if not entry.is_dir()]
                prepared_entries: list[tuple[zipfile.ZipInfo, Path]] = []
                for entry in entries:
                    try:
                        relative = _relative_member_name(entry.filename)
                    except ValueError as exc:
                        raise ArchiveIntegrityError(str(exc)) from exc
                    key = str(relative).casefold()
                    if key in seen_names:
                        raise ArchiveIntegrityError(
                            f"ZIP members collide after normalization: {relative}"
                        )
                    seen_names.add(key)
                    prepared_entries.append((entry, relative))

                if extractor == "python":
                    for entry, relative in prepared_entries:
                        target = staging / relative
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with source.open(entry) as input_handle, target.open(
                            "wb"
                        ) as output_handle:
                            shutil.copyfileobj(
                                input_handle, output_handle, length=1024 * 1024
                            )
                        if target.stat().st_size != entry.file_size:
                            raise IOError(
                                f"Extracted size mismatch for {entry.filename}: "
                                f"{target.stat().st_size} != {entry.file_size}"
                            )
                        extracted_bytes += entry.file_size
                        extracted_paths.append(target)
                else:
                    executable = _resolve_bandizip(bandizip_path)
                    print(
                        f"  bandizip_extracting: {archive_path.name}",
                        file=sys.stderr,
                        flush=True,
                    )
                    process = subprocess.run(
                        [
                            str(executable),
                            "x",
                            "-aoa",
                            "-y",
                            f"-o:{staging}",
                            str(archive_path),
                        ],
                        check=False,
                    )
                    if process.returncode != 0:
                        crc_result = verify_archive_crc(archive_path, root=root_path)
                        if crc_result["status"] != "verified_crc":
                            raise ArchiveIntegrityError(
                                str(crc_result.get("error") or "Bandizip CRC failure")
                            )
                        raise RuntimeError(
                            f"Bandizip failed with exit code {process.returncode} "
                            "although the ZIP passed CRC verification"
                        )
                    for entry, relative in prepared_entries:
                        target = staging / relative
                        if not target.is_file():
                            raise IOError(f"Bandizip output is missing: {target}")
                        if target.stat().st_size != entry.file_size:
                            raise IOError(
                                f"Bandizip size mismatch for {entry.filename}: "
                                f"{target.stat().st_size} != {entry.file_size}"
                            )
                        extracted_bytes += entry.file_size
                        extracted_paths.append(target)
        except (zipfile.BadZipFile, EOFError, zlib.error) as exc:
            raise ArchiveIntegrityError(f"{type(exc).__name__}: {exc}") from exc

        wav_paths = [
            path for path in extracted_paths if path.suffix.lower() in {".wav", ".wave"}
        ]
        checked_wavs, wav_errors = _check_wav_headers(wav_paths, wav_check_limit)
        if wav_errors:
            raise ArchiveIntegrityError("; ".join(wav_errors[:10]))

        report = {
            "archive": str(archive_path),
            "archive_bytes": archive_path.stat().st_size,
            "destination": str(destination),
            "status": "verified",
            "members": len(extracted_paths),
            "wav_members": len(wav_paths),
            "wav_headers_checked": checked_wavs,
            "uncompressed_bytes": extracted_bytes,
            "archive_deleted": False,
            "staging_root": str(Path(staging_root).resolve()) if external_staging else None,
            "extractor": extractor,
        }
        if external_staging:
            assert transfer is not None
            print(
                f"  transferring_to_destination: {archive_path.name} -> {destination}",
                file=sys.stderr,
                flush=True,
            )
            transfer.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(staging, transfer)
            for source_path in extracted_paths:
                relative = source_path.relative_to(staging)
                copied_path = transfer / relative
                if not copied_path.is_file():
                    raise IOError(f"Transferred file is missing: {copied_path}")
                if copied_path.stat().st_size != source_path.stat().st_size:
                    raise IOError(
                        f"Transferred size mismatch for {copied_path}: "
                        f"{copied_path.stat().st_size} != {source_path.stat().st_size}"
                    )
            transferred_wavs = [
                transfer / path.relative_to(staging) for path in wav_paths
            ]
            _, transfer_wav_errors = _check_wav_headers(
                transferred_wavs, wav_check_limit
            )
            if transfer_wav_errors:
                raise IOError("; ".join(transfer_wav_errors[:10]))
            (transfer / MARKER_NAME).write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            transfer.rename(destination)
        else:
            (staging / MARKER_NAME).write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            staging.rename(destination)

        if delete_archive_after_verify:
            archive_path.unlink()
            report["archive_deleted"] = True
            report["status"] = "verified_archive_deleted"
            (destination / MARKER_NAME).write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        if external_staging:
            try:
                shutil.rmtree(staging)
            except OSError as exc:
                report["staging_cleanup_error"] = f"{type(exc).__name__}: {exc}"
        return report
    except ArchiveIntegrityError:
        # External staging contains only disposable output generated by this run.
        # Keep the original ZIP, but reclaim C: so later archives can continue.
        if external_staging and staging.exists():
            shutil.rmtree(staging)
        raise
    except Exception:
        # Preserve partial data for storage/copy failures so it can be inspected.
        raise


def extract_selected_archives(
    archives: Iterable[Path],
    *,
    root: str | Path,
    wav_check_limit: int = 100,
    minimum_free_bytes: int = 20 * 1024**3,
    delete_archive_after_verify: bool = False,
    delete_confirmation: str | None = None,
    staging_root: str | Path | None = None,
    continue_on_archive_error: bool = False,
    extractor: str = "python",
    bandizip_path: str | Path | None = None,
) -> dict[str, object]:
    if delete_archive_after_verify and delete_confirmation != DELETE_CONFIRMATION:
        raise ValueError(
            "Deleting originals requires "
            f"--confirm-delete-originals {DELETE_CONFIRMATION}"
        )
    results: list[dict[str, object]] = []
    selected = list(archives)
    for index, archive in enumerate(selected, start=1):
        print(
            f"[{index}/{len(selected)}] Processing {archive.name}",
            file=sys.stderr,
            flush=True,
        )
        try:
            result = extract_archive(
                archive,
                root=root,
                wav_check_limit=wav_check_limit,
                minimum_free_bytes=minimum_free_bytes,
                delete_archive_after_verify=delete_archive_after_verify,
                delete_confirmation=delete_confirmation,
                staging_root=staging_root,
                extractor=extractor,
                bandizip_path=bandizip_path,
            )
        except ArchiveIntegrityError as exc:
            if not continue_on_archive_error:
                raise
            result = {
                "archive": str(archive),
                "status": "invalid_archive_retained",
                "archive_deleted": False,
                "error": f"{type(exc).__name__}: {exc}",
            }
        results.append(result)
        print(
            f"[{index}/{len(selected)}] {result['status']}: {archive.name}",
            file=sys.stderr,
            flush=True,
        )
    invalid = [
        result for result in results
        if result["status"] == "invalid_archive_retained"
    ]
    return {
        "processed": len(results),
        "successful_count": len(results) - len(invalid),
        "invalid_count": len(invalid),
        "invalid_archives": invalid,
        "results": results,
    }
