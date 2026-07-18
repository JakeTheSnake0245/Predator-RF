"""
TDOA inclusivity tests — ensure ANY GPS-fixed node can participate in
TDOA, but the result confidence is scaled down for nodes with poor
timing hardware (system-clock RTL-SDR vs GPSDO-fed HackRF).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.fusion.tdoa_coordinator import TDOACoordinator


class _Node:
    """Minimal SensorNodeTrust shim for tdoa_coordinator."""
    def __init__(self, node_id, lat, lon,
                 can_tdoa=False, timing_trust=0.4):
        self.node_id = node_id
        self.location_gps = (lat, lon)
        self.can_do_tdoa = can_tdoa
        self.timing_stability_trust = timing_trust


class TDOAInclusiveTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tdoa = TDOACoordinator()
        self.now = time.time_ns()

    async def test_non_tdoa_capable_nodes_still_queued(self):
        """can_do_tdoa=False nodes are accepted at record_measurement —
        but a 2-node all-system-clock solve is now SUPPRESSED (needs
        >=3 distinct hearers). Measurements are re-merged so a third
        hearer can still trigger a solve."""
        n1 = _Node("phone-1", 35.10, -106.50, can_tdoa=False, timing_trust=0.6)
        n2 = _Node("phone-2", 35.15, -106.45, can_tdoa=False, timing_trust=0.6)
        em = "em-cheap"
        self.tdoa.record_measurement(em, n1, self.now)
        self.tdoa.record_measurement(em, n2, self.now + 1_000_000)
        self.assertEqual(self.tdoa.distinct_nodes(em), 2,
            "non-TDOA nodes must now be queued, not silently dropped")
        result = await self.tdoa.solve(em)
        self.assertIsNone(result,
            "2-node all-system-clock hyperbola must be suppressed")
        self.assertEqual(self.tdoa.distinct_nodes(em), 2,
            "suppressed measurements must be re-merged for a later solve")
        # Third cheap hearer arrives → solve now allowed, capped low.
        n3 = _Node("phone-3", 35.12, -106.47, can_tdoa=False, timing_trust=0.6)
        self.tdoa.record_measurement(em, n3, self.now + 2_000_000)
        self.tdoa._triangulate = lambda ms: (35.12, -106.47, 0.85)
        result = await self.tdoa.solve(em)
        self.assertIsNotNone(result)
        self.assertLessEqual(result.location_confidence, 0.35,
            "all-system-clock fix must be confidence-capped")
        self.assertGreater(result.location_confidence, 0.0,
            "but still produce SOME confidence — operator gets a search area")

    async def test_gpsdo_fix_keeps_full_confidence(self):
        """Three TDOA-capable nodes with high timing trust should
        produce essentially the geometric confidence with negligible
        scaling penalty."""
        n1 = _Node("hf-1", 35.10, -106.50, can_tdoa=True, timing_trust=0.95)
        n2 = _Node("hf-2", 35.15, -106.45, can_tdoa=True, timing_trust=0.95)
        n3 = _Node("hf-3", 35.12, -106.47, can_tdoa=True, timing_trust=0.95)
        em = "em-pro"
        self.tdoa.record_measurement(em, n1, self.now)
        self.tdoa.record_measurement(em, n2, self.now + 1_000_000)
        self.tdoa.record_measurement(em, n3, self.now + 2_000_000)
        # Stub the LSQ triangulator (numpy not in this Repl)
        self.tdoa._triangulate = lambda ms: (35.123, -106.456, 0.85)
        result = await self.tdoa.solve(em)
        self.assertIsNotNone(result)
        # 0.85 * 0.95 ≈ 0.808
        self.assertGreater(result.location_confidence, 0.75)

    async def test_mixed_fleet_confidence_in_between(self):
        """One GPSDO node + two cheap phone nodes — confidence sits
        between the all-cheap and all-pro extremes."""
        n1 = _Node("hf", 35.10, -106.50, can_tdoa=True, timing_trust=0.95)
        n2 = _Node("phone-1", 35.15, -106.45, can_tdoa=False, timing_trust=0.6)
        n3 = _Node("phone-2", 35.12, -106.47, can_tdoa=False, timing_trust=0.6)
        em = "em-mix"
        self.tdoa.record_measurement(em, n1, self.now)
        self.tdoa.record_measurement(em, n2, self.now + 1_000_000)
        self.tdoa.record_measurement(em, n3, self.now + 2_000_000)
        self.tdoa._triangulate = lambda ms: (35.12, -106.47, 0.85)
        result = await self.tdoa.solve(em)
        # Mean timing = (0.95 + 0.3 + 0.3) / 3 ≈ 0.517 → conf ≈ 0.439
        self.assertGreater(result.location_confidence, 0.35)
        self.assertLess(result.location_confidence, 0.55)

    async def test_node_without_gps_still_dropped(self):
        """No GPS = can't triangulate at ALL, no matter how good the
        timing is. This rule stays."""
        class _NoGPS(_Node):
            def __init__(self, **kw):
                super().__init__(**kw)
                self.location_gps = None
        n1 = _NoGPS(node_id="n1", lat=0, lon=0, can_tdoa=True)
        em = "em"
        self.tdoa.record_measurement(em, n1, self.now)
        self.assertEqual(self.tdoa.distinct_nodes(em), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
