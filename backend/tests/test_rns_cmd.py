"""Tests for roadmap #6 — RNS commanding wrapper.

Coverage matrix:
  * cmd envelope: round-trip, version mismatch, missing fields,
    bad content-type, tx.* hard-reject (wrap + unwrap), allowlist
    class enforcement, oversized → Link prediction, malformed payload.
  * RNSCmdBridge: publish→handle round-trip, loop suppression,
    per-peer dedupe, peer allowlist, dispatcher accept/reject path,
    dispatcher exception → counted as rejection, no-publisher
    short-circuit, no-dispatcher graceful drop.
  * KujhadRNSClient: send_{tune,scan,mission}_command produce wire
    bodies byte-identical to `KujhadClient`'s payload dicts — the
    parity guarantee Roadmap #6 promises.
"""
from __future__ import annotations

import json
import unittest

from backend.rns.cmd import (
    CMD_CONTENT_TYPE,
    CMD_ENVELOPE_VERSION,
    CmdEnvelopeError,
    PACKET_MDU,
    unwrap_cmd,
    will_use_link,
    wrap_cmd,
)
from backend.rns.cmd_handler import RNSCmdBridge
from backend.coordination.kujhad_rns_client import KujhadRNSClient


H16_A = "a" * 16
H16_B = "b" * 16
H16_C = "c" * 16


class CmdEnvelopeTests(unittest.TestCase):

    def test_round_trip(self):
        cmd = {"class": "tune", "action": "set",
               "args": {"frequencyHz": 433_920_000.0, "vfo": "VFO A"}}
        env = wrap_cmd(cmd, src_hash16=H16_A, uid="u1")
        out = unwrap_cmd(env)
        self.assertEqual(out["v"], CMD_ENVELOPE_VERSION)
        self.assertEqual(out["src"], H16_A)
        self.assertEqual(out["uid"], "u1")
        self.assertEqual(out["ct"], CMD_CONTENT_TYPE)
        self.assertEqual(out["cmd"], cmd)

    def test_tx_class_rejected_on_wrap(self):
        for cls in ("tx", "tx.fire", "TX.something", "Tx_relay"):
            with self.assertRaises(CmdEnvelopeError):
                wrap_cmd({"class": cls, "action": "set", "args": {}},
                         src_hash16=H16_A, uid="u")

    def test_tx_class_rejected_on_unwrap(self):
        # Synthesise an envelope that bypassed wrap-time validation by
        # hand-rolling cbor with a tx.* class. Unwrap MUST still reject.
        import cbor2
        bad = cbor2.dumps({
            "v": CMD_ENVELOPE_VERSION, "ts": 1, "src": H16_A, "uid": "u",
            "ct": CMD_CONTENT_TYPE, "z": 0,
            "p": json.dumps({"class": "tx.fire", "action": "go",
                             "args": {}}).encode("utf-8"),
        })
        with self.assertRaises(CmdEnvelopeError):
            unwrap_cmd(bad)

    def test_unknown_class_rejected(self):
        with self.assertRaises(CmdEnvelopeError):
            wrap_cmd({"class": "delete_everything", "action": "now",
                      "args": {}},
                     src_hash16=H16_A, uid="u")

    def test_missing_fields_rejected(self):
        with self.assertRaises(CmdEnvelopeError):
            wrap_cmd({"class": "tune"}, src_hash16=H16_A, uid="u")
        with self.assertRaises(CmdEnvelopeError):
            wrap_cmd({"action": "set", "args": {}},
                     src_hash16=H16_A, uid="u")

    def test_bad_src_hash(self):
        with self.assertRaises(CmdEnvelopeError):
            wrap_cmd({"class": "tune", "action": "set", "args": {}},
                     src_hash16="short", uid="u")

    def test_bad_uid(self):
        with self.assertRaises(CmdEnvelopeError):
            wrap_cmd({"class": "tune", "action": "set", "args": {}},
                     src_hash16=H16_A, uid="")

    def test_version_mismatch(self):
        import cbor2
        bad = cbor2.dumps({
            "v": 99, "ts": 1, "src": H16_A, "uid": "u",
            "ct": CMD_CONTENT_TYPE, "z": 0,
            "p": json.dumps({"class": "ping", "action": "p",
                             "args": {}}).encode("utf-8"),
        })
        with self.assertRaises(CmdEnvelopeError):
            unwrap_cmd(bad)

    def test_wrong_content_type(self):
        import cbor2
        bad = cbor2.dumps({
            "v": CMD_ENVELOPE_VERSION, "ts": 1, "src": H16_A, "uid": "u",
            "ct": "cot/xml", "z": 0,
            "p": json.dumps({"class": "ping", "action": "p",
                             "args": {}}).encode("utf-8"),
        })
        with self.assertRaises(CmdEnvelopeError):
            unwrap_cmd(bad)

    def test_link_prediction(self):
        # Small command → Packet.
        small = wrap_cmd({"class": "tune", "action": "set",
                          "args": {"frequencyHz": 1.0, "vfo": "A"}},
                         src_hash16=H16_A, uid="u")
        self.assertFalse(will_use_link(small))
        # Reliable forces Link regardless.
        self.assertTrue(will_use_link(small, reliable=True))
        # Oversized payload → Link. Use os.urandom-derived hex so zlib
        # can't compress the payload below PACKET_MDU and mask the test.
        import os
        big_args = {"bands": [{"start": i, "end": i + 1,
                               "label": os.urandom(48).hex()}
                              for i in range(10)]}
        big = wrap_cmd({"class": "mission", "action": "setSearchBands",
                        "args": big_args}, src_hash16=H16_A, uid="u")
        self.assertGreater(len(big), PACKET_MDU)
        self.assertTrue(will_use_link(big))


