"""Unit tests for CustodyElector — covers scoring weights, hard gates,
handover overlap, stand-down generation, and stats.

These tests deliberately do not exercise async / I/O paths — the elector
is pure logic. Integration with TrackManager / DecisionEngine / SSE is
covered by the wider system tests; here we verify the contract.
"""
import time
import unittest

from backend.coordination.custody_election import (
    CustodyElector,
    CustodyDecision,
    DEFAULT_K_TOTAL,
)
from backend.models.emitter_track import EmitterTrack, TrackState
from backend.models.sensor_node import SensorNodeTrust


def _make_node(node_id: str,
               *,
               gps: tuple = (37.0, -122.0),
               gps_updated_ns: int = None,
               gps_synced: bool = True,
               trust: float = 0.8,
               sensitivity_trust: float = 0.9,
               thermal: bool = False,
               available_decoders=None,
               hardware_code: str = "rtlsdr") -> SensorNodeTrust:
    """Build a SensorNodeTrust without going through the full hardware-
    capability lookup chain (which depends on numpy in some envs).
    We force the relevant trust fields directly so scoring is
    deterministic."""
    n = SensorNodeTrust(
        node_id=node_id,
        hardware_code=hardware_code,
        location_gps=gps,
        location_gps_updated_ns=gps_updated_ns
            if gps_updated_ns is not None else time.time_ns(),
        gps_synchronized=gps_synced,
        base_trust=trust,
        sensitivity_trust=sensitivity_trust,
        thermal_throttling_active=thermal,
        available_decoders=list(available_decoders or []),
    )
    # Pin the trust components after __post_init__ so capability lookup
    # (which may set them to its own values) doesn't fight the test.
    n.sensitivity_trust = sensitivity_trust
    n.frequency_stability_trust = 0.9
    n.timing_stability_trust = 0.9
    n.thermal_throttling_active = thermal
    return n


def _make_track(*,
                primary_freq: float = 462_675_000.0,
                threat: str = "low",
                protocol: str = None,
                detecting_nodes=None,
                lat: float = None,
                lon: float = None) -> EmitterTrack:
    t = EmitterTrack(
        primary_frequency=primary_freq,
        threat_level=threat,
        protocol=protocol,
        estimated_lat=lat,
        estimated_lon=lon,
    )
    t.detecting_nodes = list(detecting_nodes or [])
    t.state = TrackState.TRACKING
    return t


class TestBasicElection(unittest.TestCase):

    def test_picks_highest_trust_when_all_else_equal(self):
        elector = CustodyElector()
        nodes = [
            _make_node("A", trust=0.5),
            _make_node("B", trust=0.9),
            _make_node("C", trust=0.7),
        ]
        track = _make_track()
        d = elector.elect(track, nodes, now_ns=1_000_000_000)
        self.assertEqual(d.primary, "B")
        # Backups ordered by score, capped at K-1 = 2.
        self.assertEqual(len(d.backups), DEFAULT_K_TOTAL - 1)
        self.assertEqual(d.backups[0], "C")
        self.assertEqual(d.tasked_nodes, ["B", "C", "A"])

    def test_no_eligible_nodes_returns_no_primary(self):
        elector = CustodyElector()
        # High-threat + no GPS sync → all hard-gated.
        nodes = [_make_node("A", gps_synced=False)]
        track = _make_track(threat="critical")
        d = elector.elect(track, nodes)
        self.assertIsNone(d.primary)
        self.assertEqual(d.backups, [])
        self.assertEqual(d.stand_down, [])
        self.assertIn("no eligible", d.reason)
        self.assertEqual(elector.elections_no_eligible_node, 1)

    def test_empty_node_list(self):
        elector = CustodyElector()
        d = elector.elect(_make_track(), [])
        self.assertIsNone(d.primary)
        self.assertEqual(d.tasked_nodes, [])

    def test_k_total_cap_respected(self):
        elector = CustodyElector(k_total=2)
        nodes = [_make_node(f"N{i}", trust=0.8) for i in range(5)]
        d = elector.elect(_make_track(), nodes)
        self.assertEqual(len(d.backups), 1)            # k=2 → 1 backup
        self.assertEqual(len(d.tasked_nodes), 2)


