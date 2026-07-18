"""Unit tests for operator target nomination (NominationManager).

Run: python -m unittest backend.tests.test_target_nomination -v
"""
import os
import tempfile
import unittest

from backend.persistence.store import MissionStore
from backend.coordination.correlation_engine import CorrelationEngine
from backend.coordination.target_nomination import (
    NominationManager, NOMINATION_RULE_ID, NOMINATION_FREQ_TOL_HZ)
from backend.signal_repository.repository import SignalRepository


class FakeTrack:
    def __init__(self, emitter_id, freq):
        self.emitter_id = emitter_id
        self.primary_frequency = freq
        self.modulation = "nfm"
        self.protocol = None
        self.threat_level = "medium"
        self.estimated_lat = 34.1
        self.estimated_lon = -117.2
        self.location_method = "tdoa"


class FakeTrackManager:
    def __init__(self):
        self.tracks = {}


class NominationTestBase(unittest.TestCase):
    def setUp(self):
        fd, self.db_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        fd, self.repo_path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = MissionStore(self.db_path)
        self.repo = SignalRepository(self.repo_path,
                                     iq_dir=tempfile.mkdtemp())
        self.engine = CorrelationEngine(store=self.store)
        self.tm = FakeTrackManager()
        self.tm.tracks["em-1"] = FakeTrack("em-1", 462_562_500.0)
        self.mgr = NominationManager(
            store=self.store, correlation_engine=self.engine,
            signal_repo=self.repo, track_manager=self.tm)

    def tearDown(self):
        for p in (self.db_path, self.repo_path):
            try:
                os.unlink(p)
            except OSError:
                pass


class TestNominate(NominationTestBase):
    def test_nominate_by_emitter_id(self):
        nom = self.mgr.nominate(emitter_id="em-1", operator="op1")
        self.assertEqual(nom["emitter_id"], "em-1")
        self.assertAlmostEqual(nom["frequency_hz"], 462_562_500.0)
        self.assertEqual(nom["nominated_by"], "op1")
        self.assertIsNotNone(self.mgr.current())

    def test_nominate_by_frequency(self):
        nom = self.mgr.nominate(frequency_hz=155_000_000.0, label="repeater")
        self.assertIsNone(nom["emitter_id"])
        self.assertEqual(nom["label"], "repeater")

    def test_nominate_unknown_track_raises(self):
        with self.assertRaises(ValueError):
            self.mgr.nominate(emitter_id="nope")

    def test_nominate_no_args_raises(self):
        with self.assertRaises(ValueError):
            self.mgr.nominate()
        with self.assertRaises(ValueError):
            self.mgr.nominate(frequency_hz=-5.0)

    def test_single_active_target(self):
        self.mgr.nominate(frequency_hz=100e6)
        self.mgr.nominate(frequency_hz=200e6)
        cur = self.mgr.current()
        self.assertAlmostEqual(cur["frequency_hz"], 200e6)
        # Only one nomination rule exists (fixed rule id upsert).
        rules = [r for r in self.engine.list_rules()
                 if r["rule_id"] == NOMINATION_RULE_ID]
        self.assertEqual(len(rules), 1)
        self.assertAlmostEqual(rules[0]["freq_lo_hz"],
                               200e6 - NOMINATION_FREQ_TOL_HZ)

    def test_correlation_rule_created(self):
        self.mgr.nominate(emitter_id="em-1")
        rules = [r for r in self.engine.list_rules()
                 if r["rule_id"] == NOMINATION_RULE_ID]
        self.assertEqual(len(rules), 1)
        self.assertEqual(rules[0]["min_nodes"], 2)
        self.assertEqual(rules[0]["action"], "alert")

    def test_known_target_registered(self):
        nom = self.mgr.nominate(emitter_id="em-1")
        self.assertIsNotNone(nom["signal_id"])
        hits = self.repo.search(freq_hz=462_562_500.0)
        self.assertTrue(any(h["signal_id"] == nom["signal_id"] and
                            h["node_id"] == "operator_nomination"
                            for h in hits))


class TestClear(NominationTestBase):
    def test_clear(self):
        self.mgr.nominate(frequency_hz=100e6)
        self.assertTrue(self.mgr.clear())
        self.assertIsNone(self.mgr.current())
        rules = [r for r in self.engine.list_rules()
                 if r["rule_id"] == NOMINATION_RULE_ID]
        self.assertEqual(rules, [])

    def test_clear_when_none(self):
        self.assertFalse(self.mgr.clear())

    def test_repo_entry_survives_clear(self):
        nom = self.mgr.nominate(emitter_id="em-1")
        self.mgr.clear()
        hits = self.repo.search(freq_hz=462_562_500.0)
        self.assertTrue(any(h["signal_id"] == nom["signal_id"] for h in hits))


class TestIsNominated(NominationTestBase):
    def test_match_by_emitter(self):
        self.mgr.nominate(emitter_id="em-1")
        self.assertTrue(self.mgr.is_nominated("em-1", None))
        self.assertFalse(self.mgr.is_nominated("em-2", 999e6))

    def test_match_by_frequency_tolerance(self):
        self.mgr.nominate(frequency_hz=100e6)
        self.assertTrue(self.mgr.is_nominated(None, 100e6 + 20_000))
        self.assertFalse(self.mgr.is_nominated(None, 100e6 + 30_000))

    def test_no_nomination(self):
        self.assertFalse(self.mgr.is_nominated("em-1", 462_562_500.0))


class TestPersistence(NominationTestBase):
    def test_survives_restart(self):
        self.mgr.nominate(emitter_id="em-1", label="hot", operator="op9")
        # Simulate coordinator restart: fresh manager on the same store.
        mgr2 = NominationManager(
            store=self.store, correlation_engine=self.engine,
            signal_repo=self.repo, track_manager=self.tm)
        cur = mgr2.current()
        self.assertIsNotNone(cur)
        self.assertEqual(cur["emitter_id"], "em-1")
        self.assertEqual(cur["label"], "hot")
        self.assertEqual(cur["nominated_by"], "op9")

    def test_restart_reasserts_rule(self):
        self.mgr.nominate(frequency_hz=100e6)
        self.engine.remove_rule(NOMINATION_RULE_ID)
        mgr2 = NominationManager(
            store=self.store, correlation_engine=self.engine,
            signal_repo=self.repo, track_manager=self.tm)
        rules = [r for r in self.engine.list_rules()
                 if r["rule_id"] == NOMINATION_RULE_ID]
        self.assertEqual(len(rules), 1)

    def test_clear_persists(self):
        self.mgr.nominate(frequency_hz=100e6)
        self.mgr.clear()
        mgr2 = NominationManager(store=self.store)
        self.assertIsNone(mgr2.current())

    def test_no_store_still_works(self):
        mgr = NominationManager(track_manager=self.tm)
        mgr.nominate(emitter_id="em-1")
        self.assertIsNotNone(mgr.current())
        self.assertTrue(mgr.clear())


if __name__ == "__main__":
    unittest.main()