class RNSCmdBridgeTests(unittest.TestCase):

    def _make_pair(self, allowlist=None):
        """Two bridges (A=Controller, B=Device) with A's publish
        directly fed into B's handle_inbound. Returns (A, B, dispatched)
        where `dispatched` is a list captured by the dispatcher."""
        dispatched = []

        def dispatch(cmd, src, uid):
            dispatched.append((cmd, src, uid))
            return True

        a = RNSCmdBridge(own_hash16=H16_A)
        b = RNSCmdBridge(own_hash16=H16_B,
                         dispatch_fn=dispatch,
                         peer_allowlist=allowlist)
        a.set_publish_fn(lambda env, rel=False: b.handle_inbound(env))
        return a, b, dispatched

    def test_publish_to_dispatch_round_trip(self):
        a, b, dispatched = self._make_pair()
        ok = a.publish({"class": "tune", "action": "set",
                        "args": {"frequencyHz": 1.0, "vfo": "A"}}, uid="u1")
        self.assertTrue(ok)
        self.assertEqual(len(dispatched), 1)
        self.assertEqual(dispatched[0][0]["class"], "tune")
        self.assertEqual(dispatched[0][1], H16_A.lower())
        self.assertEqual(dispatched[0][2], "u1")
        self.assertEqual(b.dispatched, 1)

    def test_loop_suppression(self):
        # A publish where src == own_hash16 must drop, never dispatch.
        dispatched = []
        b = RNSCmdBridge(own_hash16=H16_A,
                         dispatch_fn=lambda *a: dispatched.append(a) or True)
        env = wrap_cmd({"class": "ping", "action": "p", "args": {}},
                       src_hash16=H16_A, uid="loop1")
        b.handle_inbound(env)
        self.assertEqual(dispatched, [])
        self.assertEqual(b.loop_suppressed, 1)

    def test_dedupe_within_one_second(self):
        a, b, dispatched = self._make_pair()
        cmd = {"class": "ping", "action": "p", "args": {}}
        env = wrap_cmd(cmd, src_hash16=H16_A, uid="u-dup", ts_ms=1_000_000)
        env_dup = wrap_cmd(cmd, src_hash16=H16_A, uid="u-dup",
                           ts_ms=1_000_500)  # same wall-clock second
        env_late = wrap_cmd(cmd, src_hash16=H16_A, uid="u-dup",
                            ts_ms=1_002_000)  # 2 s later → ok
        b.handle_inbound(env)
        b.handle_inbound(env_dup)
        b.handle_inbound(env_late)
        self.assertEqual(len(dispatched), 2)
        self.assertEqual(b.deduped, 1)

    def test_allowlist_blocks_unknown_peer(self):
        a, b, dispatched = self._make_pair(allowlist=[H16_C])
        a.publish({"class": "ping", "action": "p", "args": {}}, uid="u")
        self.assertEqual(dispatched, [])
        self.assertEqual(b.allowlist_rejected, 1)

    def test_allowlist_passes_known_peer(self):
        a, b, dispatched = self._make_pair(allowlist=[H16_A])
        a.publish({"class": "ping", "action": "p", "args": {}}, uid="u")
        self.assertEqual(len(dispatched), 1)

    def test_dispatcher_rejection_is_counted(self):
        def reject(cmd, src, uid):
            return False
        b = RNSCmdBridge(own_hash16=H16_B, dispatch_fn=reject)
        env = wrap_cmd({"class": "tune", "action": "set",
                        "args": {"frequencyHz": 1.0, "vfo": "A"}},
                       src_hash16=H16_A, uid="u")
        out = b.handle_inbound(env)
        self.assertFalse(out)
        self.assertEqual(b.dispatch_rejected, 1)
        self.assertEqual(b.dispatched, 0)

    def test_dispatcher_exception_is_swallowed_and_counted(self):
        def boom(cmd, src, uid):
            raise RuntimeError("device hardware busy")
        b = RNSCmdBridge(own_hash16=H16_B, dispatch_fn=boom)
        env = wrap_cmd({"class": "tune", "action": "set",
                        "args": {"frequencyHz": 1.0, "vfo": "A"}},
                       src_hash16=H16_A, uid="u")
        out = b.handle_inbound(env)
        self.assertFalse(out)
        self.assertEqual(b.dispatch_rejected, 1)

    def test_publish_without_publisher_returns_false(self):
        a = RNSCmdBridge(own_hash16=H16_A)  # no publish_fn
        ok = a.publish({"class": "ping", "action": "p", "args": {}},
                       uid="u")
        self.assertFalse(ok)
        self.assertEqual(a.published, 0)

    def test_publish_refuses_tx_at_caller_side(self):
        sent = []
        a = RNSCmdBridge(own_hash16=H16_A,
                         publish_fn=lambda env, rel=False: sent.append(env))
        ok = a.publish({"class": "tx.fire", "action": "go", "args": {}},
                       uid="u")
        self.assertFalse(ok)
        self.assertEqual(sent, [])  # nothing leaked to publish_fn
        self.assertEqual(a.envelope_errors, 1)

    def test_no_dispatcher_drops_gracefully(self):
        b = RNSCmdBridge(own_hash16=H16_B)  # no dispatch_fn
        env = wrap_cmd({"class": "ping", "action": "p", "args": {}},
                       src_hash16=H16_A, uid="u")
        out = b.handle_inbound(env)
        self.assertFalse(out)
        # Counted as received but not dispatched.
        self.assertEqual(b.received, 1)
        self.assertEqual(b.dispatched, 0)

    def test_packet_src_overrides_envelope_src(self):
        """When the daemon supplies a packet-derived src, that wins
        over the envelope's self-declared src and the (uid, sec)
        dedupe key is computed off the packet src — so an attacker
        replaying our own envelope from peer C cannot impersonate
        peer A."""
        seen = []
        b = RNSCmdBridge(own_hash16=H16_B,
                         dispatch_fn=lambda c, s, u: seen.append(s) or True)
        env = wrap_cmd({"class": "ping", "action": "p", "args": {}},
                       src_hash16=H16_A, uid="u1")
        # Daemon hands us packet src = C (real RNS source), envelope
        # claims A. Mismatch → drop, count as envelope error.
        out = b.handle_inbound(env, src_hash16=H16_C)
        self.assertFalse(out)
        self.assertEqual(seen, [])
        self.assertEqual(b.envelope_errors, 1)

    def test_packet_src_only_path_works(self):
        """When packet src matches envelope src, dispatch is allowed
        and the dispatcher receives the packet src."""
        seen = []
        b = RNSCmdBridge(own_hash16=H16_B,
                         dispatch_fn=lambda c, s, u: seen.append(s) or True)
        env = wrap_cmd({"class": "ping", "action": "p", "args": {}},
                       src_hash16=H16_A, uid="u-ok")
        out = b.handle_inbound(env, src_hash16=H16_A)
        self.assertTrue(out)
        self.assertEqual(seen, [H16_A.lower()])

    def test_bad_envelope_is_counted(self):
        b = RNSCmdBridge(own_hash16=H16_B)
        out = b.handle_inbound(b"\x00\x01\x02 not cbor at all")
        self.assertFalse(out)
        self.assertEqual(b.envelope_errors, 1)


