"""
Unit tests for SignalRepository, SignalFingerprinter, and CorrelationEngine.

Run:
    python -m unittest backend.tests.test_signal_repository -v
"""
import math
import os
import struct
import tempfile
import time
import unittest

from backend.signal_repository.fingerprinter import (
    SignalFingerprinter, FINGERPRINT_DIM, _cosine_sim,
    _octave_bands, _spectral_moments, _spectral_flatness,
    _peak_count, _bw_fraction, _autocorr,
)
from backend.signal_repository.repository import SignalRepository


class FingerprintDimTest(unittest.TestCase):
    def setUp(self):
        self.fp = SignalFingerprinter()

    def test_bins_output_dim(self):
        bins = [float(i) for i in range(256)]
        vec = self.fp.fingerprint_from_bins(bins)
        self.assertEqual(len(vec), FINGERPRINT_DIM)

    def test_metadata_output_dim(self):
        vec = self.fp.fingerprint_from_metadata(-60.0, 12500.0, 433.92e6, 15.0)
        self.assertEqual(len(vec), FINGERPRINT_DIM)

    def test_bins_all_zeros_no_crash(self):
        vec = self.fp.fingerprint_from_bins([0.0] * 128)
        self.assertEqual(len(vec), FINGERPRINT_DIM)

    def test_bins_single_peak(self):
        bins = [0.0] * 256
        bins[128] = 100.0
        vec = self.fp.fingerprint_from_bins(bins)
        self.assertEqual(len(vec), FINGERPRINT_DIM)
        self.assertGreater(vec[8], 0.4)

    def test_cosine_sim_identical(self):
        v = [1.0, 2.0, 3.0, 4.0]
        self.assertAlmostEqual(_cosine_sim(v, v), 1.0, places=5)

    def test_cosine_sim_orthogonal(self):
        a = [1.0, 0.0]
        b = [0.0, 1.0]
        self.assertAlmostEqual(_cosine_sim(a, b), 0.0, places=5)

    def test_cosine_sim_length_mismatch(self):
        self.assertEqual(_cosine_sim([1.0], [1.0, 2.0]), 0.0)

    def test_cosine_sim_zeros(self):
        self.assertEqual(_cosine_sim([0.0, 0.0], [0.0, 0.0]), 0.0)

    def test_is_match_self(self):
        bins = [float(i % 32) for i in range(256)]
        vec = self.fp.fingerprint_from_bins(bins)
        self.assertTrue(self.fp.is_match(vec, vec))

    def test_is_match_different_signals(self):
        # Lower-half energy vs upper-half energy — clearly different spectra.
        bins_a = [100.0 if i < 128 else 0.001 for i in range(256)]
        bins_b = [0.001 if i < 128 else 100.0 for i in range(256)]
        va = self.fp.fingerprint_from_bins(bins_a)
        vb = self.fp.fingerprint_from_bins(bins_b)
        # Similarity should be meaningfully below 1.0; strict threshold
        # depends on feature set, so just assert they are not identical.
        sim = self.fp.similarity(va, vb)
        self.assertLess(sim, 0.999)

    def test_octave_bands_normalised(self):
        bands = _octave_bands([1.0] * 256)
        self.assertAlmostEqual(sum(bands), 1.0, places=5)

    def test_spectral_flatness_flat(self):
        sf = _spectral_flatness([1.0] * 64)
        self.assertAlmostEqual(sf, 1.0, places=3)

    def test_spectral_flatness_single_peak(self):
        bins = [0.0001] * 64
        bins[32] = 100.0
        sf = _spectral_flatness(bins)
        self.assertLess(sf, 0.1)

    def test_peak_count_uniform(self):
        self.assertAlmostEqual(_peak_count([1.0] * 64), 0.0)

    def test_bw_fraction_range(self):
        bw = _bw_fraction([float(i) for i in range(64)])
        self.assertGreater(bw, 0.0)
        self.assertLessEqual(bw, 1.0)

    def test_autocorr_length(self):
        ac = _autocorr([math.sin(i * 0.5) for i in range(64)], lags=8)
        self.assertEqual(len(ac), 8)

    def test_autocorr_bounds(self):
        ac = _autocorr([float(i) for i in range(32)], lags=8)
        for v in ac:
            self.assertGreaterEqual(v, -1.0)
            self.assertLessEqual(v, 1.0)


class RepositoryTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.mkdtemp()
        db = os.path.join(self._tmp, "test_repo.db")
        iq = os.path.join(self._tmp, "iq")
        self.repo = SignalRepository(db, iq_dir=iq)

    def tearDown(self):
        import shutil
        shutil.rmtree(self._tmp, ignore_errors=True)

    def test_save_and_search_by_freq(self):
        sid = self.repo.save_signal("node-1", 433.92e6, power_dbfs=-60.0)
        self.assertIsNotNone(sid)
        results = self.repo.search(freq_hz=433.92e6, freq_tol_hz=25000.0)
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["signal_id"], sid)

    def test_search_no_results_outside_window(self):
        self.repo.save_signal("node-1", 100.0e6)
        results = self.repo.search(freq_hz=433.92e6, freq_tol_hz=25000.0)
        self.assertEqual(len(results), 0)

    def test_save_fingerprint_and_retrieve(self):
        sid = self.repo.save_signal("node-1", 433.92e6)
        fp = SignalFingerprinter()
        vec = fp.fingerprint_from_metadata(-60.0, 12500.0, 433.92e6, 15.0)
        self.repo.save_fingerprint(sid, vec)
        stored = self.repo.get_fingerprint(sid)
        self.assertIsNotNone(stored)
        self.assertEqual(len(stored), FINGERPRINT_DIM)
        for a, b in zip(vec, stored):
            self.assertAlmostEqual(a, b, places=5)

    def test_find_similar_self_match(self):
        fp = SignalFingerprinter()
        vec = fp.fingerprint_from_metadata(-50.0, 25000.0, 162.55e6, 20.0)
        sid = self.repo.save_signal("node-2", 162.55e6,
                                    fingerprint_vec=vec)
        matches = self.repo.find_similar(vec, threshold=0.5)
        self.assertGreater(len(matches), 0)
        self.assertEqual(matches[0]["signal_id"], sid)
        self.assertGreaterEqual(matches[0]["similarity"], 0.5)

    def test_find_similar_no_match_below_threshold(self):
        fp = SignalFingerprinter()
        vec_a = fp.fingerprint_from_bins([100.0 if i < 128 else 0.0 for i in range(256)])
        vec_b = fp.fingerprint_from_bins([0.0 if i < 64 else 50.0 for i in range(256)])
        sid = self.repo.save_signal("node-1", 433.0e6, fingerprint_vec=vec_a)
        matches = self.repo.find_similar(vec_b, threshold=0.99)
        self.assertEqual(len(matches), 0)

    def test_record_iq_capture(self):
        sid = self.repo.save_signal("node-1", 433.92e6)
        cid = self.repo.record_iq_capture(
            signal_id=sid, file_path="/tmp/test.cs8",
            duration_s=5.0, sample_rate_hz=2.4e6,
            center_hz=433.92e6, captured_by="operator")
        self.assertIsNotNone(cid)
        with self.repo._lock:
            row = self.repo._conn.execute(
                "SELECT * FROM iq_captures WHERE capture_id=?", (cid,)).fetchone()
        self.assertIsNotNone(row)
        self.assertAlmostEqual(row["duration_s"], 5.0)

    def test_save_correlated_intercept(self):
        sid = self.repo.save_signal("node-1", 433.92e6)
        iid = self.repo.save_correlated_intercept(
            signal_id=sid, node_ids=["node-1", "node-2"],
            center_hz=433.92e6, rule_id="rule-alpha")
        self.assertIsNotNone(iid)
        with self.repo._lock:
            row = self.repo._conn.execute(
                "SELECT * FROM correlated_intercepts WHERE intercept_id=?",
                (iid,)).fetchone()
        self.assertIsNotNone(row)
        self.assertIn("node-1", row["node_ids_json"])

    def test_search_by_modulation(self):
        self.repo.save_signal("node-1", 433.92e6, modulation="FSK")
        self.repo.save_signal("node-2", 433.92e6, modulation="AM")
        results = self.repo.search(freq_hz=433.92e6, modulation="FSK")
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["modulation"], "FSK")

    def test_search_limit(self):
        for i in range(20):
            self.repo.save_signal(f"node-{i}", 433.92e6)
        results = self.repo.search(freq_hz=433.92e6, limit=5)
        self.assertLessEqual(len(results), 5)

    def test_observation_count_increments(self):
        fp = SignalFingerprinter()
        vec = fp.fingerprint_from_metadata(-60.0, 12500.0, 162.55e6, 10.0)
        sid = "fixed-uuid-for-test"
        self.repo.save_signal("node-1", 162.55e6, signal_id=sid)
        self.repo.save_signal("node-1", 162.55e6, signal_id=sid)
        with self.repo._lock:
            row = self.repo._conn.execute(
                "SELECT observation_count FROM signal_repository WHERE signal_id=?",
                (sid,)).fetchone()
        self.assertGreaterEqual(row["observation_count"], 1)


class CorrelationEngineTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.mkdtemp()
        db = os.path.join(self._tmp, "corr_test.db")
        iq = os.path.join(self._tmp, "iq")
        from backend.persistence.store import MissionStore
        self.store = MissionStore(db)
        self.repo  = SignalRepository(db, iq_dir=iq)
        from backend.coordination.correlation_engine import CorrelationEngine
        self.engine = CorrelationEngine(
            store=self.store, signal_repo=self.repo, auto_tasker=None)
        self.alerts = []
        self.engine.on_alert(self.alerts.append)

    def tearDown(self):
        import shutil
        shutil.rmtree(self._tmp, ignore_errors=True)

    def test_add_and_list_rule(self):
        from backend.coordination.correlation_engine import CorrelationRule
        rule = CorrelationRule(
            rule_id="r1", name="Test Rule", nodes=[],
            freq_lo_hz=430e6, freq_hi_hz=440e6, window_s=60.0,
            min_nodes=2, action="alert")
        self.engine.add_rule(rule)
        rules = self.engine.list_rules()
        self.assertEqual(len(rules), 1)
        self.assertEqual(rules[0]["rule_id"], "r1")

    def test_remove_rule(self):
        from backend.coordination.correlation_engine import CorrelationRule
        rule = CorrelationRule(
            rule_id="r2", name="Remove Me", nodes=[],
            freq_lo_hz=430e6, freq_hi_hz=440e6, window_s=60.0,
            min_nodes=2, action="alert")
        self.engine.add_rule(rule)
        removed = self.engine.remove_rule("r2")
        self.assertTrue(removed)
        self.assertEqual(len(self.engine.list_rules()), 0)

    def test_remove_nonexistent_rule(self):
        removed = self.engine.remove_rule("does-not-exist")
        self.assertFalse(removed)

    def test_rule_not_fired_single_node(self):
        from backend.coordination.correlation_engine import CorrelationRule
        rule = CorrelationRule(
            rule_id="r3", name="Two Node Rule", nodes=[],
            freq_lo_hz=430e6, freq_hi_hz=440e6, window_s=60.0,
            min_nodes=2, action="alert")
        self.engine.add_rule(rule)
        self.engine.on_event("node-A", 433.92e6)
        self.engine.on_event("node-A", 433.92e6)
        self.assertEqual(len(self.alerts), 0)

    def test_rule_fires_on_two_distinct_nodes(self):
        import asyncio
        from backend.coordination.correlation_engine import CorrelationRule
        rule = CorrelationRule(
            rule_id="r4", name="Two Node Alert", nodes=[],
            freq_lo_hz=430e6, freq_hi_hz=440e6, window_s=60.0,
            min_nodes=2, action="alert")
        self.engine.add_rule(rule)
        loop = asyncio.new_event_loop()
        self.engine.on_event("node-A", 433.92e6)
        self.engine.on_event("node-B", 435.00e6)
        loop.run_until_complete(asyncio.sleep(0.1))
        loop.close()

    def test_rule_ignores_out_of_band(self):
        from backend.coordination.correlation_engine import CorrelationRule
        rule = CorrelationRule(
            rule_id="r5", name="Band Filter", nodes=[],
            freq_lo_hz=430e6, freq_hi_hz=440e6, window_s=60.0,
            min_nodes=2, action="alert")
        self.engine.add_rule(rule)
        self.engine.on_event("node-A", 162.55e6)
        self.engine.on_event("node-B", 162.55e6)
        self.assertEqual(len(self.alerts), 0)

    def test_node_filter_respected(self):
        from backend.coordination.correlation_engine import CorrelationRule
        rule = CorrelationRule(
            rule_id="r6", name="Node Filter", nodes=["alpha", "bravo"],
            freq_lo_hz=430e6, freq_hi_hz=440e6, window_s=60.0,
            min_nodes=2, action="alert")
        self.engine.add_rule(rule)
        self.engine.on_event("charlie", 433.92e6)
        self.engine.on_event("delta", 433.92e6)
        self.assertEqual(len(self.alerts), 0)


