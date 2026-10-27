"""Small paho-mqtt v2 wrapper: JSON publish/subscribe, reconnect, QoS per pfn.v1 transport policy."""

from __future__ import annotations

import json
import logging
import threading
import time
from typing import Any, Callable, Mapping

import paho.mqtt.client as mqtt

from .config import BrokerConfig

log = logging.getLogger(__name__)
Handler = Callable[[str, dict[str, Any]], None]


class Bus:
    def __init__(self, broker: BrokerConfig, client_id: str) -> None:
        self.broker = broker
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        if broker.username:
            self.client.username_pw_set(broker.username, broker.password())
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self._outbox: list[tuple[float, str, str, int]] = []  # (deadline_ms, topic, payload, qos)
        self._outbox_lock = threading.Lock()
        self.client.reconnect_delay_set(1, 30)
        self._handlers: dict[str, Handler] = {}
        self.connected = threading.Event()

    def subscribe(self, topic: str, handler: Handler) -> None:
        self._handlers[topic] = handler
        if self.connected.is_set():
            self.client.subscribe(topic, qos=1)

    def start(self, timeout: float = 10.0) -> None:
        self.client.connect_async(self.broker.host, self.broker.port, keepalive=self.broker.keepalive)
        self.client.loop_start()
        if not self.connected.wait(timeout):
            log.warning("MQTT not connected yet to %s:%s; will keep retrying", self.broker.host, self.broker.port)

    def stop(self) -> None:
        self.client.disconnect()
        self.client.loop_stop()

    def publish(self, topic: str, document: Mapping[str, Any], qos: int = 1) -> None:
        payload = json.dumps(document, ensure_ascii=False, separators=(",", ":"))
        if not self.connected.is_set():
            # paho would queue this forever and replay it after a reconnect, so a stale command could run
            # minutes later. Hold it only while it is still meaningful (same rules as the C++ perception node).
            with self._outbox_lock:
                self._outbox.append((_deadline_ms(topic, document), topic, payload, qos))
            log.warning("MQTT disconnected: holding %s until reconnect or expiry", topic)
            return
        info = self.client.publish(topic, payload, qos=qos, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            log.error("publish failed topic=%s rc=%s", topic, info.rc)

    def _flush_outbox(self) -> None:
        now = time.time() * 1000
        with self._outbox_lock:
            held, self._outbox = self._outbox, []
        for deadline, topic, payload, qos in held:
            if deadline < now:
                log.warning("dropped stale %s (held past its deadline)", topic)
                continue
            self.client.publish(topic, payload, qos=qos, retain=False)

    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        del userdata, flags, properties
        if reason_code != 0:
            log.error("MQTT connect failed: %s", reason_code)
            return
        for topic in self._handlers:
            client.subscribe(topic, qos=1)
        self.connected.set()
        log.info("MQTT connected %s:%s", self.broker.host, self.broker.port)
        self._flush_outbox()

    def _on_disconnect(self, client, userdata, flags, reason_code, properties) -> None:
        del client, userdata, flags, properties
        self.connected.clear()
        if reason_code != 0:
            log.warning("MQTT disconnected (%s); reconnecting", reason_code)

    def _on_message(self, client, userdata, message) -> None:
        del client, userdata
        for pattern, handler in self._handlers.items():
            if mqtt.topic_matches_sub(pattern, message.topic):
                try:
                    handler(message.topic, json.loads(message.payload.decode("utf-8")))
                except Exception:  # noqa: BLE001 - one bad message must not stop the node
                    log.exception("handler failed for %s", message.topic)


def _deadline_ms(topic: str, document: Mapping[str, Any]) -> float:
    """Until when a message held during a disconnect may still be sent."""

    now = time.time() * 1000
    if topic == "pfn/v1/ai/perception":
        return float((document.get("attributes") or {}).get("valid_until_ms") or now + 10_000)
    if topic == "pfn/v1/ai/llm/request":
        return float(document.get("expires_at_ms") or now + 10_000)
    return now + 10_000  # task requests and everything else: 10 s