class KujhadRNSClientParityTests(unittest.TestCase):
    """Asserts that the wire bodies emitted by KujhadRNSClient are
    byte-identical to the JSON dicts KujhadClient sends over HTTP. This
    is the load-bearing parity guarantee of roadmap #6 — Device-side
    dispatchers can use one code path for both transports."""

    def setUp(self):
        # Capture-only bridge: snag every published envelope so we can
        # decode it and compare against the HTTP-shape dict.
        self.captured = []

        def capture(env, rel=False):
            # The bridge will only call this with a valid envelope.
            self.captured.append((env, rel))

        self.bridge = RNSCmdBridge(own_hash16=H16_A, publish_fn=capture)
        self.client = KujhadRNSClient(self.bridge)

    def _last_cmd(self):
        env, _rel = self.captured[-1]
        return unwrap_cmd(env)["cmd"]

    def test_tune_parity(self):
        ok = self.client.send_tune_command(H16_B, 433_920_000)
        self.assertTrue(ok)
        # The exact dict KujhadClient.send_tune_command would POST.
        expected = {
            "class": "tune", "action": "set",
            "args": {"frequencyHz": 433_920_000.0, "vfo": "VFO A"},
        }
        self.assertEqual(self._last_cmd(), expected)

    def test_scan_parity_start(self):
        self.client.send_scan_command(H16_B, 100e6, 200e6, dwell_ms=750)
        expected = {
            "class": "scan", "action": "start",
            "args": {"startHz": 100e6, "endHz": 200e6, "dwellMs": 750},
        }
        self.assertEqual(self._last_cmd(), expected)

    def test_scan_parity_stop(self):
        self.client.send_scan_command(H16_B, 100e6, 200e6, start=False)
        self.assertEqual(self._last_cmd()["action"], "stop")

    def test_mission_parity(self):
        self.client.send_mission_command(H16_B, "setMode",
                                         {"mode": "scan"})
        self.assertEqual(self._last_cmd(), {
            "class": "mission", "action": "setMode",
            "args": {"mode": "scan"},
        })

    def test_bad_peer_hash_rejected(self):
        ok = self.client.send_tune_command("short", 1.0)
        self.assertFalse(ok)
        self.assertEqual(self.captured, [])


