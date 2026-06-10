"""
LOB (Line of Bearing) triangulator.

Converts a list of LOBMeasurement objects (one per KrakenSDR array node) into
a crosscut geolocation fix.  Stateless — the caller owns the per-track
measurement history list and trims it to `history_max` samples before calling.

Algorithm selection:
  0 LOBs  → None
  1 LOB   → None (single bearing has no crosscut)
  2 LOBs  → closed-form 2-line intersection with crossing-angle veto
  3+ LOBs → weighted least-squares (weight = confidence²)

Coordinate system:
  All arithmetic is done in a local flat-earth frame (metres, x east / y north)
  centred on the mean node position.  Distances up to ~50 km are fine;
  beyond that the flat-earth error (~0.01 %) is negligible for DF work.

Crossing-angle veto (2-LOB only):
  If the two bearing lines are nearly parallel (crossing angle < MIN_CROSS_DEG)
  the intersection is geometrically unstable.  The fix is suppressed and the
  caller should wait for a third node or a more favourable geometry.

Error radius:
  Estimated as range_to_crosscut × tan(mean_bearing_uncert_deg) × 2, clamped
  to [LOB_MIN_RADIUS_M, LOB_MAX_RADIUS_M].  This is a rough 1-sigma envelope
  for the DF scatter cone; use it as the map circle radius, not a hard CEP.
"""
from __future__ import annotations

import logging
import math
from dataclasses import dataclass
from typing import List, Optional

from backend.models.lob_measurement import LOBMeasurement

logger = logging.getLogger(__name__)

# ── Tuneable constants ────────────────────────────────────────────────────────

# Minimum crossing angle (degrees) for a 2-node fix to be accepted.
MIN_CROSS_DEG = 15.0

# Crosscut confidence ceiling — even a perfect 3-node fix can't exceed this
# without a TDOA confirmation because antenna phase ambiguity limits DOA.
MAX_LOB_CONFIDENCE = 0.70

# Fix result is clamped to these radii so the map circle is always visible
# but never comically large.
LOB_MIN_RADIUS_M = 20.0
LOB_MAX_RADIUS_M = 50_000.0

# WGS-84 conversions (1-degree approximation at the operating latitude).
_M_PER_DEG_LAT = 111_120.0   # nearly constant


# ── Result types ─────────────────────────────────────────────────────────────

@dataclass
class LOBFix:
    """Crosscut geolocation result from ≥2 bearing lines."""
    estimated_lat: float
    estimated_lon: float
    error_radius_m: float
    location_confidence: float
    contributing_nodes: List[str]
    n_measurements: int
    location_method: str = "lob_crosscut"


# ── Helpers ───────────────────────────────────────────────────────────────────

def _m_per_deg_lon(lat_deg: float) -> float:
    return _M_PER_DEG_LAT * math.cos(math.radians(lat_deg))


def _to_xy(lat: float, lon: float, ref_lat: float, ref_lon: float) -> tuple:
    """Convert WGS-84 to local metres relative to reference point."""
    x = (lon - ref_lon) * _m_per_deg_lon(ref_lat)
    y = (lat - ref_lat) * _M_PER_DEG_LAT
    return x, y


def _from_xy(x: float, y: float, ref_lat: float, ref_lon: float) -> tuple:
    """Convert local metres back to WGS-84."""
    lat = ref_lat + y / _M_PER_DEG_LAT
    lon = ref_lon + x / _m_per_deg_lon(ref_lat)
    return lat, lon


def _bearing_direction(bearing_deg: float) -> tuple:
    """Unit direction vector (east, north) for a true bearing."""
    b = math.radians(bearing_deg)
    return math.sin(b), math.cos(b)   # (dx, dy) in metres


# ── Core triangulator ─────────────────────────────────────────────────────────

