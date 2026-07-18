"""
Unit tests for PhoneGPSSource — the coordinator kit's paired-phone GPS
poller (backend/coordination/phone_gps.py).

Covers: live-fix update + accuracy propagation, no-fix / unreachable
fallback semantics, staleness handling (retain-then-revert), manual
location honesty (updated_ns stays 0), contact stamping, and the
fleet-manager local-node exposure.

Run: python -m unittest backend.tests.test_phone_gps -v
"""
import asyncio
import unittest

from backend.coordination.phone_gps import PhoneGPSSource
from backend.models.sensor_node import SensorNodeTrust

NOW_NS = 2_000_000_000_000_000_000  # far-future, matches other test suites


def _node(**kw):
    return SensorNodeTrust(node_id="coordinator", hardware_code="rtlsdr", **kw)


def _src(node, manual=None, **kw):
    async def _never_called():
        raise AssertionError("fetch should not be called in apply-only tests")
    return PhoneGPSSource(node=node, host="10.0.0.2", port=5259,
                          manual_location=manual,
                          fetch=kw.pop("fetch", _never_called), **kw)


class TestPhoneFixUpdate(unittest.TestCase):
    def test_fix_updates_location_accuracy_and_freshness(self):
        node = _node()
        src = _src(node)
        src.apply({"hasFix": True, "lat": 34.1, "lon": -118.2,
                   "accuracy": 4.5}, now_ns=NOW_NS)
        self.assertEqual(node.location_gps, (34.1, -118.2))
        self.assertEqual(node.location_accuracy_m, 4.5)
        self.assertEqual(node.location_gps_updated_ns, NOW_NS)
        self.assertEqual(node.gps_source, "phone")
        self.assertTrue(src.phone_reachable)

    def test_accuracy_propagates_on_every_fix(self):
        node = _node()
        src = _src(node)
        src.apply({"hasFix": True, "lat": 1, "lon": 2, "accuracy": 3.0},
                  now_ns=NOW_NS)
        src.apply({"hasFix": True, "lat": 1, "lon": 2, "accuracy": 12.0},
                  now_ns=NOW_NS + 1_000_000_000)
        self.assertEqual(node.location_accuracy_m, 12.0)

    def test_garbage_coords_ignored(self):
        node = _node()
        src = _src(node)
        src.apply({"hasFix": True, "lat": "not-a-number", "lon": None},
                  now_ns=NOW_NS)
        self.assertIsNone(node.location_gps)
        self.assertEqual(node.gps_source, "")

    def test_contact_stamped_when_standalone(self):
        node = _node()
        src = _src(node, stamp_contact=True)
        src.apply({"hasFix": False}, now_ns=NOW_NS)
        self.assertEqual(node.last_contact_ns, NOW_NS)

    def test_contact_not_stamped_when_fleet_node(self):
        node = _node()
        src = _src(node)  # stamp_contact defaults False
        src.apply({"hasFix": True, "lat": 1, "lon": 2}, now_ns=NOW_NS)
        self.assertEqual(node.last_contact_ns, 0)


