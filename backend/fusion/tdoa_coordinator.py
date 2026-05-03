import asyncio
import logging
import math
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from backend.models.emitter_track import EmitterTrack
from backend.models.sensor_node import SensorNodeTrust

logger = logging.getLogger(__name__)

SPEED_OF_LIGHT = 299_792_458.0  # m/s


@dataclass
class TDOAMeasurement:
    node_id: str
    timestamp_ns: int
    node_lat: float
    node_lon: float


@dataclass
class TDOAResult:
    emitter_id: str
    estimated_lat: float
    estimated_lon: float
    location_confidence: float
    participating_nodes: List[str] = field(default_factory=list)
    time_differences_ns: Dict[str, int] = field(default_factory=dict)


class TDOACoordinator:
    """
    Coordinate TDOA (Time Difference of Arrival) geolocation.

    Requires ≥2 GPS-synchronized nodes that observed the same emitter.
    Uses a simplified hyperbolic positioning algorithm (works for short baselines).
    """

    def __init__(self):
        self._pending: Dict[str, List[TDOAMeasurement]] = {}  # emitter_id → measurements

    def record_measurement(self, emitter_id: str, node: SensorNodeTrust,
                            timestamp_ns: int):
        """Record that a node heard an emitter at a specific timestamp."""
        if not node.location_gps:
            return  # No location → can't do TDOA
        if not node.can_do_tdoa:
            return

        m = TDOAMeasurement(
            node_id=node.node_id,
            timestamp_ns=timestamp_ns,
            node_lat=node.location_gps[0],
            node_lon=node.location_gps[1],
        )
        self._pending.setdefault(emitter_id, []).append(m)

    async def solve(self, emitter_id: str) -> Optional[TDOAResult]:
        """Attempt TDOA solution for an emitter. Returns None if <2 measurements."""
        measurements = self._pending.get(emitter_id, [])
        if len(measurements) < 2:
            return None

        # Sort by timestamp (reference = earliest)
        measurements.sort(key=lambda m: m.timestamp_ns)
        ref = measurements[0]

        tdiffs: Dict[str, int] = {}
        for m in measurements[1:]:
            tdiffs[f"{ref.node_id}→{m.node_id}"] = m.timestamp_ns - ref.timestamp_ns

        # Simplified: with 2 nodes, only a hyperbolic line is possible;
        # with 3+ nodes we can triangulate.
        if len(measurements) >= 3:
            lat, lon, conf = self._triangulate(measurements)
        else:
            # 2 nodes: midpoint on the TDOA hyperbola (rough estimate)
            lat = (measurements[0].node_lat + measurements[1].node_lat) / 2.0
            lon = (measurements[0].node_lon + measurements[1].node_lon) / 2.0
            conf = 0.3  # Low confidence with only 2 nodes

        result = TDOAResult(
            emitter_id=emitter_id,
            estimated_lat=lat,
            estimated_lon=lon,
            location_confidence=conf,
            participating_nodes=[m.node_id for m in measurements],
            time_differences_ns=tdiffs,
        )
        logger.info("TDOA result for %s: (%.5f, %.5f) conf=%.2f",
                    emitter_id, lat, lon, conf)

        # Clear pending for this emitter
        self._pending.pop(emitter_id, None)
        return result

    def _triangulate(self, measurements: List[TDOAMeasurement]) -> Tuple[float, float, float]:
        """
        Iterative least-squares TDOA triangulation.
        Converts lat/lon to a local ENU frame, solves, then converts back.
        """
        ref = measurements[0]
        ref_lat_r = math.radians(ref.node_lat)
        ref_lon_r = math.radians(ref.node_lon)

        def to_enu(lat: float, lon: float) -> Tuple[float, float]:
            dlat = math.radians(lat - ref.node_lat)
            dlon = math.radians(lon - ref.node_lon)
            e = dlon * math.cos(ref_lat_r) * 6_371_000
            n = dlat * 6_371_000
            return e, n

        node_positions = [to_enu(m.node_lat, m.node_lon) for m in measurements]
        time_diffs_s = [(m.timestamp_ns - measurements[0].timestamp_ns) / 1e9
                        for m in measurements]
        range_diffs = [td * SPEED_OF_LIGHT for td in time_diffs_s]

        # Initial estimate: centroid of node positions
        ex = sum(p[0] for p in node_positions) / len(node_positions)
        ey = sum(p[1] for p in node_positions) / len(node_positions)

        for _ in range(50):  # iterate
            rows = []
            rhs = []
            r0 = math.hypot(ex - node_positions[0][0], ey - node_positions[0][1]) + 1e-6
            for i in range(1, len(node_positions)):
                ri = math.hypot(ex - node_positions[i][0], ey - node_positions[i][1]) + 1e-6
                dx0 = (ex - node_positions[0][0]) / r0
                dy0 = (ey - node_positions[0][1]) / r0
                dxi = (ex - node_positions[i][0]) / ri
                dyi = (ey - node_positions[i][1]) / ri
                rows.append([dxi - dx0, dyi - dy0])
                rhs.append(range_diffs[i] - (ri - r0))

            import numpy as np
            A = np.array(rows)
            b = np.array(rhs)
            if A.shape[0] < 2:
                break
            try:
                delta, _, _, _ = np.linalg.lstsq(A, b, rcond=None)
                ex += delta[0]
                ey += delta[1]
            except Exception:
                break

        # Convert back to lat/lon
        est_lat = ref.node_lat + math.degrees(ey / 6_371_000)
        est_lon = ref.node_lon + math.degrees(ex / (6_371_000 * math.cos(ref_lat_r)))

        # Confidence based on node count and timing quality
        conf = min(0.95, 0.5 + len(measurements) * 0.1)
        return est_lat, est_lon, conf