class LOBTriangulator:
    """
    Stateless triangulator.  Instantiate once per TrackManager; reuse it for
    all tracks — no internal per-track state is kept.

    Usage:
        triangulator = LOBTriangulator()
        fix = triangulator.triangulate(measurements)   # returns LOBFix | None

    forget(track_id) is a no-op (provided for API parity with CustodyElector
    so TrackManager._age_tracks() can call it unconditionally).
    """

    def __init__(self,
                 min_cross_deg: float = MIN_CROSS_DEG,
                 max_confidence: float = MAX_LOB_CONFIDENCE,
                 min_radius_m: float = LOB_MIN_RADIUS_M,
                 max_radius_m: float = LOB_MAX_RADIUS_M):
        self.min_cross_deg = min_cross_deg
        self.max_confidence = max_confidence
        self.min_radius_m = min_radius_m
        self.max_radius_m = max_radius_m

    # ── Public API ────────────────────────────────────────────────────────

    def triangulate(self, measurements: List[LOBMeasurement]) -> Optional[LOBFix]:
        """
        Return a LOBFix or None.

        None is returned when:
          - Fewer than 2 measurements
          - All measurements are from the same physical position
          - 2-node crossing angle < min_cross_deg
          - Numerical failure
        """
        # Deduplicate: use only the most recent measurement per unique node_id.
        by_node: dict = {}
        for m in measurements:
            if m.node_id not in by_node or m.timestamp_ns > by_node[m.node_id].timestamp_ns:
                by_node[m.node_id] = m
        valid = [m for m in by_node.values()
                 if m.node_lat != 0.0 or m.node_lon != 0.0]

        if len(valid) < 2:
            return None

        if len(valid) == 2:
            return self._two_lob_fix(valid[0], valid[1])
        return self._n_lob_fix(valid)

    def forget(self, track_id: str) -> None:
        """No-op — triangulator carries no per-track state."""

    # ── 2-LOB closed form ─────────────────────────────────────────────────

    def _two_lob_fix(self, m1: LOBMeasurement, m2: LOBMeasurement) -> Optional[LOBFix]:
        ref_lat = (m1.node_lat + m2.node_lat) / 2.0
        ref_lon = (m1.node_lon + m2.node_lon) / 2.0

        x1, y1 = _to_xy(m1.node_lat, m1.node_lon, ref_lat, ref_lon)
        x2, y2 = _to_xy(m2.node_lat, m2.node_lon, ref_lat, ref_lon)

        b1 = math.radians(m1.bearing_deg)
        b2 = math.radians(m2.bearing_deg)

        # Line i: A[i] * [px, py]^T = rhs[i]
        # Derived from: (px - xi)*cos(bi) - (py - yi)*sin(bi) = 0
        # → px*cos(bi) - py*sin(bi) = xi*cos(bi) - yi*sin(bi)
        a11, a12 = math.cos(b1), -math.sin(b1)
        a21, a22 = math.cos(b2), -math.sin(b2)
        r1 = x1 * math.cos(b1) - y1 * math.sin(b1)
        r2 = x2 * math.cos(b2) - y2 * math.sin(b2)

        det = a11 * a22 - a12 * a21   # = sin(b1)*cos(b2) - cos(b1)*sin(b2) = sin(b1-b2) (check)
        cross_sin = abs(det)           # |sin(crossing angle)|
        if cross_sin < math.sin(math.radians(self.min_cross_deg)):
            logger.debug(
                "LOB 2-node fix rejected: crossing angle %.1f° < %.1f° minimum",
                math.degrees(math.asin(min(1.0, cross_sin))), self.min_cross_deg)
            return None

        px = (r1 * a22 - r2 * a12) / det
        py = (a11 * r2 - a21 * r1) / det

        lat, lon = _from_xy(px, py, ref_lat, ref_lon)

        if not _valid_coords(lat, lon):
            return None

        return self._make_fix(
            px, py, ref_lat, ref_lon,
            measurements=[m1, m2],
            cross_quality=cross_sin,
        )

    # ── N-LOB weighted least squares ─────────────────────────────────────

    def _n_lob_fix(self, measurements: List[LOBMeasurement]) -> Optional[LOBFix]:
        try:
            import numpy as np
        except ImportError:
            logger.warning("numpy not available; falling back to 2-LOB fix")
            return self._two_lob_fix(measurements[0], measurements[1])

        ref_lat = sum(m.node_lat for m in measurements) / len(measurements)
        ref_lon = sum(m.node_lon for m in measurements) / len(measurements)

        rows_A = []
        rows_b = []
        weights = []

        for m in measurements:
            xi, yi = _to_xy(m.node_lat, m.node_lon, ref_lat, ref_lon)
            bi = math.radians(m.bearing_deg)
            cb, sb = math.cos(bi), math.sin(bi)
            # Constraint row: cb*px - sb*py = cb*xi - sb*yi
            rows_A.append([cb, -sb])
            rows_b.append(cb * xi - sb * yi)
            weights.append(max(1e-6, m.confidence ** 2))

        A = np.array(rows_A, dtype=float)
        b = np.array(rows_b, dtype=float)
        W = np.diag(weights)

        try:
            AtWA = A.T @ W @ A
            AtWb = A.T @ W @ b
            sol = np.linalg.solve(AtWA, AtWb)
        except np.linalg.LinAlgError:
            logger.warning("LOB N-LOB WLS: singular matrix — falling back to 2-LOB")
            return self._two_lob_fix(measurements[0], measurements[1])

        px, py = float(sol[0]), float(sol[1])
        lat, lon = _from_xy(px, py, ref_lat, ref_lon)

        if not _valid_coords(lat, lon):
            return None

        # Residual spread → error radius
        residuals = []
        for i, m in enumerate(measurements):
            xi, yi = _to_xy(m.node_lat, m.node_lon, ref_lat, ref_lon)
            bi = math.radians(m.bearing_deg)
            cb, sb = math.cos(bi), math.sin(bi)
            # Perpendicular distance from crosscut to bearing line
            res = abs(cb * (px - xi) - sb * (py - yi))
            residuals.append(res)
        rms_residual = math.sqrt(sum(r ** 2 for r in residuals) / len(residuals))

        mean_uncert_rad = math.radians(
            sum(m.bearing_uncert_deg for m in measurements) / len(measurements))
        range_m = math.sqrt(px ** 2 + py ** 2)  # rough range from centroid
        cone_radius = max(range_m * math.tan(mean_uncert_rad), rms_residual)

        radius_m = max(self.min_radius_m, min(self.max_radius_m, cone_radius * 2.0))

        mean_conf = sum(m.confidence for m in measurements) / len(measurements)
        confidence = min(self.max_confidence,
                         mean_conf * (1.0 - radius_m / self.max_radius_m))
        confidence = max(0.05, confidence)

        return LOBFix(
            estimated_lat=round(lat, 7),
            estimated_lon=round(lon, 7),
            error_radius_m=round(radius_m, 1),
            location_confidence=round(confidence, 3),
            contributing_nodes=[m.node_id for m in measurements],
            n_measurements=len(measurements),
            location_method="lob_crosscut",
        )

    # ── Internal helper ───────────────────────────────────────────────────

    def _make_fix(self, px: float, py: float,
                  ref_lat: float, ref_lon: float,
                  measurements: List[LOBMeasurement],
                  cross_quality: float) -> LOBFix:
        lat, lon = _from_xy(px, py, ref_lat, ref_lon)

        mean_uncert_rad = math.radians(
            sum(m.bearing_uncert_deg for m in measurements) / len(measurements))
        range_m = math.sqrt(px ** 2 + py ** 2)
        cone_radius = range_m * math.tan(mean_uncert_rad)

        radius_m = max(self.min_radius_m, min(self.max_radius_m, cone_radius * 2.0))

        mean_conf = sum(m.confidence for m in measurements) / len(measurements)
        # Reward well-crossing geometry (cross_quality = |sin(crossing angle)|)
        confidence = min(self.max_confidence,
                         mean_conf * cross_quality * (1.0 - radius_m / self.max_radius_m))
        confidence = max(0.05, confidence)

        return LOBFix(
            estimated_lat=round(lat, 7),
            estimated_lon=round(lon, 7),
            error_radius_m=round(radius_m, 1),
            location_confidence=round(confidence, 3),
            contributing_nodes=[m.node_id for m in measurements],
            n_measurements=len(measurements),
            location_method="lob_crosscut",
        )


# ── Coordinate validator ──────────────────────────────────────────────────────

def _valid_coords(lat: float, lon: float) -> bool:
    return (
        math.isfinite(lat) and math.isfinite(lon) and
        -90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0
    )
