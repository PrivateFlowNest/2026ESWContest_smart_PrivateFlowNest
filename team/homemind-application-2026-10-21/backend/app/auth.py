"""QR pairing and bearer tokens for the Mobile API.

Same files and algorithm as the C++ service (native/services/homemind-mobile-api/src/auth.hpp), so a QR printed by
either side is accepted by the other and both read the same tokens.json:

  pairing secret   <secret_file>: first line = 64 lowercase hex chars (32 random bytes), mode 0600. Created when
                   missing. Never leaves the device.
  nonce            "v1.<expires_at_unix_s>.<32 hex random>"
  pairing code     base32(HMAC-SHA256(secret, "homemind-pairing-v1\\n" + nonce)[:5]) -> 8 chars, shown "ABCD-EFGH".
                   One-time (used nonces kept in <data_dir>/pairing-used.json until they expire), short-lived
                   (default 600 s, at most 3600 s).
  token            "hm1_" + 64 hex (32 random bytes). Only SHA-256(token) is stored, in <data_dir>/tokens.json (0600),
                   with an 8-hex id, the phone's name and the creation time. Valid until revoked.
  failed claims    at most N per client address and M overall in a sliding window -> 429 + Retry-After.

Codes and tokens are never logged.
"""

from __future__ import annotations

import base64
import contextlib
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator

log = logging.getLogger(__name__)

CODE_CONTEXT = b"homemind-pairing-v1\n"
CODE_BYTES = 5  # 40 bits -> 8 base32 characters
MIN_CODE_TTL_S = 30
MAX_CODE_TTL_S = 3600
EXPIRY_SLACK_S = 60
_NONCE = re.compile(r"v1\.([0-9]{1,12})\.([0-9a-f]{32})")
_HEX64 = re.compile(r"[0-9a-f]{64}")


def now_ms() -> int:
    return int(time.time() * 1000)


def utc_iso_seconds(seconds: int) -> str:
    return datetime.fromtimestamp(seconds, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _env_int(name: str, default: int) -> int:
    value = os.getenv(name)
    return int(value) if value else default


@dataclass
class AuthConfig:
    data_dir: Path
    secret_file: Path | None = None  # default <data_dir>/pairing-secret
    device_id: str = "NODE-RPI4-01"
    device_name: str = "HomeMind NODE-RPI4-01"
    public_base_url: str = ""  # what the phone uses to reach this API (QR base_url)
    code_ttl_s: int = 600
    max_failures_per_client: int = 5
    max_failures_global: int = 20
    failure_window_s: int = 300

    def __post_init__(self) -> None:
        self.data_dir = Path(self.data_dir)
        self.secret_file = Path(self.secret_file) if self.secret_file else self.data_dir / "pairing-secret"
        if not MIN_CODE_TTL_S <= self.code_ttl_s <= MAX_CODE_TTL_S:
            raise RuntimeError(f"pairing code lifetime must be {MIN_CODE_TTL_S}..{MAX_CODE_TTL_S} s")
        if min(self.max_failures_per_client, self.max_failures_global, self.failure_window_s) < 1:
            raise RuntimeError("pairing failure limits must be positive")

    @classmethod
    def from_env(cls) -> "AuthConfig":
        if os.getenv("HOMEMIND_PAIRING_SECRET"):
            raise RuntimeError("HOMEMIND_PAIRING_SECRET is not accepted; use HOMEMIND_PAIRING_SECRET_FILE")
        device_id = os.getenv("HOMEMIND_DEVICE_ID") or os.getenv("HOMEMIND_SOURCE_NODE_ID") or "NODE-RPI4-01"
        return cls(
            data_dir=Path(os.getenv("HOMEMIND_DATA_DIR") or Path.home() / ".homemind" / "mobile-api"),
            secret_file=Path(os.environ["HOMEMIND_PAIRING_SECRET_FILE"]) if os.getenv("HOMEMIND_PAIRING_SECRET_FILE") else None,
            device_id=device_id,
            device_name=os.getenv("HOMEMIND_DEVICE_NAME") or f"HomeMind {device_id}",
            public_base_url=os.getenv("HOMEMIND_PUBLIC_BASE_URL", ""),
            code_ttl_s=_env_int("HOMEMIND_PAIRING_CODE_TTL_S", 600),
            max_failures_per_client=_env_int("HOMEMIND_PAIRING_MAX_FAILURES_PER_CLIENT", 5),
            max_failures_global=_env_int("HOMEMIND_PAIRING_MAX_FAILURES_GLOBAL", 20),
            failure_window_s=_env_int("HOMEMIND_PAIRING_FAILURE_WINDOW_S", 300),
        )


# --- files ------------------------------------------------------------------------------------------------------
def ensure_private_dir(path: Path) -> None:
    """mkdir -p; a directory created here gets mode 0700."""

    if path.exists():
        return
    path.mkdir(parents=True)
    os.chmod(path, 0o700)


def _match_directory_owner(fd: int, path: Path) -> None:
    """When root writes (the CLI run by a person), hand the file to the owner of its directory (the service user)."""

    if not hasattr(os, "geteuid") or os.geteuid() != 0:
        return
    try:
        info = os.stat(path.parent)
        os.fchown(fd, info.st_uid, info.st_gid)
    except OSError as error:
        log.warning("cannot hand %s to the directory owner: %s", path, error)


def _create_private(path: Path, content: bytes) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o600)
    try:
        view = memoryview(content)
        while view:
            view = view[os.write(fd, view):]
        os.fsync(fd)
        _match_directory_owner(fd, path)
    except BaseException:
        os.close(fd)
        path.unlink(missing_ok=True)
        raise
    os.close(fd)