class TestHardGates(unittest.TestCase):

    def test_high_threat_demands_gps_sync(self):
        elector = CustodyElector()
        good = _make_node("good", gps_synced=True)
        bad = _make_node("bad", gps_synced=False)
        track = _make_track(threat="high")
        d = elector.elect(track, [bad, good])
        self.assertEqual(d.primary, "good")
        # 'bad' is in scores with a rejection reason and total=0.
        bad_score = next(s for s in d.scores if s.node_id == "bad")
        self.assertEqual(bad_score.total, 0.0)
        self.assertIn("gps_sync", bad_score.rejected_reason)

    def test_stale_gps_blocks_high_threat(self):
        elector = CustodyElector(stale_gps_after_s=60.0)
        # Use a wall clock far enough in the future that subtracting
        # 600 s × 1e9 ns stays positive — otherwise gps_updated_ns
        # goes negative and the > 0 guard in _hard_gate skips the
        # stale check, masking the regression we want to catch.
        now = 2_000_000_000_000_000_000
        # Node fixed 10 minutes ago — stale.
        stale = _make_node("stale",
                            gps_updated_ns=now - int(600 * 1e9),
                            gps_synced=True)
        fresh = _make_node("fresh",
                            gps_updated_ns=now - int(5 * 1e9),
                            gps_synced=True)
        d = elector.elect(_make_track(threat="critical"),
                           [stale, fresh], now_ns=now)
        self.assertEqual(d.primary, "fresh")
        stale_score = next(s for s in d.scores if s.node_id == "stale")
        self.assertIn("stale", stale_score.rejected_reason)

    def test_missing_decoder_hard_gates(self):
        elector = CustodyElector()
        no_p25 = _make_node("rtl",
                             available_decoders=["rtl_433", "ads-b"])
        has_p25 = _make_node("hackrf",
                              available_decoders=["p25", "rtl_433"])
        track = _make_track(protocol="P25")
        d = elector.elect(track, [no_p25, has_p25])
        self.assertEqual(d.primary, "hackrf")
        no_p25_score = next(s for s in d.scores if s.node_id == "rtl")
        self.assertIn("missing_decoder", no_p25_score.rejected_reason)

    def test_unknown_protocol_does_not_gate(self):
        # When track.protocol is None, decoder gate is bypassed.
        elector = CustodyElector()
        n = _make_node("any", available_decoders=["rtl_433"])
        d = elector.elect(_make_track(protocol=None), [n])
        self.assertEqual(d.primary, "any")

    def test_node_without_capability_probe_not_gated(self):
        # available_decoders empty → we don't know capabilities, fall
        # back to soft scoring instead of hard-gating the node out.
        elector = CustodyElector()
        n = _make_node("untested", available_decoders=[])
        d = elector.elect(_make_track(protocol="P25"), [n])
        self.assertEqual(d.primary, "untested")


class TestSoftScoring(unittest.TestCase):

    def test_thermal_throttle_demotes(self):
        elector = CustodyElector()
        cool = _make_node("cool", trust=0.7, thermal=False)
        hot = _make_node("hot", trust=0.95, thermal=True)
        d = elector.elect(_make_track(), [cool, hot])
        # Even though hot has higher base trust, the 0.5x thermal
        # multiplier should let cool win.
        self.assertEqual(d.primary, "cool")

    def test_distance_picks_closer_node(self):
        elector = CustodyElector()
        # Track at (37.0, -122.0); near=0.01° away, far=1°.
        near = _make_node("near", gps=(37.01, -122.0), trust=0.7)
        far = _make_node("far", gps=(38.0, -122.0), trust=0.7)
        track = _make_track(lat=37.0, lon=-122.0)
        d = elector.elect(track, [near, far])
        self.assertEqual(d.primary, "near")

    def test_load_spreads_custody(self):
        elector = CustodyElector()
        a = _make_node("A", trust=0.8)
        b = _make_node("B", trust=0.8)
        # A has 5 active custody assignments; B has 0. With the load
        # weight at 0.10, the load delta (1.0 - 1/6 = 0.833) yields
        # ~0.083 additional weighted score for B — enough to flip
        # ties when other components are equal.
        d = elector.elect(_make_track(), [a, b],
                           node_loads={"A": 5, "B": 0})
        self.assertEqual(d.primary, "B")

    def test_detecting_node_gets_snr_bonus(self):
        elector = CustodyElector()
        heard = _make_node("heard", trust=0.7)
        not_heard = _make_node("not_heard", trust=0.7)
        track = _make_track(detecting_nodes=["heard"])
        d = elector.elect(track, [not_heard, heard])
        self.assertEqual(d.primary, "heard")


