"""TDOAResult error ellipse: high-confidence fix → small ellipse;
collinear node geometry → narrow eccentric ellipse."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.fusion.tdoa_coordinator import TDOACoordinator, TDOAMeasurement


def _m(node_id, lat, lon, ts=0):
    return TDOAMeasurement(node_id=node_id, timestamp_ns=ts,
                           node_lat=lat, node_lon=lon, timing_trust=1.0)


class ErrorEllipseTests(unittest.TestCase):
    def test_high_confidence_yields_small_ellipse(self):
        ms = [_m("a", 35.0, -106.0), _m("b", 35.01, -106.0),
              _m("c", 35.0, -106.01)]
        a, b, theta = TDOACoordinator._estimate_ellipse(ms, conf=0.9)
        # base = 50 + (1-0.9)*4950 = 545 m
        self.assertLess(a, 600)
        self.assertGreater(a, 400)

    def test_low_confidence_yields_large_ellipse(self):
        ms = [_m("a", 35.0, -106.0), _m("b", 35.1, -106.0),
              _m("c", 35.0, -106.1)]
        a, _, _ = TDOACoordinator._estimate_ellipse(ms, conf=0.0)
        self.assertGreater(a, 4000)  # ~5 km at zero confidence

    def test_collinear_nodes_yield_eccentric_ellipse(self):
        # Three nodes strung E-W → spread on x, none on y → very
        # different eigenvalues → b/a ratio hits the 0.2 floor.
        ms = [_m("a", 35.0, -106.000), _m("b", 35.0, -106.010),
              _m("c", 35.0, -106.020)]
        a, b, theta = TDOACoordinator._estimate_ellipse(ms, conf=0.5)
        self.assertGreater(a, b)
        # Should hit the eccentricity floor of 0.2 (not infinite)
        self.assertGreaterEqual(b / a, 0.19)

    def test_clustered_nodes_yield_circular_ellipse(self):
        # Symmetric square cluster, sized in METRES (lon offset is
        # divided by cos(lat) so x and y spreads match in metres-space,
        # not degree-space) → equal eigenvalues → b/a ≈ 1
        import math
        lat0 = 35.0
        d_deg_lat = 0.001
        d_deg_lon = 0.001 / math.cos(math.radians(lat0))
        ms = [_m("a", lat0 + d_deg_lat, -106.0 + d_deg_lon),
              _m("b", lat0 + d_deg_lat, -106.0 - d_deg_lon),
              _m("c", lat0 - d_deg_lat, -106.0 + d_deg_lon),
              _m("d", lat0 - d_deg_lat, -106.0 - d_deg_lon)]
        a, b, _ = TDOACoordinator._estimate_ellipse(ms, conf=0.5)
        self.assertAlmostEqual(b / a, 1.0, places=2)

    def test_two_node_minimum(self):
        ms = [_m("a", 35.0, -106.0), _m("b", 35.01, -106.0)]
        a, b, _ = TDOACoordinator._estimate_ellipse(ms, conf=0.5)
        # Should still produce a sensible ellipse, not crash
        self.assertGreater(a, 0)
        self.assertGreater(b, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