def write_private_file(path: Path, content: str) -> None:
    """Write via a 0600 temp file + rename."""

    tmp = path.parent / f".{path.name}.tmp-{secrets.token_hex(4)}"
    _create_private(tmp, content.encode("utf-8"))
    try:
        os.replace(tmp, path)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise


@contextlib.contextmanager
def _directory_lock(path: Path) -> Iterator[None]:
    """Exclusive flock on the data directory for read-modify-write of tokens.json (service vs CLI). POSIX only."""

    try:
        import fcntl
    except ImportError:  # Windows PC development: one process at a time
        yield
        return
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


# --- pairing secret / code ----------------------------------------------------------------------------------------
def ensure_pairing_secret(path: Path) -> bool:
    """Creates the secret when missing. True when it was created now."""

    if path.exists():
        return False
    ensure_private_dir(path.parent)
    try:
        _create_private(path, (secrets.token_hex(32) + "\n").encode("ascii"))
    except FileExistsError:
        return False
    return True


def load_pairing_secret(path: Path) -> bytes:
    text = path.read_text(encoding="utf-8-sig")
    line = text.splitlines()[0].strip(" \t") if text else ""
    if not _HEX64.fullmatch(line):
        raise RuntimeError(f"pairing secret {path} must hold 64 lowercase hex characters")
    return bytes.fromhex(line)


def pairing_code(secret: bytes, nonce: str) -> str:
    mac = hmac.new(secret, CODE_CONTEXT + nonce.encode("utf-8"), hashlib.sha256).digest()
    return base64.b32encode(mac[:CODE_BYTES]).decode("ascii")


def format_code(code: str) -> str:
    return f"{code[:4]}-{code[4:]}" if len(code) == 8 else code


def normalize_code(code: str) -> str:
    return "".join(ch.upper() if "a" <= ch <= "z" else ch for ch in code if ch not in "- ")


def nonce_expiry(nonce: str) -> int | None:
    match = _NONCE.fullmatch(nonce)
    return int(match.group(1)) if match else None


def make_nonce(expires_at_s: int) -> str:
    return f"v1.{expires_at_s}.{secrets.token_hex(16)}"


def clean_device_name(name: str) -> str:
    cleaned = "".join(ch for ch in name if ord(ch) >= 0x20 and ord(ch) != 0x7F).strip(" ")
    return cleaned or "unnamed"


def bearer_token(authorization: str) -> str | None:
    value = authorization.strip(" \t\r\n")
    scheme, separator, rest = value.partition(" ")
    if not separator or scheme.lower() != "bearer":
        return None
    token = rest.strip(" \t\r\n")
    if not token or len(token) > 512:
        return None
    return token


@dataclass
class PairingOffer:
    code: str  # formatted
    nonce: str
    expires_at_s: int
    qr: dict[str, Any]  # payload the app parses (lib/models/pairing_models.dart, version 1)


# --- tokens -------------------------------------------------------------------------------------------------------
@dataclass
class TokenRecord:
    id: str
    sha256: str
    device_name: str
    created_at: str