class TestHandover(unittest.TestCase):

    def test_no_handover_when_primary_unchanged(self):
        elector = CustodyElector()
        nodes = [_make_node("A", trust=0.9), _make_node("B", trust=0.6)]
        track = _make_track()
        d1 = elector.elect(track, nodes, now_ns=1_000)
        d2 = elector.elect(track, nodes, now_ns=2_000)
        self.assertEqual(d2.primary, "A")
        self.assertIsNone(d2.handover_from)
        self.assertEqual(d2.handover_until_ns, 0)
        self.assertFalse(d2.is_handover())

    def test_handover_keeps_old_primary_in_tasked(self):
        elector = CustodyElector(handover_overlap_s=15.0)
        track = _make_track()
        # First election: A wins.
        nodes_v1 = [_make_node("A", trust=0.9), _make_node("B", trust=0.6)]
        elector.elect(track, nodes_v1, now_ns=1_000_000_000)
        # Second election with new geometry: B is now better.
        nodes_v2 = [_make_node("A", trust=0.6), _make_node("B", trust=0.95)]
        d2 = elector.elect(track, nodes_v2, now_ns=2_000_000_000)
        self.assertEqual(d2.primary, "B")
        self.assertEqual(d2.handover_from, "A")
        self.assertIn("A", d2.tasked_nodes)
        self.assertIn("B", d2.tasked_nodes)
        # Overlap deadline = now + 15s
        expected = 2_000_000_000 + int(15.0 * 1e9)
        self.assertEqual(d2.handover_until_ns, expected)
        self.assertEqual(elector.elections_with_handover, 1)

    def test_handover_overlap_persists_across_multiple_elections(self):
        # Regression test for the architect-flagged bug: previously,
        # handover_from was only set on the EXACT election where the
        # primary changed and dropped on the next tick. The overlap
        # must span every re-election that falls inside the deadline.
        # Uses k_total=2 + 3 nodes so the demoted node A is NOT
        # automatically a backup — that way we can prove A stays in
        # tasked_nodes via the handover mechanism specifically, then
        # exits cleanly when the deadline expires.
        elector = CustodyElector(k_total=2, handover_overlap_s=15.0)
        track = _make_track()
        # T=0: A is primary, C is backup.
        nodes_a = [_make_node("A", trust=0.9),
                   _make_node("B", trust=0.5),
                   _make_node("C", trust=0.7)]
        elector.elect(track, nodes_a, now_ns=1_000_000_000)
        # T=1s: B and C overtake A; B primary, C backup. A handover
        # window for A starts.
        nodes_b = [_make_node("A", trust=0.4),
                   _make_node("B", trust=0.95),
                   _make_node("C", trust=0.85)]
        d_handover = elector.elect(track, nodes_b, now_ns=2_000_000_000)
        self.assertEqual(d_handover.primary, "B")
        self.assertEqual(d_handover.handover_from, "A")
        self.assertIn("A", d_handover.tasked_nodes)
        deadline = d_handover.handover_until_ns
        # T=5s: re-election BEFORE deadline; primary unchanged. A
        # would normally fall out (k_total=2, A not a backup), but
        # the inherited handover keeps A tasked.
        d_mid = elector.elect(track, nodes_b, now_ns=5_000_000_000)
        self.assertEqual(d_mid.primary, "B")
        self.assertEqual(d_mid.handover_from, "A")
        self.assertIn("A", d_mid.tasked_nodes)
        # Deadline must NOT be reset by the inherited handover —
        # otherwise a long-running stable primary keeps the outgoing
        # node tasked forever.
        self.assertEqual(d_mid.handover_until_ns, deadline)
        # T=20s: PAST deadline. A is no longer in tasked_nodes (it's
        # not a top-K node and the overlap expired) and shows up in
        # stand_down so AutoTasker can release it.
        d_after = elector.elect(track, nodes_b, now_ns=22_000_000_000)
        self.assertIsNone(d_after.handover_from)
        self.assertNotIn("A", d_after.tasked_nodes)
        self.assertIn("A", d_after.stand_down)

    def test_handover_skipped_when_old_primary_disappeared(self):
        # Old primary gone from available_nodes → no overlap, just
        # straight cutover. Otherwise we'd task a phantom node.
        elector = CustodyElector()
        track = _make_track()
        elector.elect(track, [_make_node("A", trust=0.9)],
                       now_ns=1_000_000_000)
        d2 = elector.elect(track, [_make_node("B", trust=0.9)],
                            now_ns=2_000_000_000)
        self.assertEqual(d2.primary, "B")
        self.assertIsNone(d2.handover_from)
        self.assertNotIn("A", d2.tasked_nodes)


