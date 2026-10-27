"""Messages published while MQTT is down are held only while still meaningful (no stale replay)."""

import json
import time

from homemind_runtime.config import BrokerConfig
from homemind_runtime.mqtt import Bus


class _Info:
    rc = 0


class _FakeClient:
    def __init__(self):
        self.sent = []

    def publish(self, topic, payload, qos=0, retain=False):
        self.sent.append((topic, json.loads(payload)))
        return _Info()

    def subscribe(self, *_args, **_kwargs):
        pass


def _bus():
    bus = Bus(BrokerConfig(), "test-outbox")
    bus.client = _FakeClient()
    return bus


def test_held_while_disconnected_then_sent_in_order():
    bus = _bus()
    now = time.time() * 1000
    bus.publish("pfn/v1/core/task/request", {"n": 1})
    bus.publish("pfn/v1/ai/llm/request", {"n": 2, "expires_at_ms": now + 60_000})
    assert bus.client.sent == []
    bus._on_connect(bus.client, None, None, 0, None)
    assert [doc["n"] for _, doc in bus.client.sent] == [1, 2]


def test_expired_messages_are_dropped():
    bus = _bus()
    now = time.time() * 1000
    bus.publish("pfn/v1/ai/llm/request", {"n": 1, "expires_at_ms": now - 1})
    bus.publish("pfn/v1/ai/perception", {"n": 2, "attributes": {"valid_until_ms": now - 1}})
    bus._outbox.append((now - 1, "pfn/v1/core/task/request", json.dumps({"n": 3}), 1))  # an old task request
    bus.publish("pfn/v1/ai/perception", {"n": 4, "attributes": {"valid_until_ms": now + 60_000}})
    bus._on_connect(bus.client, None, None, 0, None)
    assert [doc["n"] for _, doc in bus.client.sent] == [4]


def test_connected_publishes_directly_and_disconnect_holds_again():
    bus = _bus()
    bus._on_connect(bus.client, None, None, 0, None)
    bus.publish("pfn/v1/core/task/request", {"n": 1})
    bus._on_disconnect(bus.client, None, None, 7, None)
    bus.publish("pfn/v1/core/task/request", {"n": 2})
    assert [doc["n"] for _, doc in bus.client.sent] == [1]
