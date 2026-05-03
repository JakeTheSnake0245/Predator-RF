"""Per-peer dedupe, allowlist enforcement, reliable-mode plumbing,
and IP↔RNS loop break in CoTEmitter."""
from __future__ import annotations

import asyncio
import socket

from backend.output.cot_emitter import CoTEmitter
from backend.rns.bridge import RNSCotBridge
from backend.rns.envelope import unwrap_cot, wrap_cot


def test_per_peer_dedupe_isolated_buckets():
    """Spec section C: dedupe LRU is per-peer. Same (uid, ts) from two
    different peers must both be delivered."""
    delivered: list[tuple[bytes, str]] = []
    b = RNSCotBridge(own_hash16="0" * 16,
                     inbound_fn=lambda x, s: delivered.append((x, s)))
    a = wrap_cot(b"<a/>", src_hash16="a" * 16, uid="u", ts_ms=1_700_000_000_000)
    c = wrap_cot(b"<a/>", src_hash16="c" * 16, uid="u", ts_ms=1_700_000_000_000)
    assert b.handle_inbound(a) is True
    assert b.handle_inbound(c) is True
    # Replays from each peer are dropped independently.
    assert b.handle_inbound(a) is False
    assert b.handle_inbound(c) is False
    assert b.deduped == 2
    assert len(delivered) == 2
    stats = b.stats()
    assert stats["peers_seen"] == 2


def test_allowlist_rejects_unknown_peer():
    delivered: list[tuple[bytes, str]] = []
    b = RNSCotBridge(own_hash16="0" * 16,
                     inbound_fn=lambda x, s: delivered.append((x, s)),
                     peer_allowlist=["a" * 16])
    ok = wrap_cot(b"<a/>", src_hash16="a" * 16, uid="u")
    bad = wrap_cot(b"<a/>", src_hash16="b" * 16, uid="u")
    assert b.handle_inbound(ok) is True
    assert b.handle_inbound(bad) is False
    assert b.allowlist_rejected == 1
    assert len(delivered) == 1


def test_publish_reliable_flag_is_propagated():
    """`reliable_default=True` results in the publisher being called
    with `reliable=True` even when the caller doesn't override."""
    seen: list[tuple[bytes, bool]] = []
    b = RNSCotBridge(own_hash16="0" * 16, reliable_default=True,
                     publish_fn=lambda env, rel: seen.append((env, rel)))
    assert b.publish(b"<a/>", "u1") is True
    assert seen and seen[0][1] is True
    assert b.publish(b"<a/>", "u2", reliable=False) is True
    assert seen[1][1] is False


def test_publish_falls_back_to_single_arg_callable():
    """Tests / older publishers may take just `(env)`. Bridge must
    transparently fall back rather than raising TypeError."""
    seen: list[bytes] = []
    b = RNSCotBridge(own_hash16="0" * 16,
                     publish_fn=lambda env: seen.append(env))
    assert b.publish(b"<a/>", "u1") is True
    assert len(seen) == 1


def test_cot_emitter_skips_rns_sourced_track_by_default():
    """IP↔RNS loop break (spec section C). RNS-sourced tracks must NOT
    be re-emitted on the TAK UDP feed unless `set_rns_to_ip_relay(True)`."""
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink.bind(("127.0.0.1", 0))
    port = sink.getsockname()[1]
    em = CoTEmitter(dest_host="127.0.0.1", dest_port=port, enabled=True)
    track = {"emitter_id": "X", "estimated_lat": 1.0, "estimated_lon": 2.0,
             "primary_frequency": 1e8, "observation_count": 1,
             "confidence": 0.9, "source_transport": "rns"}
    report = {"escalate_to_atak": True, "threat_level": "low",
              "summary": ""}
    loop = asyncio.new_event_loop()
    try:
        sent = loop.run_until_complete(em.emit_track(track, report))
    finally:
        loop.close()
    assert sent is False
    em.close()
    sink.close()


def test_cot_emitter_relays_when_rns_to_ip_relay_enabled():
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink.bind(("127.0.0.1", 0))
    port = sink.getsockname()[1]
    em = CoTEmitter(dest_host="127.0.0.1", dest_port=port, enabled=True)
    em.set_rns_to_ip_relay(True)
    track = {"emitter_id": "Y", "estimated_lat": 1.0, "estimated_lon": 2.0,
             "primary_frequency": 1e8, "observation_count": 1,
             "confidence": 0.9, "source_transport": "rns"}
    report = {"escalate_to_atak": True, "threat_level": "low",
              "summary": ""}
    loop = asyncio.new_event_loop()
    try:
        sent = loop.run_until_complete(em.emit_track(track, report))
    finally:
        loop.close()
    assert sent is True
    em.close()
    sink.close()