class UnicastRoutingTests(unittest.TestCase):
    """Roadmap #6 release-blocker fix: cmd publish MUST be strict
    per-peer unicast. A `tune` for peer A must NOT execute on peer B
    just because B is in the allowlist."""

    def test_publish_passes_peer_h16_to_publisher(self):
        captured = []
        b = RNSCmdBridge(
            own_hash16=H16_A,
            publish_fn=lambda env, rel, peer: captured.append(peer) or True,
        )
        b.publish({"class": "ping", "action": "p", "args": {}},
                  uid="u", peer_h16=H16_B)
        self.assertEqual(captured, [H16_B])

    def test_publish_fn_returning_false_propagates(self):
        # Production daemon returns False on unknown peer. Bridge must
        # propagate that as publish=False AND not bump `published`.
        b = RNSCmdBridge(
            own_hash16=H16_A,
            publish_fn=lambda env, rel, peer: False,
        )
        ok = b.publish({"class": "ping", "action": "p", "args": {}},
                       uid="u", peer_h16=H16_B)
        self.assertFalse(ok)
        self.assertEqual(b.published, 0)

    def test_kujhad_client_targets_named_peer(self):
        captured = []
        b = RNSCmdBridge(
            own_hash16=H16_A,
            publish_fn=lambda env, rel, peer: captured.append(peer) or True,
        )
        client = KujhadRNSClient(b)
        client.send_tune_command(H16_B, 100e6)
        client.send_tune_command(H16_C, 200e6)
        self.assertEqual(captured, [H16_B, H16_C])

    def test_daemon_publish_envelope_cmd_rejects_no_peer(self):
        """Stub-mode daemon should refuse a None peer_h16 even when
        RNS isn't available — the gate runs before the RNS branch."""
        import tempfile
        from backend.rns.daemon import RNSDaemon
        d = RNSDaemon(state_dir=tempfile.mkdtemp(prefix="prdrns_t_"))
        # Stub mode: _HAVE_RNS may be False; either way the function
        # must return False when peer_h16 is None.
        out = d._publish_envelope_cmd(b"\x00" * 10, peer_h16=None)
        self.assertFalse(out)