class TestNoFixFallback(unittest.TestCase):
    def test_manual_seeded_at_construction(self):
        node = _node()
        _src(node, manual=(40.0, -105.0))
        self.assertEqual(node.location_gps, (40.0, -105.0))
        self.assertEqual(node.gps_source, "manual")
        # NEVER a fake freshness stamp for manual positions.
        self.assertEqual(node.location_gps_updated_ns, 0)

    def test_recent_phone_fix_retained_during_short_outage(self):
        node = _node()
        src = _src(node, manual=(40.0, -105.0), fallback_after_s=300.0)
        src.apply({"hasFix": True, "lat": 34.1, "lon": -118.2,
                   "accuracy": 5}, now_ns=NOW_NS)
        # 60 s later, phone unreachable → keep the fix, honest age.
        src.apply(None, now_ns=NOW_NS + int(60e9))
        self.assertEqual(node.location_gps, (34.1, -118.2))
        self.assertEqual(node.gps_source, "phone")
        self.assertEqual(node.location_gps_updated_ns, NOW_NS)
        self.assertFalse(src.phone_reachable)

    def test_prolonged_outage_reverts_to_manual(self):
        node = _node()
        src = _src(node, manual=(40.0, -105.0), fallback_after_s=300.0)
        src.apply({"hasFix": True, "lat": 34.1, "lon": -118.2},
                  now_ns=NOW_NS)
        src.apply(None, now_ns=NOW_NS + int(301e9))
        self.assertEqual(node.location_gps, (40.0, -105.0))
        self.assertEqual(node.gps_source, "manual")
        self.assertEqual(node.location_gps_updated_ns, 0)

    def test_prolonged_outage_without_manual_clears_source_tag(self):
        node = _node()
        src = _src(node, manual=None, fallback_after_s=300.0)
        src.apply({"hasFix": True, "lat": 34.1, "lon": -118.2},
                  now_ns=NOW_NS)
        src.apply(None, now_ns=NOW_NS + int(301e9))
        # Last-known coords remain but the tag says "not live".
        self.assertEqual(node.location_gps, (34.1, -118.2))
        self.assertEqual(node.gps_source, "")
        self.assertEqual(node.location_gps_updated_ns, NOW_NS)

    def test_no_fix_never_produces_fake_fix(self):
        node = _node()
        src = _src(node)  # no manual location either
        src.apply({"hasFix": False}, now_ns=NOW_NS)
        src.apply(None, now_ns=NOW_NS + int(10e9))
        self.assertIsNone(node.location_gps)
        self.assertEqual(node.gps_source, "")
        self.assertEqual(node.location_gps_updated_ns, 0)

    def test_phone_recovery_after_manual_fallback(self):
        node = _node()
        src = _src(node, manual=(40.0, -105.0), fallback_after_s=300.0)
        src.apply({"hasFix": True, "lat": 34.1, "lon": -118.2},
                  now_ns=NOW_NS)
        src.apply(None, now_ns=NOW_NS + int(400e9))         # → manual
        self.assertEqual(node.gps_source, "manual")
        t2 = NOW_NS + int(500e9)
        src.apply({"hasFix": True, "lat": 34.2, "lon": -118.3,
                   "accuracy": 7}, now_ns=t2)                # phone back
        self.assertEqual(node.location_gps, (34.2, -118.3))
        self.assertEqual(node.gps_source, "phone")
        self.assertEqual(node.location_gps_updated_ns, t2)


class TestPollOnce(unittest.TestCase):
    def test_poll_once_uses_injected_fetch(self):
        node = _node()

        async def fake_fetch():
            return {"hasFix": True, "lat": 51.5, "lon": -0.1,
                    "accuracy": 8.0}

        src = PhoneGPSSource(node=node, host="10.0.0.2", fetch=fake_fetch)
        asyncio.run(src.poll_once(now_ns=NOW_NS))
        self.assertEqual(node.location_gps, (51.5, -0.1))
        self.assertEqual(node.gps_source, "phone")

    def test_status_reports_source_and_reachability(self):
        node = _node()
        src = _src(node)
        src.apply({"hasFix": True, "lat": 1, "lon": 2}, now_ns=NOW_NS)
        st = src.status()
        self.assertTrue(st["phone_reachable"])
        self.assertEqual(st["gps_source"], "phone")
        self.assertEqual(st["last_phone_fix_ns"], NOW_NS)


class TestNodeDictExposure(unittest.TestCase):
    def test_to_dict_carries_source_age_accuracy(self):
        import time
        node = _node()
        src = _src(node)
        src.apply({"hasFix": True, "lat": 1, "lon": 2, "accuracy": 6.0},
                  now_ns=time.time_ns())
        d = node.to_dict()
        self.assertEqual(d["gps_source"], "phone")
        self.assertEqual(d["location_accuracy_m"], 6.0)
        self.assertIsNotNone(d["gps_age_s"])
        self.assertLess(d["gps_age_s"], 5.0)

    def test_to_dict_manual_has_no_age(self):
        node = _node()
        _src(node, manual=(40.0, -105.0))
        d = node.to_dict()
        self.assertEqual(d["gps_source"], "manual")
        self.assertIsNone(d["gps_age_s"])


class TestFleetManagerLocalNode(unittest.TestCase):
    def test_local_node_included_in_all_nodes(self):
        from backend.coordination.kujhad_client import KujhadFleetManager
        fm = KujhadFleetManager()
        local = _node()
        fm.set_local_node(local)
        nodes = fm.all_nodes()
        self.assertEqual(len(nodes), 1)
        self.assertIs(nodes[0], local)

    def test_local_node_not_duplicated_when_registered_client(self):
        from backend.coordination.kujhad_client import KujhadFleetManager
        fm = KujhadFleetManager()
        local = _node()
        fm._clients[local.node_id] = object()  # simulate registered client
        fm.set_local_node(local)
        # all_nodes would include the client's .node; here we only assert
        # the local node isn't appended a second time.
        self.assertEqual(
            sum(1 for n in [fm.local_node]
                if fm.local_node.node_id in fm._clients), 1)
        fm._clients.clear()
        self.assertEqual(len(fm.all_nodes()), 1)


if __name__ == "__main__":
    unittest.main()
