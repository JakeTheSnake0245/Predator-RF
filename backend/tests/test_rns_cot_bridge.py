"""Bridge tests: dedupe, loop suppression, fan-out from CoTEmitter."""
from __future__ import annotations

import asyncio
import socket

import pytest

from backend.output.cot_emitter import CoTEmitter, build_cot_xml
from backend.rns.bridge import RNSCotBridge
from backend.rns.envelope import unwrap_cot, wrap_cot


def test_publish_with_no_publish_fn_is_soft_noop():
    b = RNSCotBridge(own_hash16="a" * 16)
    assert b.publish(b"<event/>", "u1") is False


def test_publish_envelope_round_trip():
    captured: list[bytes] = []
    b = RNSCotBridge(own_hash16="b" * 16,
                     publish_fn=captured.append)
    assert b.publish(b"<event/>", "u1") is True
    assert len(captured) == 1
    env = unwrap_cot(captured[0])
    assert env["uid"] == "u1"
    assert env["src"] == "b" * 16
    assert env["xml"] == b"<event/>"


def test_inbound_loop_suppression_drops_own_src():
    delivered: list[tuple[bytes, str]] = []
    b = RNSCotBridge(own_hash16="c" * 16,
                     inbound_fn=lambda x, s: delivered.append((x, s)))
    own = wrap_cot(b"<event/>", src_hash16="c" * 16, uid="u")
    assert b.handle_inbound(own) is False
    assert delivered == []
    assert b.loop_suppressed == 1


def test_inbound_dedupe_drops_repeat():
    delivered: list[tuple[bytes, str]] = []
    b = RNSCotBridge(own_hash16="c" * 16,
                     inbound_fn=lambda x, s: delivered.append((x, s)))
    other = wrap_cot(b"<event/>", src_hash16="d" * 16, uid="u",
                      ts_ms=1_700_000_000_000)
    assert b.handle_inbound(other) is True
    assert b.handle_inbound(other) is False
    assert len(delivered) == 1
    assert b.deduped == 1


def test_cot_emitter_fanout_reaches_bridge():
    """Verifies the same XML the TAK UDP path emits also arrives at the
    RNS bridge — section 5 of the task spec."""
    received: list[tuple[bytes, str]] = []
    bridge = RNSCotBridge(own_hash16="e" * 16,
                          publish_fn=lambda env: received.append(
                              (unwrap_cot(env)["xml"],
                               unwrap_cot(env)["uid"])))
    # Use a local UDP sink so emit_track actually sends.
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink.bind(("127.0.0.1", 0))
    port = sink.getsockname()[1]
    em = CoTEmitter(dest_host="127.0.0.1", dest_port=port,
                    enabled=True, multicast_ttl=1)
    em.attach_fanout(lambda xml, uid: bridge.publish(xml, uid))

    track = {"emitter_id": "EMIT0001", "estimated_lat": 1.0,
             "estimated_lon": 2.0, "primary_frequency": 915_000_000,
             "observation_count": 4, "confidence": 0.9}
    report = {"escalate_to_atak": True, "threat_level": "high",
              "summary": "test"}
    sent = asyncio.get_event_loop().run_until_complete(
        em.emit_track(track, report))
    assert sent is True
    # UDP datagram landed in our sink.
    sink.settimeout(1.0)
    data, _ = sink.recvfrom(65535)
    assert b"<event" in data
    # Bridge fan-out received the same XML.
    assert len(received) == 1
    fan_xml, fan_uid = received[0]
    assert fan_uid == f"PREDATOR.{track['emitter_id']}"
    assert b"<event" in fan_xml
    em.close()
    sink.close()