class TokenStore:
    def __init__(self, data_dir: Path) -> None:
        self.data_dir = Path(data_dir)
        self.path = self.data_dir / "tokens.json"
        self._lock = threading.Lock()
        self._records: list[TokenRecord] = []
        self._loaded: tuple[int, int, int] | None = (-1, -1, -1)
        self._corrupt = False

    def _signature(self) -> tuple[int, int, int] | None:
        try:
            info = os.stat(self.path)
        except FileNotFoundError:
            return None
        return info.st_mtime_ns, info.st_size, info.st_ino

    def _load(self, force: bool) -> None:
        current = self._signature()
        if not force and current == self._loaded:
            return
        self._records, self._corrupt, self._loaded = [], False, current
        if current is None:
            return
        try:
            document = json.loads(self.path.read_text(encoding="utf-8-sig"))
            for item in document["tokens"]:
                record = TokenRecord(str(item["id"]), str(item["sha256"]), str(item.get("device_name", "unnamed")),
                                     str(item.get("created_at", "")))
                if not _HEX64.fullmatch(record.sha256) or not record.id:
                    log.warning("ignoring a malformed entry in %s", self.path)
                    continue
                self._records.append(record)
        except (OSError, ValueError, KeyError, TypeError) as error:
            self._records, self._corrupt = [], True  # fail closed until the file is fixed
            log.error("%s is unreadable; every token is refused: %s", self.path, error)

    def _save(self) -> None:
        document = {"version": 1, "tokens": [record.__dict__ for record in self._records]}
        ensure_private_dir(self.data_dir)
        write_private_file(self.path, json.dumps(document, ensure_ascii=False, indent=2) + "\n")
        self._loaded, self._corrupt = self._signature(), False

    def verify(self, token: str) -> str | None:
        """Token id when valid. Re-reads tokens.json when it changed on disk (CLI revocation)."""

        digest = hashlib.sha256(token.encode("utf-8")).hexdigest()
        with self._lock:
            self._load(False)
            if self._corrupt:
                return None
            found = None
            for record in self._records:
                if hmac.compare_digest(record.sha256, digest):
                    found = record.id
            return found

    def issue(self, device_name: str) -> tuple[TokenRecord, str]:
        with self._lock:
            ensure_private_dir(self.data_dir)
            with _directory_lock(self.data_dir):
                self._load(True)
                if self._corrupt:
                    raise RuntimeError(f"{self.path} is unreadable; fix or remove it before pairing")
                token = "hm1_" + secrets.token_hex(32)
                ids = {record.id for record in self._records}
                token_id = secrets.token_hex(4)
                while token_id in ids:
                    token_id = secrets.token_hex(4)
                record = TokenRecord(token_id, hashlib.sha256(token.encode("utf-8")).hexdigest(), device_name,
                                     utc_iso_seconds(int(time.time())))
                self._records.append(record)
                self._save()
                return record, token

    def list(self) -> list[TokenRecord]:
        with self._lock:
            self._load(True)
            if self._corrupt:
                raise RuntimeError(f"{self.path} is unreadable")
            return list(self._records)

    def revoke(self, token_id: str) -> bool:
        with self._lock:
            if not self.data_dir.exists():
                return False
            with _directory_lock(self.data_dir):
                self._load(True)
                if self._corrupt:
                    raise RuntimeError(f"{self.path} is unreadable; use --revoke-all-tokens to reset it")
                kept = [record for record in self._records if record.id != token_id]
                if len(kept) == len(self._records):
                    return False
                self._records = kept
                self._save()
                return True

    def revoke_all(self) -> int:
        with self._lock:
            if not self.data_dir.exists():
                return 0
            with _directory_lock(self.data_dir):
                self._load(True)
                count = len(self._records)
                self._records = []
                self._save()  # also resets an unreadable file
                return count


# --- failed-claim limiter -----------------------------------------------------------------------------------------
class FailureLimiter:
    def __init__(self, per_client: int, global_limit: int, window_s: int) -> None:
        self.per_client = per_client
        self.global_limit = global_limit
        self.window_ms = window_s * 1000
        self._lock = threading.Lock()
        self._failures: deque[tuple[int, str]] = deque()  # (time, client), oldest first

    def _prune(self, at_ms: int) -> None:
        while self._failures and self._failures[0][0] + self.window_ms <= at_ms:
            self._failures.popleft()

    def blocked_for(self, client: str, at_ms: int) -> int:
        """Seconds until the client may try again (0 = allowed)."""

        with self._lock:
            self._prune(at_ms)
            wait_ms = 0
            if len(self._failures) >= self.global_limit:
                wait_ms = max(wait_ms, self._failures[len(self._failures) - self.global_limit][0] + self.window_ms - at_ms)
            mine = [at for at, who in self._failures if who == client]
            if len(mine) >= self.per_client:
                wait_ms = max(wait_ms, mine[len(mine) - self.per_client] + self.window_ms - at_ms)
            return 0 if wait_ms <= 0 else (wait_ms + 999) // 1000

    def record(self, client: str, at_ms: int) -> None:
        with self._lock:
            self._prune(at_ms)
            self._failures.append((at_ms, client))


# --- pairing ------------------------------------------------------------------------------------------------------
@dataclass
class ClaimResult:
    status: str  # ok | invalid | rate_limited
    token: str | None = None
    record: TokenRecord | None = None
    retry_after_s: int = 0