class TestStandDown(unittest.TestCase):

    def test_stand_down_lists_nodes_no_longer_tasked(self):
        elector = CustodyElector(k_total=2)
        track = _make_track()
        # First: A primary, B backup. tasked = [A, B].
        elector.elect(track, [
            _make_node("A", trust=0.9),
            _make_node("B", trust=0.7),
        ], now_ns=1_000_000_000)
        # Second: C arrives with high trust + B drops trust. New
        # tasked = [C, A] (handover B → C? no, A is primary either way).
        # Actually: scoring picks C primary, A backup; B falls off.
        # B should appear in stand_down.
        d2 = elector.elect(track, [
            _make_node("A", trust=0.85),
            _make_node("B", trust=0.4),
            _make_node("C", trust=0.95),
        ], now_ns=2_000_000_000)
        self.assertIn("B", d2.stand_down)
        self.assertNotIn("A", d2.stand_down)
        self.assertNotIn("C", d2.stand_down)


class TestOnChangeCallback(unittest.TestCase):

    def test_callback_fires_on_primary_change_only(self):
        events = []
        elector = CustodyElector(on_change=lambda d: events.append(d))
        track = _make_track()
        # First election → fires (None → A is a change).
        elector.elect(track, [_make_node("A", trust=0.9)], now_ns=1_000)
        # Re-confirmation with same primary → does NOT fire.
        elector.elect(track, [_make_node("A", trust=0.9)], now_ns=2_000)
        # Handover → fires.
        elector.elect(track, [_make_node("A", trust=0.5),
                              _make_node("B", trust=0.95)], now_ns=3_000)
        self.assertEqual(len(events), 2)
        self.assertEqual(events[0].primary, "A")
        self.assertEqual(events[1].primary, "B")
        self.assertEqual(events[1].handover_from, "A")

    def test_callback_exception_does_not_break_election(self):
        def boom(_):
            raise RuntimeError("operator UI crashed")
        elector = CustodyElector(on_change=boom)
        d = elector.elect(_make_track(),
                           [_make_node("A", trust=0.9)],
                           now_ns=1_000)
        self.assertEqual(d.primary, "A")  # election still completed


class TestStateManagement(unittest.TestCase):

    def test_forget_releases_cache(self):
        elector = CustodyElector()
        track = _make_track()
        elector.elect(track, [_make_node("A", trust=0.9)])
        self.assertIsNotNone(elector.last_decision(track.emitter_id))
        elector.forget(track.emitter_id)
        self.assertIsNone(elector.last_decision(track.emitter_id))

    def test_stats_track_election_counts(self):
        elector = CustodyElector()
        track = _make_track()
        elector.elect(track, [_make_node("A", trust=0.9)], now_ns=1_000)
        elector.elect(track, [_make_node("B", trust=0.9)], now_ns=2_000)
        s = elector.stats()
        self.assertEqual(s["elections_total"], 2)
        # Old primary "A" disappeared from available_nodes on the
        # second election, so handover is suppressed (see
        # test_handover_skipped_when_old_primary_disappeared).
        self.assertEqual(s["elections_with_handover"], 0)
        self.assertEqual(s["tracks_in_cache"], 1)


class TestDeterminism(unittest.TestCase):

    def test_equal_scores_break_ties_by_node_id(self):
        elector = CustodyElector()
        # Two nodes constructed identically → equal scores → tie-break
        # by node_id ascending.
        nodes = [_make_node("zebra", trust=0.7),
                 _make_node("alpha", trust=0.7)]
        d = elector.elect(_make_track(), nodes)
        self.assertEqual(d.primary, "alpha")


if __name__ == "__main__":
    unittest.main()
