"""Tests for node link-health surfacing (Task: link health on dashboard).

Covers:
  - SensorNodeTrust.is_online / to_dict fields (120 s staleness rule)
  - LinkHealthMonitor transition detection (baseline seeding, offline,
    recovery, fleet pruning)
  - FleetStateManager.record_link_event ring serialisation

Run: python -m unittest backend.tests.test_link_health -v
"""
import time
import unittest

from backend.models.sensor_node import SensorNodeTrust, NODE_OFFLINE_AFTER_S
from backend.coordination.link_health import LinkHealthMonitor
from backend.coordination.fleet_state_manager import FleetStateManager


NS = 1_000_000_000


class SensorNodeOnlineTest(unittest.TestCase):
    def test_never_contacted_is_offline(self):
        n = SensorNodeTrust(node_id="alpha")
        self.assertFalse(n.is_online)

    def test_recent_contact_is_online(self):
        n = SensorNodeTrust(node_id="alpha")
        n.last_contact_ns = time.time_ns()
        self.assertTrue(n.is_online)

    def test_stale_contact_is_offline(self):
        n = SensorNodeTrust(node_id="alpha")
        n.last_contact_ns = time.time_ns() - int((NODE_OFFLINE_AFTER_S + 5) * NS)
        self.assertFalse(n.is_online)

    def test_to_dict_carries_link_fields(self):
        n = SensorNodeTrust(node_id="alpha")
        d = n.to_dict()
        self.assertIn("is_online", d)
        self.assertIs(d["is_online"], False)
        self.assertEqual(d["offline_after_s"], NODE_OFFLINE_AFTER_S)
        n.last_contact_ns = time.time_ns()
        self.assertIs(n.to_dict()["is_online"], True)


class LinkHealthMonitorTest(unittest.TestCase):
    def setUp(self):
        self.mon = LinkHealthMonitor(offline_after_s=120.0)
        self.now = 2_000_000_000 * NS  # far-future fixed clock

    def test_first_observation_seeds_without_event(self):
        ev = self.mon.observe("alpha", self.now - 1 * NS, now_ns=self.now)
        self.assertIsNone(ev)

    def test_offline_transition_fires_once(self):
        self.mon.observe("alpha", self.now - 1 * NS, now_ns=self.now)
        later = self.now + 200 * NS
        ev = self.mon.observe("alpha", self.now - 1 * NS, now_ns=later)
        self.assertIsNotNone(ev)
        self.assertEqual(ev["type"], "node_offline")
        self.assertEqual(ev["node_id"], "alpha")
        self.assertEqual(ev["offline_after_s"], 120.0)
        # No repeat while still offline
        self.assertIsNone(
            self.mon.observe("alpha", self.now - 1 * NS, now_ns=later + 10 * NS))

    def test_recovery_fires_node_online(self):
        self.mon.observe("alpha", None, now_ns=self.now)  # baseline offline
        ev = self.mon.observe("alpha", self.now + 5 * NS, now_ns=self.now + 6 * NS)
        self.assertIsNotNone(ev)
        self.assertEqual(ev["type"], "node_online")

    def test_never_contacted_is_offline(self):
        self.assertFalse(self.mon.is_online(None, now_ns=self.now))
        self.assertFalse(self.mon.is_online(0, now_ns=self.now))

    def test_boundary_exactly_at_threshold_is_offline(self):
        last = self.now - int(120.0 * NS)
        self.assertFalse(self.mon.is_online(last, now_ns=self.now))
        self.assertTrue(self.mon.is_online(last + 1 * NS, now_ns=self.now))

    def test_observe_fleet_prunes_removed_nodes(self):
        self.mon.observe("alpha", self.now - 1 * NS, now_ns=self.now)
        self.mon.observe("bravo", self.now - 1 * NS, now_ns=self.now)
        evs = self.mon.observe_fleet({"alpha": self.now - 1 * NS},
                                     now_ns=self.now + 1 * NS)
        self.assertEqual(evs, [])
        self.assertNotIn("bravo", self.mon._prior)
        # bravo re-added later seeds fresh (no event)
        evs = self.mon.observe_fleet(
            {"alpha": self.now - 1 * NS, "bravo": None},
            now_ns=self.now + 2 * NS)
        self.assertEqual(evs, [])

    def test_observe_fleet_returns_transitions(self):
        base = {"alpha": self.now - 1 * NS, "bravo": self.now - 1 * NS}
        self.mon.observe_fleet(base, now_ns=self.now)
        later = self.now + 500 * NS
        evs = self.mon.observe_fleet(base, now_ns=later)
        self.assertEqual(len(evs), 2)
        self.assertTrue(all(e["type"] == "node_offline" for e in evs))


class FleetStateLinkEventTest(unittest.TestCase):
    def test_record_link_event_serialises_into_ring(self):
        mgr = FleetStateManager()
        mgr.record_link_event({"type": "node_offline", "node_id": "alpha",
                               "ts_ns": 123, "last_contact_ns": None,
                               "offline_after_s": 120.0})
        mgr.record_link_event({"type": "node_online", "node_id": "alpha",
                               "ts_ns": 456, "last_contact_ns": 400,
                               "offline_after_s": 120.0})
        evs = mgr.get_events_since(0)
        kinds = [e.get("type") for e in evs]
        self.assertIn("node_offline", kinds)
        self.assertIn("node_online", kinds)
        serials = [e["_fleet_serial"] for e in evs]
        self.assertEqual(serials, sorted(serials))


if __name__ == "__main__":
    unittest.main()
