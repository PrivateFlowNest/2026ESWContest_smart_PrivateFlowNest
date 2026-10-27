from __future__ import annotations

import io
import json
import wave
import zipfile
from pathlib import Path

import pytest

from homemind_audio.archive_processing import (
    DELETE_CONFIRMATION,
    MARKER_NAME,
    destination_for,
    extract_archive,
    extract_selected_archives,
    scan_archives,
    verify_selected_archives,
)


def _wav_bytes() -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(16_000)
        writer.writeframes(b"\x00\x00" * 16_000)
    return output.getvalue()


def _archive(root: Path, member: str = "/audio/test.wav") -> Path:
    path = (
        root
        / "demo_dataset"
        / "archives"
        / "source"
        / "Training"
        / "TS1.zip"
    )
    path.parent.mkdir(parents=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(member, _wav_bytes())
        archive.writestr("metadata/test.json", json.dumps({"ok": True}))
    return path


def test_scan_and_extract_archive_safely(tmp_path):
    archive = _archive(tmp_path)

    scan = scan_archives(tmp_path)
    result = extract_archive(archive, root=tmp_path)
    destination = Path(result["destination"])

    assert scan["archive_count"] == 1
    assert scan["invalid_count"] == 0
    assert (destination / "audio" / "test.wav").is_file()
    assert (destination / "metadata" / "test.json").is_file()
    assert (destination / MARKER_NAME).is_file()
    assert archive.is_file()

    repeated = extract_archive(archive, root=tmp_path)
    assert repeated["status"] == "already_verified"


def test_rejects_path_traversal(tmp_path):
    archive = _archive(tmp_path, member="../outside.wav")

    result = scan_archives(tmp_path)

    assert result["invalid_count"] == 1
    assert "Unsafe ZIP member path" in result["archives"][0]["error"]


def test_deleting_archive_requires_explicit_confirmation(tmp_path):
    archive = _archive(tmp_path)

    with pytest.raises(ValueError, match="Deleting originals requires"):
        extract_archive(
            archive,
            root=tmp_path,
            delete_archive_after_verify=True,
        )

    result = extract_archive(
        archive,
        root=tmp_path,
        delete_archive_after_verify=True,
        delete_confirmation=DELETE_CONFIRMATION,
    )
    assert result["archive_deleted"] is True
    assert not archive.exists()


def test_can_delete_archive_after_an_existing_verified_extraction(tmp_path):
    archive = _archive(tmp_path)
    extract_archive(archive, root=tmp_path)

    result = extract_archive(
        archive,
        root=tmp_path,
        delete_archive_after_verify=True,
        delete_confirmation=DELETE_CONFIRMATION,
    )

    assert result["status"] == "verified_archive_deleted"
    assert result["archive_deleted"] is True
    assert not archive.exists()


def test_nearest_split_directory_wins_over_outer_staging_name(tmp_path):
    archive = (
        tmp_path
        / "demo_dataset"
        / "archives"
        / "source"
        / "validation"
        / "01.data"
        / "1.Training"
        / "source_data"
        / "TS3.zip"
    )

    destination = destination_for(archive, root=tmp_path)

    assert destination == (
        tmp_path / "demo_dataset" / "extracted" / "training" / "TS3"
    )


def test_crc_verification_continues_and_reports_corrupt_archive(tmp_path):
    valid = _archive(tmp_path)
    corrupt = valid.with_name("TS2.zip")
    with zipfile.ZipFile(corrupt, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.writestr("audio/corrupt.wav", b"known-payload")
    payload = corrupt.read_bytes()
    corrupt.write_bytes(payload.replace(b"known-payload", b"brokenpayload", 1))

    result = verify_selected_archives([corrupt, valid], root=tmp_path)

    assert result["processed"] == 2
    assert result["verified_count"] == 1
    assert result["invalid_count"] == 1
    assert result["invalid_archives"][0]["archive"] == str(corrupt.resolve())
    assert result["invalid_archives"][0]["bad_member"] == "audio/corrupt.wav"


def test_external_staging_transfers_then_deletes_source(tmp_path):
    archive = _archive(tmp_path)
    staging_root = tmp_path / "fast-ssd-staging"

    result = extract_archive(
        archive,
        root=tmp_path,
        staging_root=staging_root,
        delete_archive_after_verify=True,
        delete_confirmation=DELETE_CONFIRMATION,
    )

    destination = Path(result["destination"])
    assert result["status"] == "verified_archive_deleted"
    assert (destination / "audio" / "test.wav").is_file()
    assert not archive.exists()
    assert not list(staging_root.rglob("*.partial"))


def test_external_staging_continues_after_crc_failure(tmp_path):
    valid = _archive(tmp_path)
    corrupt = valid.with_name("TS0.zip")
    with zipfile.ZipFile(corrupt, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.writestr("audio/corrupt.wav", b"known-payload")
    payload = corrupt.read_bytes()
    corrupt.write_bytes(payload.replace(b"known-payload", b"brokenpayload", 1))
    staging_root = tmp_path / "fast-ssd-staging"

    result = extract_selected_archives(
        [corrupt, valid],
        root=tmp_path,
        staging_root=staging_root,
        continue_on_archive_error=True,
        delete_archive_after_verify=True,
        delete_confirmation=DELETE_CONFIRMATION,
    )

    assert result["processed"] == 2
    assert result["successful_count"] == 1
    assert result["invalid_count"] == 1
    assert corrupt.exists()
    assert not valid.exists()
    assert not list(staging_root.rglob("*.partial"))


@pytest.mark.skipif(
    not Path(r"C:\Program Files\Bandizip\bz.exe").is_file(),
    reason="Bandizip console tool is not installed",
)
def test_bandizip_external_staging_integration(tmp_path):
    archive = _archive(tmp_path)
    staging_root = tmp_path / "bandizip-staging"

    result = extract_archive(
        archive,
        root=tmp_path,
        staging_root=staging_root,
        extractor="bandizip",
        bandizip_path=Path(r"C:\Program Files\Bandizip\bz.exe"),
    )

    destination = Path(result["destination"])
    assert result["extractor"] == "bandizip"
    assert (destination / "audio" / "test.wav").is_file()
    assert (destination / "metadata" / "test.json").is_file()


@pytest.mark.skipif(
    not Path(r"C:\Program Files\Bandizip\bz.exe").is_file(),
    reason="Bandizip console tool is not installed",
)
def test_bandizip_continues_after_real_crc_failure(tmp_path):
    valid = _archive(tmp_path)
    corrupt = valid.with_name("TS0.zip")
    with zipfile.ZipFile(corrupt, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.writestr("audio/corrupt.wav", b"known-payload")
    payload = corrupt.read_bytes()
    corrupt.write_bytes(payload.replace(b"known-payload", b"brokenpayload", 1))

    result = extract_selected_archives(
        [corrupt, valid],
        root=tmp_path,
        staging_root=tmp_path / "bandizip-staging",
        extractor="bandizip",
        bandizip_path=Path(r"C:\Program Files\Bandizip\bz.exe"),
        continue_on_archive_error=True,
    )

    assert result["invalid_count"] == 1
    assert result["successful_count"] == 1
    assert corrupt.exists()
    assert valid.exists()
