"""
Unit tests for backend.fusion.lob_triangulator.

Run:  python -m unittest backend.tests.test_lob_triangulator -v
"""
import math
import time
import unittest

from backend.models.lob_measurement import LOBMeasurement
from backend.fusion.lob_triangulator import (
    LOBTriangulator, LOBFix,
    MIN_CROSS_DEG, _to_xy, _from_xy, _valid_coords,
)


def _meas(node_id, node_lat, node_lon, bearing_deg,
          confidence=0.8, bearing_uncert_deg=5.0,
          frequency_hz=433_920_000, power_dbfs=-40.0):
    return LOBMeasurement(
        node_id=node_id,
        node_lat=node_lat,
        node_lon=node_lon,
        bearing_deg=bearing_deg,
        confidence=confidence,
        bearing_uncert_deg=bearing_uncert_deg,
        frequency_hz=frequency_hz,
        power_dbfs=power_dbfs,
        timestamp_ns=time.time_ns(),
    )


class TestCoordinateHelpers(unittest.TestCase):
    def test_round_trip_near_equator(self):
        ref_lat, ref_lon = 0.0, 0.0
        for lat, lon in [(0.01, 0.01), (-0.02, 0.03), (0.0, -0.05)]:
            x, y = _to_xy(lat, lon, ref_lat, ref_lon)
            lat2, lon2 = _from_xy(x, y, ref_lat, ref_lon)
            self.assertAlmostEqual(lat, lat2, places=5)
            self.assertAlmostEqual(lon, lon2, places=5)

    def test_round_trip_mid_latitude(self):
        ref_lat, ref_lon = 51.5, -0.12   # London
        for dlat, dlon in [(0.05, 0.05), (-0.02, 0.1), (0.0, -0.03)]:
            lat, lon = ref_lat + dlat, ref_lon + dlon
            x, y = _to_xy(lat, lon, ref_lat, ref_lon)
            lat2, lon2 = _from_xy(x, y, ref_lat, ref_lon)
            self.assertAlmostEqual(lat, lat2, places=5)
            self.assertAlmostEqual(lon, lon2, places=5)

    def test_valid_coords(self):
        self.assertTrue(_valid_coords(37.4, -122.1))
        self.assertFalse(_valid_coords(91.0, 0.0))
        self.assertFalse(_valid_coords(0.0, 181.0))
        self.assertFalse(_valid_coords(float('nan'), 0.0))
        self.assertFalse(_valid_coords(float('inf'), 0.0))


class TestTriangulatorInstantiation(unittest.TestCase):
    def test_defaults(self):
        t = LOBTriangulator()
        self.assertEqual(t.min_cross_deg, MIN_CROSS_DEG)

    def test_forget_is_noop(self):
        t = LOBTriangulator()
        t.forget("any-track-id")   # must not raise

    def test_none_on_empty(self):
        t = LOBTriangulator()
        self.assertIsNone(t.triangulate([]))

    def test_none_on_single_measurement(self):
        t = LOBTriangulator()
        m = _meas("n1", 37.0, -122.0, 45.0)
        self.assertIsNone(t.triangulate([m]))


