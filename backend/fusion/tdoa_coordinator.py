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
    # 0..1 — how much we trust this node's timing for TDOA purposes.
    # 1.0 = GPSDO/OCXO + dedicated TDOA hardware; ~0.3 = generic phone
    # SDR with system-clock-only timestamps. Mean of these across the
    # solve's participating nodes scales the final location confidence.
    timing_trust: float = 1.0


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
        # Per-emitter solve lock — singleflight pattern. Without this,
        # bursty events for one emitter could spawn many concurrent
        # solve() coroutines that race on `_pending` and waste CPU.
        self._solve_locks: Dict[str, asyncio.Lock] = {}

    def record_measurement(self, emitter_id: str, node: SensorNodeTrust,
                            timestamp_ns: int):
        """Record that a node heard an emitter at a specific timestamp.

        Inclusive policy: any node with a GPS fix participates, even if
        its hardware lacks a dedicated TDOA timing path (e.g. RTL-SDR,
        an Android phone's bundled SDR dongle). We need a position fix
        to triangulate at all, but we'll accept system-clock timestamps
        and DOWNGRADE the resulting fix's confidence by the average
        timing_trust of the participants. A rough fix from 4 cheap nodes
        is still operationally useful — it gives the operator a search
        area instead of nothing.
        """
        if not node.location_gps:
            return  # No location → can't triangulate at all

        # Map node capability into a timing-trust score. Use getattr
        # with defaults so test fakes / minimal node shims don't have
        # to populate every hardware-trust field.
        can_tdoa = getattr(node, "can_do_tdoa", False)
        hw_timing = getattr(node, "timing_stability_trust", 0.8)
        if can_tdoa:
            # GPSDO/OCXO timing path — trust the underlying hardware factor
            timing_trust = max(0.5, min(1.0, hw_timing))
        else:
            # System-clock timestamps. Floor of 0.2 so a 3-node solve from
            # cheap nodes still produces a usable (low-confidence) fix.
            # Use half the hardware's timing_stability_trust as the cap.
            timing_trust = max(0.2, min(0.5, hw_timing * 0.5))

        m = TDOAMeasurement(
            node_id=node.node_id,
            timestamp_ns=timestamp_ns,
            node_lat=node.location_gps[0],
            node_lon=node.location_gps[1],
            timing_trust=timing_trust,
        )
        self._pending.setdefault(emitter_id, []).append(m)

    def prune_old(self, emitter_id: str, max_age_s: float = 5.0,
                  now_ns: Optional[int] = None) -> None:
        """Drop measurements older than `max_age_s` from the pending queue
        for `emitter_id`. Critical for correctness: TDOA assumes all
        measurements correlate to the SAME transmission, so anything
        outside a tight time window must be dropped before solving."""
        ms = self._pending.get(emitter_id)
        if not ms:
            return
        import time as _time
        cutoff_ns = (now_ns if now_ns is not None else _time.time_ns()) \
                    - int(max_age_s * 1e9)
        kept = [m for m in ms if m.timestamp_ns >= cutoff_ns]
        if kept:
            self._pending[emitter_id] = kept
        else:
            self._pending.pop(emitter_id, None)

    def distinct_nodes(self, emitter_id: str) -> int:
        """How many distinct sensor nodes have a pending measurement for
        this emitter. Used by the orchestrator to decide whether it's
        worth calling solve()."""
        ms = self._pending.get(emitter_id, [])
        return len({m.node_id for m in ms})

    async def solve(self, emitter_id: str) -> Optional[TDOAResult]:
        """Attempt TDOA solution for an emitter. Returns None unless we
        have measurements from ≥2 *distinct* sensor nodes — two events
        heard by the same receiver carry no time-difference information,
        so they cannot triangulate.

        Concurrency: per-emitter singleflight lock prevents duplicate
        concurrent solves. Heavy CPU work (iterative least-squares) is
        offloaded to a worker thread so the asyncio loop stays
        responsive under bursty event loads.
        """
        lock = self._solve_locks.setdefault(emitter_id, asyncio.Lock())
        async with lock:
            # Atomically TAKE OWNERSHIP of the pending measurements. If we
            # only snapshot-copied, any record_measurement() call landing
            # during the (long) await asyncio.to_thread(...) below would
            # be silently dropped by the final pop(). Pop now, restore on
            # the not-enough-data path, and let any new arrivals
            # accumulate in a fresh queue for the next solve.
            measurements = self._pending.pop(emitter_id, [])
            if len({m.node_id for m in measurements}) < 2:
                if measurements:
                    # Re-merge in case another caller already started a
                    # fresh queue for this emitter.
                    self._pending.setdefault(emitter_id, []).extend(measurements)
                return None

            measurements.sort(key=lambda m: m.timestamp_ns)
            ref = measurements[0]

            tdiffs: Dict[str, int] = {}
            for m in measurements[1:]:
                tdiffs[f"{ref.node_id}→{m.node_id}"] = m.timestamp_ns - ref.timestamp_ns

            # Simplified: with 2 nodes, only a hyperbolic line is
            # possible; with 3+ nodes we can triangulate. Triangulation
            # is CPU-bound (50 LSQ iterations + numpy) so push it off
            # the event loop.
            #
            # Branch on DISTINCT nodes, not raw measurement count: three
            # measurements from only two nodes (duplicate hearings) cannot
            # support LSQ triangulation — the system is rank-deficient and
            # the iterative solver produces unstable/biased results. Fall
            # back to the 2-node midpoint estimate in that case.
            distinct = len({m.node_id for m in measurements})
            if distinct >= 3:
                lat, lon, conf = await asyncio.to_thread(
                    self._triangulate, measurements)
            else:
                # Pick one measurement per node so the midpoint isn't
                # skewed by duplicates from the chattier receiver.
                seen: set = set()
                uniq: List[TDOAMeasurement] = []
                for m in measurements:
                    if m.node_id in seen:
                        continue
                    seen.add(m.node_id)
                    uniq.append(m)
                lat = (uniq[0].node_lat + uniq[1].node_lat) / 2.0
                lon = (uniq[0].node_lon + uniq[1].node_lon) / 2.0
                conf = 0.3  # Low confidence with only 2 nodes

            # Scale by the mean timing trust of the participating
            # measurements. A 3-node fix from cheap RTL-SDR phones
            # (timing_trust ~0.3 each) gets ~30% of the geometric
            # confidence; a fix from GPSDO-equipped HackRFs keeps
            # essentially all of it. Operator sees a "search area"
            # vs a "tight fix" instead of being told "no fix".
            timing_factor = sum(m.timing_trust for m in measurements) / len(measurements)
            conf = conf * timing_factor

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
            # Note: we already popped the input measurements at the top
            # of the lock — measurements that arrived during the await
            # are safely sitting in a fresh queue for the next solve.
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
