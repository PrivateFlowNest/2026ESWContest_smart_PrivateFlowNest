"""Persistent, resumable cache for frozen audio embeddings."""

from __future__ import annotations

import hashlib
import json
import sqlite3
from pathlib import Path

import numpy as np


CACHE_SCHEMA_VERSION = 1


class CachedEmbeddingExtractor:
    """Wrap an extractor and persist vectors in a compact SQLite database."""

    def __init__(self, extractor, cache_path: str | Path, *, commit_every: int = 25):
        if commit_every < 1:
            raise ValueError("commit_every must be positive")
        self.extractor = extractor
        self.cache_path = Path(cache_path).resolve()
        self.cache_path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(self.cache_path)
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.execute("PRAGMA synchronous=NORMAL")
        self.connection.execute(
            """
            CREATE TABLE IF NOT EXISTS embeddings (
                cache_key TEXT PRIMARY KEY,
                dimensions INTEGER NOT NULL,
                vector BLOB NOT NULL
            )
            """
        )
        self.commit_every = commit_every
        self.pending_writes = 0
        self.hits = 0
        self.misses = 0
        # Windowed manifests can reference the same large source recording many
        # times. Resolving and stat-ing /mnt/e for every window is especially
        # expensive under WSL, so retain immutable metadata for this run.
        self._file_metadata: dict[str, tuple[str, int, int]] = {}
        self.model_identity = str(
            getattr(
                extractor,
                "cache_identity",
                f"{type(extractor).__module__}.{type(extractor).__qualname__}",
            )
        )

    def lookup(
        self,
        path: str | Path,
        start_seconds: float | None = None,
        end_seconds: float | None = None,
    ) -> tuple[str, np.ndarray | None]:
        """Return a cache key and vector without running the wrapped model."""

        key = self._key(path, start_seconds, end_seconds)
        cached = self.connection.execute(
            "SELECT dimensions, vector FROM embeddings WHERE cache_key = ?", (key,)
        ).fetchone()
        if cached is None:
            self.misses += 1
            return key, None
        dimensions, raw = cached
        values = np.frombuffer(raw, dtype=np.float32)
        if values.size != dimensions:
            self.misses += 1
            return key, None
        self.hits += 1
        return key, values.copy()

    def lookup_many(
        self,
        items: list[tuple[str | Path, float | None, float | None]],
        *,
        query_size: int = 500,
    ) -> list[tuple[str, np.ndarray | None]]:
        """Read many cached vectors with bounded SQLite round trips."""

        if query_size < 1:
            raise ValueError("query_size must be positive")
        keys = [self._key(path, start, end) for path, start, end in items]
        cached_by_key: dict[str, tuple[int, bytes]] = {}
        if len(keys) >= 5_000:
            # Cache keys are hashes, so repeated indexed lookups become random I/O.
            # A temporary key table lets SQLite scan the large BLOB table once in
            # storage order and probe the small in-memory key index.
            self.connection.execute("DROP TABLE IF EXISTS temp.requested_cache_keys")
            self.connection.execute(
                "CREATE TEMP TABLE requested_cache_keys "
                "(cache_key TEXT PRIMARY KEY) WITHOUT ROWID"
            )
            try:
                self.connection.executemany(
                    "INSERT OR IGNORE INTO requested_cache_keys(cache_key) VALUES (?)",
                    ((key,) for key in keys),
                )
                rows = self.connection.execute(
                    "SELECT e.cache_key, e.dimensions, e.vector "
                    "FROM embeddings AS e NOT INDEXED "
                    "JOIN requested_cache_keys AS r ON r.cache_key = e.cache_key"
                ).fetchall()
                cached_by_key.update(
                    {
                        str(key): (int(dimensions), raw)
                        for key, dimensions, raw in rows
                    }
                )
            finally:
                self.connection.execute("DROP TABLE temp.requested_cache_keys")
        else:
            for offset in range(0, len(keys), query_size):
                chunk = keys[offset : offset + query_size]
                placeholders = ",".join("?" for _ in chunk)
                rows = self.connection.execute(
                    f"SELECT cache_key, dimensions, vector FROM embeddings "
                    f"WHERE cache_key IN ({placeholders})",
                    chunk,
                ).fetchall()
                cached_by_key.update(
                    {
                        str(key): (int(dimensions), raw)
                        for key, dimensions, raw in rows
                    }
                )

        results: list[tuple[str, np.ndarray | None]] = []
        for key in keys:
            cached = cached_by_key.get(key)
            if cached is None:
                self.misses += 1
                results.append((key, None))
                continue
            dimensions, raw = cached
            values = np.frombuffer(raw, dtype=np.float32)
            if values.size != dimensions:
                self.misses += 1
                results.append((key, None))
                continue
            self.hits += 1
            results.append((key, values.copy()))
        return results

    def store(self, key: str, vector: np.ndarray) -> np.ndarray:
        """Persist one already-computed vector and return its normalized form."""

        values = np.asarray(vector, dtype=np.float32).reshape(-1)
        self.connection.execute(
            "INSERT OR REPLACE INTO embeddings(cache_key, dimensions, vector) "
            "VALUES (?, ?, ?)",
            (key, int(values.size), values.tobytes()),
        )
        self.pending_writes += 1
        if self.pending_writes >= self.commit_every:
            self.connection.commit()
            self.pending_writes = 0
        return values

    def _key(
        self,
        path: str | Path,
        start_seconds: float | None,
        end_seconds: float | None,
    ) -> str:
        source_key = str(path)
        metadata = self._file_metadata.get(source_key)
        if metadata is None:
            audio_path = Path(path).resolve()
            stat = audio_path.stat()
            metadata = (str(audio_path), stat.st_size, stat.st_mtime_ns)
            self._file_metadata[source_key] = metadata
        resolved_path, size, mtime_ns = metadata
        payload = {
            "schema": CACHE_SCHEMA_VERSION,
            "model": self.model_identity,
            "path": resolved_path,
            "size": size,
            "mtime_ns": mtime_ns,
            "start": start_seconds,
            "end": end_seconds,
        }
        encoded = json.dumps(payload, sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(encoded.encode("utf-8")).hexdigest()

    def _extract(
        self,
        path: str | Path,
        start_seconds: float | None,
        end_seconds: float | None,
    ) -> np.ndarray:
        key, cached = self.lookup(path, start_seconds, end_seconds)
        if cached is not None:
            return cached

        if start_seconds is None:
            vector = self.extractor.extract_file(path)
        else:
            extract_segment = getattr(self.extractor, "extract_segment", None)
            if extract_segment is None:
                vector = self.extractor.extract_file(path)
            else:
                vector = extract_segment(path, start_seconds, end_seconds)
        return self.store(key, vector)

    def extract_file(self, path: str | Path) -> np.ndarray:
        return self._extract(path, None, None)

    def extract_segment(
        self, path: str | Path, start_seconds: float, end_seconds: float
    ) -> np.ndarray:
        return self._extract(path, start_seconds, end_seconds)

    def stats(self) -> dict[str, object]:
        return {
            "path": str(self.cache_path),
            "hits": self.hits,
            "misses": self.misses,
            "model_identity": self.model_identity,
            "unique_source_files": len(self._file_metadata),
        }

    def close(self) -> None:
        if self.connection is not None:
            self.connection.commit()
            self.connection.close()
            self.connection = None