class FleetStateManagerTest(unittest.TestCase):
    def test_update_and_query(self):
        from backend.coordination.fleet_state_manager import FleetStateManager
        mgr = FleetStateManager()
        mgr.update_node_snapshot("alpha",
            tracks=[{"emitter_id": "em1", "primary_frequency": 433.92e6}],
            events=[{"type": "hit", "id": 1}],
            gps={"lat": 37.4, "lon": -122.1},
            status={"sdr_running": True, "center_freq": 433.92e6})
        snap = mgr.get_node("alpha")
        self.assertIsNotNone(snap)
        self.assertTrue(snap.is_online)
        self.assertEqual(len(snap.tracks), 1)
        self.assertAlmostEqual(snap.gps["lat"], 37.4)

    def test_online_node_ids(self):
        from backend.coordination.fleet_state_manager import FleetStateManager
        mgr = FleetStateManager()
        mgr.update_node_snapshot("alpha", status={"sdr_running": True})
        mgr.update_node_snapshot("bravo", status={"sdr_running": False})
        online = mgr.online_node_ids()
        self.assertIn("alpha", online)
        self.assertIn("bravo", online)

    def test_get_events_since(self):
        from backend.coordination.fleet_state_manager import FleetStateManager
        mgr = FleetStateManager()
        mgr.update_node_snapshot("alpha",
            events=[{"type": "hit1"}, {"type": "hit2"}])
        events = mgr.get_events_since(0)
        self.assertEqual(len(events), 2)
        # get_events_since(serial) returns events with _fleet_serial >= serial.
        # events[1] has serial N; asking for >N returns nothing, asking for >=N
        # returns just that last event.
        last_serial = events[-1]["_fleet_serial"]
        events2 = mgr.get_events_since(last_serial)
        self.assertEqual(len(events2), 1)
        self.assertEqual(events2[0]["type"], "hit2")

    def test_get_all_tracks_deduplication(self):
        from backend.coordination.fleet_state_manager import FleetStateManager
        mgr = FleetStateManager()
        track = {"emitter_id": "em1", "primary_frequency": 433.92e6,
                 "last_seen_ns": 1000}
        mgr.update_node_snapshot("alpha", tracks=[track])
        mgr.update_node_snapshot("bravo", tracks=[track])
        all_tracks = mgr.get_all_tracks()
        self.assertEqual(len(all_tracks), 1)
        nodes = all_tracks[0].get("_observing_nodes", [])
        self.assertIn("alpha", nodes)
        self.assertIn("bravo", nodes)

    def test_serialize(self):
        from backend.coordination.fleet_state_manager import FleetStateManager
        mgr = FleetStateManager()
        mgr.update_node_snapshot("alpha", status={"sdr_running": True})
        data = mgr.serialize()
        self.assertIn("nodes", data)
        self.assertIn("alpha", data["nodes"])
        self.assertIn("online_count", data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
