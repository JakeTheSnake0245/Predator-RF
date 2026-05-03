"""TDOA stale-GPS guard: nodes with location_gps_updated_ns older than
gps_max_age_s are dropped from solves."""
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.fusion.tdoa_coordinator import TDOACoordinator


class _Node:
    def __init__(self, node_id, lat, lon, gps_age_s=0):
        self.node_id = node_id
        self.location_gps = (lat, lon)
        self.can_do_tdoa = True
        self.timing_stability_trust = 1.0
        # 0 means "never set" → bypass freshness gating (test fakes
        # contract with the production code).
        if gps_age_s > 0:
            self.location_gps_updated_ns = time.time_ns() - int(gps_age_s * 1e9)
        else:
            self.location_gps_updated_ns = 0


class TDOAGpsAgeTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        # Mid-test override of the module-level config import. The
        # coordinator does an in-method `from backend.config import config`
        # so we patch the singleton attribute directly.
        from backend.config import config as _cfg
        _cfg.gps_max_age_s = 60.0
        self.tdoa = TDOACoordinator()

    async def test_fresh_gps_is_recorded(self):
        n = _Node("n1", 35.0, -106.0, gps_age_s=10)
        self.tdoa.record_measurement("em", n, time.time_ns())
        self.assertEqual(self.tdoa.distinct_nodes("em"), 1)

    async def test_stale_gps_is_dropped(self):
        n = _Node("n1", 35.0, -106.0, gps_age_s=120)
        self.tdoa.record_measurement("em", n, time.time_ns())
        self.assertEqual(self.tdoa.distinct_nodes("em"), 0)

    async def test_zero_timestamp_bypasses_check(self):
        """Test fakes that don't populate location_gps_updated_ns
        (set to 0) opt out of freshness gating — back-compat with
        the existing 76-test suite."""
        n = _Node("n1", 35.0, -106.0, gps_age_s=0)
        self.tdoa.record_measurement("em", n, time.time_ns())
        self.assertEqual(self.tdoa.distinct_nodes("em"), 1)

    async def test_mixed_fresh_and_stale_filters_correctly(self):
        n1 = _Node("n1", 35.0, -106.0, gps_age_s=5)
        n2 = _Node("n2", 35.1, -106.0, gps_age_s=300)  # stale
        n3 = _Node("n3", 35.0, -106.1, gps_age_s=10)
        ts = time.time_ns()
        self.tdoa.record_measurement("em", n1, ts)
        self.tdoa.record_measurement("em", n2, ts + 1000)
        self.tdoa.record_measurement("em", n3, ts + 2000)
        self.assertEqual(self.tdoa.distinct_nodes("em"), 2)
        self.assertNotIn("n2", {m.node_id
            for m in self.tdoa._pending["em"]})


if __name__ == "__main__":
    unittest.main(verbosity=2)
