"""homemind-runtime CLI.

  homemind-runtime llm        --config cfg.json   # NODE-RPI5-01: EXAONE node (needs llama-server)
  homemind-runtime voice-text --config cfg.json   # NODE-RPI5-02 parser fed from the keyboard (demo fallback)
  homemind-runtime perception --config cfg.json   # NODE-RPI5-02: microphone (+camera) pipeline
"""

from __future__ import annotations

import argparse
import logging
import signal
import sys
import threading
from typing import Any

from . import pfn
from .action_map import ActionMap
from .config import RuntimeConfig
from .mqtt import Bus

log = logging.getLogger("homemind_runtime")

_REGISTRY_TO_AI_ZONE = {"bed": "BED", "desk": "DESK", "bathroom": "BATHROOM", "entrance": "ENTRANCE",
                        "living": "COMMON", "outdoor": "OUTDOOR"}


class ContextTracker:
    """Latest ACTIVITY_CONTEXT perception as the sanitized activity-context the guard/EXAONE expect."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._context: dict[str, Any] | None = None
        self._valid_until_ms = 0

    def update(self, topic: str, document: dict[str, Any]) -> None:
        del topic
        if document.get("observation_type") != "ACTIVITY_CONTEXT":
            return
        attributes = document["attributes"]
        with self._lock:
            self._context = {
                "presence_zone": _REGISTRY_TO_AI_ZONE.get(document["zone"], "UNKNOWN"),
                "activity": attributes["activity"],
                "audio_context": attributes["audio_context"],
                "quiet_required": bool(attributes["quiet_required"]),
                "confidence": float(document["confidence"]),
            }
            self._valid_until_ms = int(attributes.get("valid_until_ms", 0))

    def current(self) -> dict[str, Any] | None:
        with self._lock:
            if self._context is None or self._valid_until_ms < pfn.now_ms():
                return None
            return dict(self._context)


def _wait_forever() -> None:
    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    while not stop.wait(0.5):
        pass


def run_llm(config: RuntimeConfig) -> int:
    from homemind_exaone.client import ExaoneClient

    from .llm_node import LlmNode

    node = LlmNode(ExaoneClient(base_url=config.llm_base_url, model=config.llm_model, timeout_seconds=30.0),
                   ActionMap(config.contracts_dir), node_id=config.llm_node_id, simulated=config.simulated)
    bus = Bus(config.broker, f"{config.llm_node_id}-llm")

    def on_request(topic: str, document: dict[str, Any]) -> None:
        del topic
        for out_topic, message in node.handle(document):
            bus.publish(out_topic, message)

    bus.subscribe(pfn.TOPICS["llm_request"], on_request)
    bus.start()
    log.info("LLM node ready (%s)", config.llm_base_url)
    _wait_forever()
    bus.stop()
    return 0


def run_voice_text(config: RuntimeConfig) -> int:
    from .voice import VoiceParser

    parser = VoiceParser(ActionMap(config.contracts_dir), node_id=config.perception_node_id,
                         llm_ttl_ms=config.llm_request_ttl_ms, simulated=config.simulated)
    tracker = ContextTracker()
    bus = Bus(config.broker, f"{config.perception_node_id}-voice-text")
    bus.subscribe(pfn.TOPICS["perception"], tracker.update)
    bus.start()
    print("문장을 입력하세요 (빈 줄 무시, Ctrl+C 종료)", flush=True)
    for line in sys.stdin:
        decision = parser.handle(line, context=tracker.current())
        for topic, message in decision.messages:
            bus.publish(topic, message)
        proposal = decision.proposal or {}
        print(f"→ {decision.kind} {proposal.get('device') or ''} {proposal.get('action') or ''} {decision.note}".strip(),
              flush=True)
    bus.stop()
    return 0


def run_perception(config: RuntimeConfig) -> int:
    from .perception import run as run_perception_service

    return run_perception_service(config)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="homemind-runtime")
    parser.add_argument("command", choices=["llm", "voice-text", "perception"])
    parser.add_argument("--config", default=None)
    parser.add_argument("--log-level", default="INFO")
    args, extra = parser.parse_known_args(argv)
    if args.command == "perception":
        # perception has its own flags (--wav, --fast, --device, --listen-mode, --list-devices ...)
        from .perception import main as perception_main

        logging.basicConfig(level=args.log_level, format="%(asctime)s %(levelname)s %(name)s %(message)s")
        return perception_main((["--config", args.config] if args.config else []) + extra)
    if extra:
        parser.error(f"unrecognized arguments: {' '.join(extra)}")
    logging.basicConfig(level=args.log_level, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    config = RuntimeConfig.load(args.config)
    return {"llm": run_llm, "voice-text": run_voice_text, "perception": run_perception}[args.command](config)


if __name__ == "__main__":
    raise SystemExit(main())