class DaemonWireUpTests(unittest.TestCase):
    """Pin the cmd.v1 ↔ cot.v1 wire-up gate in `RNSDaemon.start()`. The
    flag must default OFF (cmd.v1 is opt-in until field-tested) and
    flipping it on must register a second IN destination AND bind the
    Controller-side cmd publish path. We exercise the daemon in stub
    mode (rns module absent) so these tests don't need a working RNS
    install — the relevant gates run before the RNS-only branches."""

    def _fresh_daemon(self, **cfg_overrides):
        import os
        import tempfile
        from backend.rns.daemon import RNSDaemon
        tmp = tempfile.mkdtemp(prefix="prdrns_t_")
        class FakeCot:
            def __init__(self):
                self.pf = None
            def set_publish_fn(self, fn):
                self.pf = fn
            def stats(self):
                return {"published": 0, "received": 0}
        cot = FakeCot()
        cmd = RNSCmdBridge(own_hash16=H16_A)
        d = RNSDaemon(state_dir=tmp, cot_bridge=cot, cmd_bridge=cmd)
        d.config.update(cfg_overrides)
        return d, cot, cmd, tmp

    def test_flag_off_does_not_bind_cmd_publish(self):
        d, cot, cmd, _ = self._fresh_daemon(cmd_v1_enabled=False)
        d.start()
        try:
            # cot bridge is always bound; cmd bridge must NOT be bound.
            self.assertIsNone(cmd._publish_fn)
        finally:
            d.stop()

    def test_flag_on_binds_cmd_publish(self):
        d, cot, cmd, _ = self._fresh_daemon(cmd_v1_enabled=True)
        d.start()
        try:
            self.assertIsNotNone(cmd._publish_fn)
            # The bound function should be the daemon's cmd publisher.
            self.assertEqual(cmd._publish_fn,
                             d._publish_envelope_cmd)
        finally:
            d.stop()

    def test_stop_unbinds_both_bridges(self):
        d, cot, cmd, _ = self._fresh_daemon(cmd_v1_enabled=True)
        d.start()
        d.stop()
        self.assertIsNone(cmd._publish_fn)


if __name__ == "__main__":
    unittest.main()