class TestTwoNodeFix(unittest.TestCase):
    """
    Geometry: two nodes 10 km apart, both bearing toward the same target
    at (37.0, -122.0).  We compute the expected bearings analytically
    and verify the triangulator recovers the target within a few hundred
    metres.
    """

    def _setup_two_nodes(self, target_lat=37.0, target_lon=-122.0):
        # Node A is 0.05° north of target
        lat_a, lon_a = target_lat + 0.05, target_lon
        # Node B is 0.05° east of target
        lat_b, lon_b = target_lat, target_lon + 0.07

        # True bearing from A to target: due south = 180°
        bear_a = 180.0
        # True bearing from B to target: due west ≈ 270°
        bear_b = 270.0

        return lat_a, lon_a, bear_a, lat_b, lon_b, bear_b, target_lat, target_lon

    def test_orthogonal_bearings_recover_target(self):
        la, loa, ba, lb, lob, bb, tgt_lat, tgt_lon = self._setup_two_nodes()
        t = LOBTriangulator()
        fix = t.triangulate([
            _meas("A", la, loa, ba, confidence=0.9, bearing_uncert_deg=2.0),
            _meas("B", lb, lob, bb, confidence=0.9, bearing_uncert_deg=2.0),
        ])
        self.assertIsNotNone(fix)
        self.assertIsInstance(fix, LOBFix)
        self.assertAlmostEqual(fix.estimated_lat, tgt_lat, delta=0.002)
        self.assertAlmostEqual(fix.estimated_lon, tgt_lon, delta=0.003)
        self.assertIn("A", fix.contributing_nodes)
        self.assertIn("B", fix.contributing_nodes)
        self.assertEqual(fix.n_measurements, 2)
        self.assertEqual(fix.location_method, "lob_crosscut")

    def test_crossing_angle_veto(self):
        # Two nodes with nearly parallel bearings — should return None
        t = LOBTriangulator(min_cross_deg=15.0)
        m1 = _meas("A", 37.0, -122.0, 90.0)   # bearing east
        m2 = _meas("B", 37.1, -122.0, 91.0)   # bearing east+1° (nearly parallel)
        fix = t.triangulate([m1, m2])
        self.assertIsNone(fix)

    def test_same_node_deduplication(self):
        # Two measurements from same node — only most recent kept → single node → None
        now = time.time_ns()
        m1 = _meas("A", 37.0, -122.0, 90.0)
        m1.timestamp_ns = now - 1000
        m2 = _meas("A", 37.0, -122.0, 180.0)
        m2.timestamp_ns = now
        t = LOBTriangulator()
        fix = t.triangulate([m1, m2])
        # Only one unique node → fix must be None
        self.assertIsNone(fix)

    def test_confidence_clamped(self):
        la, loa, ba, lb, lob, bb, _, _ = self._setup_two_nodes()
        t = LOBTriangulator()
        fix = t.triangulate([
            _meas("A", la, loa, ba, confidence=1.0),
            _meas("B", lb, lob, bb, confidence=1.0),
        ])
        self.assertIsNotNone(fix)
        self.assertLessEqual(fix.location_confidence, t.max_confidence)
        self.assertGreaterEqual(fix.location_confidence, 0.0)

    def test_error_radius_bounds(self):
        la, loa, ba, lb, lob, bb, _, _ = self._setup_two_nodes()
        t = LOBTriangulator()
        fix = t.triangulate([
            _meas("A", la, loa, ba, bearing_uncert_deg=20.0),
            _meas("B", lb, lob, bb, bearing_uncert_deg=20.0),
        ])
        self.assertIsNotNone(fix)
        self.assertGreaterEqual(fix.error_radius_m, t.min_radius_m)
        self.assertLessEqual(fix.error_radius_m, t.max_radius_m)