@dataclass
class PairingService:
    config: AuthConfig
    tokens: TokenStore = field(init=False)

    def __post_init__(self) -> None:
        self.tokens = TokenStore(self.config.data_dir)
        self._limiter = FailureLimiter(self.config.max_failures_per_client, self.config.max_failures_global,
                                       self.config.failure_window_s)
        self._lock = threading.Lock()
        self._used: dict[str, int] = {}
        self._used_loaded = False

    @property
    def _used_path(self) -> Path:
        return self.config.data_dir / "pairing-used.json"

    def prepare(self) -> None:
        """Ensures the data dir and the secret exist (service start)."""

        ensure_private_dir(self.config.data_dir)
        if ensure_pairing_secret(self.config.secret_file):
            log.info("created pairing secret %s", self.config.secret_file)
        if os.name == "posix" and os.stat(self.config.secret_file).st_mode & 0o007:
            log.warning("%s is readable by other users; chmod 0600 it", self.config.secret_file)
        load_pairing_secret(self.config.secret_file)  # fail at start, not at the first claim

    def make_offer(self, now_s: int, base_url: str, ttl_s: int) -> PairingOffer:
        if not MIN_CODE_TTL_S <= ttl_s <= MAX_CODE_TTL_S:
            raise ValueError(f"pairing code lifetime must be {MIN_CODE_TTL_S}..{MAX_CODE_TTL_S} s")
        if not (base_url.startswith("http://") or base_url.startswith("https://")):
            raise ValueError("base URL must start with http:// or https:// (set HOMEMIND_PUBLIC_BASE_URL or --base-url)")
        expires = now_s + ttl_s
        nonce = make_nonce(expires)
        code = format_code(pairing_code(load_pairing_secret(self.config.secret_file), nonce))
        url = base_url
        while len(url) > 1 and url.endswith("/"):
            url = url[:-1]
        qr = {"version": 1, "device_id": self.config.device_id, "device_name": self.config.device_name,
              "base_url": url, "pairing_code": code, "nonce": nonce, "expires_at": utc_iso_seconds(expires)}
        return PairingOffer(code=code, nonce=nonce, expires_at_s=expires, qr=qr)

    def _load_used(self, now_s: int) -> None:
        if not self._used_loaded:
            self._used_loaded = True
            if self._used_path.exists():
                try:
                    document = json.loads(self._used_path.read_text(encoding="utf-8"))
                    self._used = {str(nonce): int(expires) for nonce, expires in document["used"].items()}
                except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
                    log.error("%s is unreadable: %s", self._used_path, error)
        self._used = {nonce: expires for nonce, expires in self._used.items() if expires >= now_s}

    def _save_used(self) -> None:
        ensure_private_dir(self.config.data_dir)
        write_private_file(self._used_path, json.dumps({"version": 1, "used": dict(sorted(self._used.items()))},
                                                       indent=2) + "\n")

    def _check(self, code: str, nonce: str, now_s: int) -> str:
        """Empty when the code is good, else the reason (logged, never sent to the client)."""

        expires = nonce_expiry(nonce)
        if expires is None:
            return "malformed nonce"
        if now_s >= expires:
            return "expired"
        if expires > now_s + MAX_CODE_TTL_S + EXPIRY_SLACK_S:
            return "expiry beyond the maximum lifetime"
        expected = pairing_code(load_pairing_secret(self.config.secret_file), nonce)
        if not hmac.compare_digest(normalize_code(code).encode("utf-8"), expected.encode("utf-8")):
            return "wrong code"
        if nonce in self._used:
            return "already used"
        return ""

    def claim(self, code: str, nonce: str, device_name: str, client: str, at_ms: int) -> ClaimResult:
        wait = self._limiter.blocked_for(client, at_ms)
        if wait > 0:
            log.warning("pairing claim from %s refused: too many failed attempts", client)
            return ClaimResult("rate_limited", retry_after_s=wait)
        with self._lock:
            now_s = at_ms // 1000
            self._load_used(now_s)
            reason = self._check(code, nonce, now_s)
            if reason:
                self._limiter.record(client, at_ms)
                log.warning("pairing claim from %s refused (%s)", client, reason)
                return ClaimResult("invalid")
            self._used[nonce] = nonce_expiry(nonce) or 0
            self._save_used()
            record, token = self.tokens.issue(clean_device_name(device_name))
        log.info("token %s issued to '%s' from %s", record.id, record.device_name, client)
        return ClaimResult("ok", token=token, record=record)


OPEN_API_PATHS = {"/api/v1/health", "/api/v1/pairing/claim"}


def requires_token(method: str, path: str) -> bool:
    """Everything under /api/ except the health check and the pairing claim (and CORS preflights)."""

    if method == "OPTIONS":
        return False
    return path.startswith("/api/") and path not in OPEN_API_PATHS
