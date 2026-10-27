"""QR pairing + bearer token tests for the Python Mobile API (PC, temp dirs, random secrets; no device)."""

from __future__ import annotations

import hashlib
import json
import os
import re
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app import cli, main
from app.auth import (
    MAX_CODE_TTL_S,
    AuthConfig,
    PairingService,
    TokenStore,
    bearer_token,
    clean_device_name,
    format_code,
    load_pairing_secret,
    make_nonce,
    nonce_expiry,
    normalize_code,
    pairing_code,
)

BASE_URL = "http://192.0.2.10:8000"
POSIX = os.name == "posix"


@pytest.fixture()
def service(monkeypatch, tmp_path) -> PairingService:
    config = AuthConfig(data_dir=tmp_path / "mobile-api", public_base_url=BASE_URL, max_failures_per_client=3,
                        max_failures_global=5, failure_window_s=60)
    pairing = PairingService(config)
    pairing.prepare()
    monkeypatch.setattr(main, "auth", pairing)
    return pairing


def _claim(client: TestClient, offer, **overrides):
    body = {"code": offer.code, "nonce": offer.nonce, "device_name": "Galaxy"}
    body.update(overrides)
    return client.post("/api/v1/pairing/claim", json=body)


def test_code_matches_the_cpp_vector() -> None:
    # native/services/homemind-mobile-api/tests/test_mobile_api.cpp Pairing.CodeMatchesPythonReference
    assert pairing_code(bytes(range(32)), "v1.2000000000.00112233445566778899aabbccddeeff") == "NLI7ENCI"
    assert format_code("NLI7ENCI") == "NLI7-ENCI" and normalize_code(" nli7-enci ") == "NLI7ENCI"
    assert nonce_expiry("v1.2000000000.00112233445566778899aabbccddeeff") == 2000000000
    for bad in ("v2.1.00112233445566778899aabbccddeeff", "v1.1.00112233445566778899AABBCCDDEEFF", "v1..00",
                "v1.2000000000.0011"):
        assert nonce_expiry(bad) is None
    assert re.fullmatch(r"v1\.123\.[0-9a-f]{32}", make_nonce(123))
    assert clean_device_name("  Galaxy\n\x1b[31m S24 ") == "Galaxy[31m S24" and clean_device_name("\t") == "unnamed"
    assert bearer_token("Bearer abc") == "abc" and bearer_token("  bearer   abc  ") == "abc"
    assert bearer_token("Basic abc") is None and bearer_token("Bearer") is None and bearer_token("") is None


def test_secret_and_token_files_are_private(service: PairingService) -> None:
    data = service.config.data_dir
    secret = (data / "pairing-secret").read_text(encoding="ascii")
    assert re.fullmatch(r"[0-9a-f]{64}\n", secret)
    service.prepare()
    assert (data / "pairing-secret").read_text(encoding="ascii") == secret  # kept
    record, token = service.tokens.issue("phone")
    assert re.fullmatch(r"hm1_[0-9a-f]{64}", token)
    stored = (data / "tokens.json").read_text(encoding="utf-8")
    assert token not in stored and token[4:] not in stored
    assert hashlib.sha256(token.encode()).hexdigest() in stored and record.sha256 in stored
    if POSIX:
        assert (data.stat().st_mode & 0o777) == 0o700
        assert ((data / "pairing-secret").stat().st_mode & 0o777) == 0o600
        assert ((data / "tokens.json").stat().st_mode & 0o777) == 0o600