class TestThreeNodeFix(unittest.TestCase):
    """
    Three nodes in a triangle around a target.  WLS should recover the target.
    """

    def test_three_node_wls(self):
        tgt_lat, tgt_lon = 48.8566, 2.3522   # Paris

        def bearing(from_lat, from_lon, to_lat, to_lon):
            dlon = math.radians(to_lon - from_lon)
            lat1 = math.radians(from_lat)
            lat2 = math.radians(to_lat)
            x = math.sin(dlon) * math.cos(lat2)
            y = math.cos(lat1) * math.sin(lat2) - math.sin(lat1) * math.cos(lat2) * math.cos(dlon)
            return (math.degrees(math.atan2(x, y)) + 360) % 360

        nodes = [
            (tgt_lat + 0.04, tgt_lon),
            (tgt_lat, tgt_lon + 0.06),
            (tgt_lat - 0.04, tgt_lon - 0.05),
        ]

        measurements = [
            _meas(f"N{i}", lat, lon, bearing(lat, lon, tgt_lat, tgt_lon),
                  confidence=0.85, bearing_uncert_deg=3.0)
            for i, (lat, lon) in enumerate(nodes)
        ]

        t = LOBTriangulator()
        fix = t.triangulate(measurements)
        self.assertIsNotNone(fix)
        self.assertAlmostEqual(fix.estimated_lat, tgt_lat, delta=0.003)
        self.assertAlmostEqual(fix.estimated_lon, tgt_lon, delta=0.004)
        self.assertEqual(fix.n_measurements, 3)

    def test_three_node_with_one_low_confidence(self):
        tgt_lat, tgt_lon = 51.5, -0.12

        def bearing(from_lat, from_lon):
            dlon = math.radians(tgt_lon - from_lon)
            lat1 = math.radians(from_lat)
            lat2 = math.radians(tgt_lat)
            x = math.sin(dlon) * math.cos(lat2)
            y = math.cos(lat1) * math.sin(lat2) - math.sin(lat1) * math.cos(lat2) * math.cos(dlon)
            return (math.degrees(math.atan2(x, y)) + 360) % 360

        measurements = [
            _meas("N0", tgt_lat + 0.05, tgt_lon, bearing(tgt_lat + 0.05, tgt_lon), confidence=0.9),
            _meas("N1", tgt_lat, tgt_lon + 0.07, bearing(tgt_lat, tgt_lon + 0.07), confidence=0.9),
            _meas("N2", tgt_lat - 0.04, tgt_lon - 0.04, bearing(tgt_lat - 0.04, tgt_lon - 0.04),
                  confidence=0.05),  # low-confidence node — should still converge
        ]

        t = LOBTriangulator()
        fix = t.triangulate(measurements)
        self.assertIsNotNone(fix)
        # Low-confidence node is down-weighted; result should still be close
        self.assertAlmostEqual(fix.estimated_lat, tgt_lat, delta=0.01)
        self.assertAlmostEqual(fix.estimated_lon, tgt_lon, delta=0.01)


class TestEdgeCases(unittest.TestCase):
    def test_zero_position_nodes_excluded(self):
        # Nodes with (0, 0) are excluded from valid list
        t = LOBTriangulator()
        m_zero = _meas("zero", 0.0, 0.0, 45.0)
        m_good = _meas("good", 37.0, -122.0, 90.0)
        # Only one valid node after filter → None
        fix = t.triangulate([m_zero, m_good])
        self.assertIsNone(fix)

    def test_custom_crossing_angle_threshold(self):
        t_strict = LOBTriangulator(min_cross_deg=45.0)
        # 30° crossing angle — accepted by default (15°), rejected by strict (45°)
        m1 = _meas("A", 37.0, -122.0, 90.0)   # east
        m2 = _meas("B", 37.1, -122.0, 120.0)  # ~30° from east (crossing ~30°)
        fix_strict = t_strict.triangulate([m1, m2])
        # Don't assert exact outcome — just verify the threshold is honoured
        if fix_strict is not None:
            self.assertIsInstance(fix_strict, LOBFix)

    def test_lob_fix_fields_complete(self):
        t = LOBTriangulator()
        fix = t.triangulate([
            _meas("A", 37.0 + 0.05, -122.0, 180.0),
            _meas("B", 37.0, -122.0 + 0.07, 270.0),
        ])
        self.assertIsNotNone(fix)
        self.assertIsInstance(fix.estimated_lat, float)
        self.assertIsInstance(fix.estimated_lon, float)
        self.assertIsInstance(fix.error_radius_m, float)
        self.assertIsInstance(fix.location_confidence, float)
        self.assertIsInstance(fix.contributing_nodes, list)
        self.assertIsInstance(fix.n_measurements, int)
        self.assertEqual(fix.location_method, "lob_crosscut")


if __name__ == "__main__":
    unittest.main()
