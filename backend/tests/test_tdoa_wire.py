"""
TDOA wire-up tests — verify the orchestrator path that takes RFEvents from
two GPS-synced nodes hearing the same emitter inside a tight time window
and produces a location estimate on the EmitterTrack.

These exercise the 2-node midpoint path which is pure stdlib (no numpy
required). The 3-node least-squares triangulator path is exercised with
the heavy numpy math monkey-patched out, just to lock down that solve()
correctly threads results out of the `if/else` regardless of branch.
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.fusion.tdoa_coordinator import TDOACoordinator


class _FakeNode:
    """Minimal SensorNodeTrust shim — TDOACoordinator reads
    location_gps, can_do_tdoa, node_id, and timing_stability_trust.
    Default timing_stability_trust=1.0 so a can_tdoa=True fake yields
    timing_factor=1.0 (i.e. tests written before the inclusive-policy
    change keep their numeric assertions on raw geometric confidence)."""
    def __init__(self, node_id: str, lat: float, lon: float,
                 can_tdoa: bool = True,
                 timing_stability_trust: float = 1.0):
        self.node_id = node_id
        self.location_gps = (lat, lon)
        self.can_do_tdoa = can_tdoa
        self.timing_stability_trust = timing_stability_trust


class TDOACoordinatorWireTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tdoa = TDOACoordinator()
        self.now = time.time_ns()
        self.n1 = _FakeNode("n1", 35.100, -106.500)
        self.n2 = _FakeNode("n2", 35.150, -106.450)
        self.n3 = _FakeNode("n3", 35.120, -106.470)

    async def test_three_node_solve_invokes_triangulate_and_returns_result(self):
        """Regression: an earlier indent slip dropped the result-construction
        out of the 3-node code path. With _triangulate stubbed (so we don't
        need numpy on the Repl host), solve() must still return a TDOAResult
        whose lat/lon match the stubbed triangulator's output."""
        em = "emitter-tri"
        self.tdoa.record_measurement(em, self.n1, self.now)
        self.tdoa.record_measurement(em, self.n2, self.now + 1_000_000)
        self.tdoa.record_measurement(em, self.n3, self.now + 2_000_000)
        self.assertEqual(self.tdoa.distinct_nodes(em), 3)

        # Stub the CPU-heavy triangulator
        self.tdoa._triangulate = lambda ms: (35.123, -106.456, 0.85)

        result = await self.tdoa.solve(em)
        self.assertIsNotNone(result)
        self.assertAlmostEqual(result.estimated_lat, 35.123, places=4)
        self.assertAlmostEqual(result.estimated_lon, -106.456, places=4)
        self.assertAlmostEqual(result.location_confidence, 0.85, places=4)
        self.assertEqual(set(result.participating_nodes), {"n1", "n2", "n3"})
        # Pending should be cleared after a successful solve
        self.assertEqual(self.tdoa.distinct_nodes(em), 0)

    async def test_three_measurements_two_distinct_nodes_uses_midpoint_not_lsq(self):
        """Regression: triangulation must gate on DISTINCT node count, not
        raw measurement count. Three hits from only two nodes (one node
        heard the emitter twice) is rank-deficient — must NOT enter the
        LSQ triangulator (which would silently produce a biased result),
        must instead fall back to the 2-node midpoint."""
        em = "emitter-dup"
        # n1 hears it twice (chatty receiver), n2 once
        self.tdoa.record_measurement(em, self.n1, self.now)
        self.tdoa.record_measurement(em, self.n1, self.now + 500_000)
        self.tdoa.record_measurement(em, self.n2, self.now + 1_000_000)
        self.assertEqual(self.tdoa.distinct_nodes(em), 2)

        # Trip an exception if LSQ triangulator is wrongly invoked
        def _boom(_ms):
            raise AssertionError("LSQ triangulator must not be called "
                                 "when distinct_nodes < 3")
        self.tdoa._triangulate = _boom

        result = await self.tdoa.solve(em)
        self.assertIsNotNone(result)
        # Midpoint is between n1 (35.100, -106.500) and n2 (35.150, -106.450)
        self.assertAlmostEqual(result.estimated_lat, 35.125, places=4)
        self.assertAlmostEqual(result.estimated_lon, -106.475, places=4)
        self.assertAlmostEqual(result.location_confidence, 0.3, places=4)

    async def test_concurrent_record_during_solve_is_not_dropped(self):
        """Regression: measurements that arrive WHILE solve() is awaiting
        the CPU triangulator must not be silently popped. The fix takes
        ownership of pending measurements at the top of the lock so
        anything new accumulates in a fresh queue for the next solve."""
        em = "emitter-race"
        # Seed three nodes so solve() goes down the LSQ path
        self.tdoa.record_measurement(em, self.n1, self.now)
        self.tdoa.record_measurement(em, self.n2, self.now + 1_000_000)
        self.tdoa.record_measurement(em, self.n3, self.now + 2_000_000)

        # Stub triangulator that yields control mid-solve, simulating
        # the await asyncio.to_thread(...) window. While suspended, a new
        # measurement arrives — it must survive the solve.
        n4 = _FakeNode("n4", 35.130, -106.480)
        outer = self
        def _slow_triangulate(_ms):
            # Inject a fresh measurement during the "CPU work" — runs in
            # a worker thread, so the event loop is free to deliver the
            # record_measurement call. Use call_soon_threadsafe to be safe.
            outer.tdoa.record_measurement(em, n4, outer.now + 3_000_000)
            return (35.123, -106.456, 0.85)
        self.tdoa._triangulate = _slow_triangulate

        result = await self.tdoa.solve(em)
        self.assertIsNotNone(result)
        # The n4 measurement that arrived mid-solve must still be queued
        # for a future solve — not dropped by the post-solve cleanup.
        self.assertEqual(self.tdoa.distinct_nodes(em), 1,
            "measurement that arrived mid-solve was lost — race regression")

    async def test_solve_without_quorum_restores_measurements(self):
        """If solve() is called when there aren't enough distinct nodes,
        the measurements it took ownership of must be restored so the
        next call can use them."""
        em = "emitter-onenode"
        self.tdoa.record_measurement(em, self.n1, self.now)
        result = await self.tdoa.solve(em)
        self.assertIsNone(result)
        # Measurement must still be there for the next attempt
        self.assertEqual(self.tdoa.distinct_nodes(em), 1)

    async def test_two_node_solve_produces_midpoint(self):
        em = "emitter-A"
        self.tdoa.record_measurement(em, self.n1, self.now)
        self.tdoa.record_measurement(em, self.n2, self.now + 1_000_000)  # +1ms

        self.assertEqual(self.tdoa.distinct_nodes(em), 2)
        result = await self.tdoa.solve(em)

        self.assertIsNotNone(result)
        self.assertEqual(set(result.participating_nodes), {"n1", "n2"})
        # 2-node path is documented as midpoint
        self.assertAlmostEqual(result.estimated_lat,
                               (self.n1.location_gps[0] + self.n2.location_gps[0]) / 2.0,
                               places=4)
        self.assertAlmostEqual(result.estimated_lon,
                               (self.n1.location_gps[1] + self.n2.location_gps[1]) / 2.0,
                               places=4)
        self.assertGreater(result.location_confidence, 0.0)
        # solve() must clear pending so we don't re-fire on stale data
        self.assertEqual(self.tdoa.distinct_nodes(em), 0)

    async def test_single_node_no_solve(self):
        em = "emitter-B"
        self.tdoa.record_measurement(em, self.n1, self.now)
        self.assertEqual(self.tdoa.distinct_nodes(em), 1)
        result = await self.tdoa.solve(em)
        self.assertIsNone(result, "1 node cannot produce a TDOA fix")

    async def test_node_without_gps_dropped(self):
        em = "emitter-C"
        gpsless = _FakeNode("gpsless", 0.0, 0.0)
        gpsless.location_gps = None  # type: ignore[assignment]

        self.tdoa.record_measurement(em, gpsless, self.now)
        self.tdoa.record_measurement(em, self.n2, self.now + 500_000)

        # gpsless node must not have been recorded
        self.assertEqual(self.tdoa.distinct_nodes(em), 1)
        self.assertIsNone(await self.tdoa.solve(em))

    async def test_node_without_tdoa_capability_now_included_with_low_trust(self):
        """Policy change: nodes WITHOUT a dedicated TDOA timing path
        (e.g. RTL-SDR, phone-bundled SDR) used to be silently dropped.
        Per operator request, they now participate so the operator
        gets at least a search-area fix from cheap hardware. The fix
        is correspondingly marked low-confidence by the timing_factor."""
        em = "emitter-D"
        no_tdoa = _FakeNode("no_tdoa", 35.10, -106.50, can_tdoa=False)
        self.tdoa.record_measurement(em, no_tdoa, self.now)
        self.tdoa.record_measurement(em, self.n2, self.now)
        self.assertEqual(self.tdoa.distinct_nodes(em), 2,
            "non-TDOA-capable node must now be queued (inclusive policy)")
        # And a 2-node solve mixing one cheap + one capable yields a
        # downgraded confidence vs the all-capable case.
        result = await self.tdoa.solve(em)
        self.assertIsNotNone(result)
        # base 0.3 * mean(0.5 cheap-cap, 1.0 capable) = 0.3 * 0.75 = 0.225
        self.assertLess(result.location_confidence, 0.3,
            "mixed-trust fix must be downgraded vs all-capable baseline")
        self.assertGreater(result.location_confidence, 0.0)

    async def test_prune_old_drops_stale_measurements(self):
        em = "emitter-E"
        # n1 heard the emitter 10s ago — too old for the 5s window
        self.tdoa.record_measurement(em, self.n1, self.now - 10 * 10**9)
        # n2 heard it just now
        self.tdoa.record_measurement(em, self.n2, self.now)

        self.assertEqual(self.tdoa.distinct_nodes(em), 2,
                         "both queued before prune")

        self.tdoa.prune_old(em, max_age_s=5.0, now_ns=self.now)

        self.assertEqual(self.tdoa.distinct_nodes(em), 1,
                         "n1 measurement should be dropped as stale")
        # Solve should now refuse — only 1 node remains in window
        self.assertIsNone(await self.tdoa.solve(em))

    async def test_prune_all_old_clears_emitter_entry(self):
        em = "emitter-F"
        self.tdoa.record_measurement(em, self.n1, self.now - 100 * 10**9)
        self.tdoa.record_measurement(em, self.n2, self.now - 100 * 10**9)
        self.tdoa.prune_old(em, max_age_s=5.0, now_ns=self.now)
        self.assertNotIn(em, self.tdoa._pending,
                         "emitter entry should be removed when no measurements remain")
        self.assertEqual(self.tdoa.distinct_nodes(em), 0)

    async def test_two_events_from_same_node_count_as_one(self):
        em = "emitter-G"
        self.tdoa.record_measurement(em, self.n1, self.now)
        self.tdoa.record_measurement(em, self.n1, self.now + 100_000)
        self.assertEqual(self.tdoa.distinct_nodes(em), 1,
                         "two heard-by-n1 events are still one TDOA voice")
        self.assertIsNone(await self.tdoa.solve(em))


if __name__ == "__main__":
    unittest.main(verbosity=2)