def test_health_is_open_everything_else_needs_a_token(service: PairingService) -> None:
    _, token = service.tokens.issue("phone")
    with TestClient(main.app) as client:
        assert client.get("/api/v1/health").status_code == 200
        for path in ("/api/v1/dashboard", "/api/v1/hardware-map", "/api/v1/devices", "/api/v1/tasks/x",
                     "/api/v1/unknown"):
            response = client.get(path)
            assert response.status_code == 401, path
            assert response.json() == {"detail": "missing bearer token"}
            assert response.headers["www-authenticate"] == "Bearer"
        assert client.post("/api/v1/devices/FAN-01/commands", json={"command": "ON"}).status_code == 401
        wrong = client.get("/api/v1/dashboard", headers={"Authorization": "Bearer hm1_" + "f" * 64})
        assert wrong.status_code == 401 and wrong.json() == {"detail": "invalid or revoked token"}
        assert client.get("/api/v1/dashboard", headers={"Authorization": "Basic abc"}).status_code == 401
        assert client.get("/api/v1/dashboard", headers={"Authorization": f"Bearer {token}"}).status_code == 200
        assert client.get("/api/v1/dashboard", headers={"Authorization": f"bearer  {token} "}).status_code == 200


def test_claim_once_then_used_expired_wrong_and_rate_limited(service: PairingService) -> None:
    now = int(time.time())
    offer = service.make_offer(now, BASE_URL + "/", 600)
    assert offer.qr == {"version": 1, "device_id": "NODE-RPI4-01", "device_name": "HomeMind NODE-RPI4-01",
                        "base_url": BASE_URL, "pairing_code": offer.code, "nonce": offer.nonce,
                        "expires_at": offer.qr["expires_at"]}
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", offer.qr["expires_at"])
    with TestClient(main.app) as client:
        issued = _claim(client, offer)
        assert issued.status_code == 201
        body = issued.json()
        assert body["token_type"] == "Bearer" and body["device_id"] == "NODE-RPI4-01"
        assert client.get("/api/v1/devices", headers={"Authorization": f"Bearer {body['token']}"}).status_code == 200
        assert [(r.id, r.device_name) for r in service.tokens.list()] == [(body["token_id"], "Galaxy")]
        # one-time (failure 1)
        again = _claim(client, offer)
        assert again.status_code == 401
        assert again.json() == {"detail": "invalid, expired or already used pairing code"}
        # also after a restart: used nonces persist
        restarted = PairingService(service.config)
        assert restarted.claim(offer.code, offer.nonce, "x", "c", now * 1000).status == "invalid"
        # expired (failure 2) and lowercase / no-dash input for a fresh code is fine
        old = service.make_offer(now - 100, BASE_URL, 30)
        assert _claim(client, old).status_code == 401
        fresh = service.make_offer(now, BASE_URL, 60)
        assert _claim(client, fresh, code=normalize_code(fresh.code).lower()).status_code == 201
        # wrong code (failure 3) -> the testclient address is now limited, even with the right code
        another = service.make_offer(now, BASE_URL, 600)
        assert _claim(client, another, code="AAAA-AAAA").status_code == 401
        limited = _claim(client, another)
        assert limited.status_code == 429
        assert limited.json() == {"detail": "too many failed pairing attempts; try again later"}
        assert 1 <= int(limited.headers["retry-after"]) <= 60


def test_forged_far_future_nonce_is_refused(service: PairingService) -> None:
    now = int(time.time())
    nonce = make_nonce(now + MAX_CODE_TTL_S + 3600)
    code = pairing_code(load_pairing_secret(service.config.secret_file), nonce)  # even with the right MAC
    assert service.claim(code, nonce, "x", "c", now * 1000).status == "invalid"


