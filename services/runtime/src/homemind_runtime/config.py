"""Runtime configuration. Addresses and accounts live in a JSON file; passwords only as a file path."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

REPOSITORY_ROOT = Path(__file__).resolve().parents[4]
CONTRACTS_DIR = REPOSITORY_ROOT / "contracts"


@dataclass
class BrokerConfig:
    host: str = "127.0.0.1"
    port: int = 1883
    username: str | None = None
    password_file: str | None = None
    keepalive: int = 30

    def password(self) -> str | None:
        if not self.password_file:
            return None
        value = Path(self.password_file).read_text(encoding="utf-8").splitlines()[0].strip()
        if not value:
            raise ValueError(f"MQTT password file is empty: {self.password_file}")
        return value


@dataclass
class RuntimeConfig:
    broker: BrokerConfig = field(default_factory=BrokerConfig)
    contracts_dir: Path = CONTRACTS_DIR
    perception_node_id: str = "NODE-RPI5-02"
    llm_node_id: str = "NODE-RPI5-01"
    llm_base_url: str = "http://127.0.0.1:8080"
    llm_model: str = "exaone-1.2b-homemind-v3"
    llm_request_ttl_ms: int = 10000
    simulated: bool = True
    extra: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def load(cls, path: str | Path | None) -> "RuntimeConfig":
        if path is None:
            return cls()
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
        broker = BrokerConfig(**raw.pop("broker", {}))
        contracts = raw.pop("contracts_dir", None)
        known = {name for name in cls.__dataclass_fields__ if name not in {"broker", "contracts_dir", "extra"}}
        values = {key: raw.pop(key) for key in list(raw) if key in known}
        return cls(
            broker=broker,
            contracts_dir=Path(contracts) if contracts else CONTRACTS_DIR,
            extra=raw,
            **values,
        )