def test_global_limit_and_window(service: PairingService) -> None:
    now_ms = int(time.time()) * 1000
    offer = service.make_offer(now_ms // 1000, BASE_URL, 600)
    for client in ("a", "b", "c", "d", "e"):
        assert service.claim("AAAA-AAAA", offer.nonce, "x", client, now_ms).status == "invalid"
    blocked = service.claim(offer.code, offer.nonce, "x", "f", now_ms)
    assert blocked.status == "rate_limited" and blocked.retry_after_s == 60
    assert service.claim(offer.code, offer.nonce, "x", "f", now_ms + 60_000).status == "ok"


def test_claim_validation_matches_fastapi_shapes(service: PairingService) -> None:
    with TestClient(main.app) as client:
        missing = client.post("/api/v1/pairing/claim", json={})
        assert missing.status_code == 422
        assert [(e["type"], e["loc"]) for e in missing.json()["detail"]] == [
            ("missing", ["body", "code"]), ("missing", ["body", "nonce"])]
        long = client.post("/api/v1/pairing/claim", json={"code": "A" * 33, "nonce": "x", "device_name": "가" * 65})
        assert [(e["type"], e["loc"]) for e in long.json()["detail"]] == [
            ("string_too_long", ["body", "code"]), ("string_too_long", ["body", "device_name"])]
        assert client.get("/api/v1/pairing/claim").status_code == 405


def test_revocation_from_the_cli_is_seen_by_the_running_api(service: PairingService, monkeypatch, capsys) -> None:
    _, token_a = service.tokens.issue("phone A")
    record_b, token_b = service.tokens.issue("phone B")
    monkeypatch.setenv("HOMEMIND_DATA_DIR", str(service.config.data_dir))
    with TestClient(main.app) as client:
        assert client.get("/api/v1/devices", headers={"Authorization": f"Bearer {token_a}"}).status_code == 200
        assert cli.main(["--list-tokens"]) == 0
        listing = capsys.readouterr().out
        assert "phone A" in listing and "phone B" in listing and token_a not in listing
        token_a_id = next(line.split()[0] for line in listing.splitlines() if line.endswith("phone A"))
        assert cli.main(["--revoke-token", token_a_id]) == 0
        assert cli.main(["--revoke-token", token_a_id]) == 1
        time.sleep(0.01)
        assert client.get("/api/v1/devices", headers={"Authorization": f"Bearer {token_a}"}).status_code == 401
        assert client.get("/api/v1/devices", headers={"Authorization": f"Bearer {token_b}"}).status_code == 200
        assert cli.main(["--revoke-all-tokens"]) == 0
        assert client.get("/api/v1/devices", headers={"Authorization": f"Bearer {token_b}"}).status_code == 401
    assert record_b.id not in [r.id for r in TokenStore(service.config.data_dir).list()]


def test_cli_prints_a_qr_the_api_accepts(service: PairingService, monkeypatch, capsys) -> None:
    monkeypatch.setenv("HOMEMIND_DATA_DIR", str(service.config.data_dir))
    monkeypatch.setenv("HOMEMIND_PUBLIC_BASE_URL", BASE_URL)
    assert cli.main(["--print-pairing-qr", "--json"]) == 0
    qr = json.loads(capsys.readouterr().out)
    assert set(qr) == {"version", "device_id", "device_name", "base_url", "pairing_code", "nonce", "expires_at"}
    assert cli.main(["--print-pairing-qr"]) == 0
    human = capsys.readouterr().out
    assert "pairing code:" in human and human.strip().splitlines()[-1].startswith("{")
    with TestClient(main.app) as client:
        issued = client.post("/api/v1/pairing/claim", json={"code": qr["pairing_code"], "nonce": qr["nonce"]})
        assert issued.status_code == 201
    assert [r.device_name for r in service.tokens.list()] == ["unnamed"]
    monkeypatch.delenv("HOMEMIND_PUBLIC_BASE_URL")
    assert cli.main(["--print-pairing-qr"]) == 2  # no base URL configured


def test_corrupt_token_file_fails_closed(service: PairingService) -> None:
    _, token = service.tokens.issue("phone")
    Path(service.tokens.path).write_text("{not json", encoding="utf-8")
    assert service.tokens.verify(token) is None
    with pytest.raises(RuntimeError):
        service.tokens.issue("other")
    assert service.tokens.revoke_all() == 0
    _, token2 = service.tokens.issue("other")
    assert service.tokens.verify(token2)


def test_secret_value_in_environment_is_refused(monkeypatch) -> None:
    monkeypatch.setenv("HOMEMIND_PAIRING_SECRET", "x")
    with pytest.raises(RuntimeError):
        AuthConfig.from_env()
