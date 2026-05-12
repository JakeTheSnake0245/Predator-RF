"""
StationarityGate — TDOA fix sanity filter + emitter motion classifier.

Background
----------
Before this module existed, every TDOA solve from `_try_tdoa_solve()` was
written straight back to `track.estimated_lat/lon`. That was fine 95% of
the time but had two operational gaps the operator hit in the field:

  1. **Solver glitches corrupted tracks permanently.** A noisy TDOA fix
     occasionally produced a position 50 km away from the real emitter
     (covariance went near-singular, geometry was bad, one node had a
     transient clock skew). The next assessment promoted that ghost
     position to the map and the operator's only recovery was to delete
     the track manually. The frequency of these "TDOA flips" was tied
     to the number of contributing nodes and the geometry — common
     enough to be visible during long missions, never frequent enough
     to produce a clean test case.

  2. **No way to distinguish stationary emitters from mobile ones.**
     The `STABLE` track state was driven purely by `observation_count
     >= 10`, so a moving vehicle's emitter promoted to STABLE just as
     fast as a fixed-site repeater. Operators couldn't tell at a glance
     whether a track had been sitting in one place for an hour or was
     drifting across the map.

This module addresses both gaps with one mechanism: bounded position
history per track, with a velocity sanity check on incoming TDOA fixes
and an RMS-spread classifier that emits `motion_state ∈ {"unknown",
"stationary", "mobile"}` for downstream consumers.

Design contract
---------------
* **Pure scoring** — `evaluate()` is side-effect-free; the caller is
  responsible for applying the result and (if accepted) appending to
  history. Tests can drive the gate without instantiating any tracks.
* **Bypassed for NEW tracks** — without history we have nothing to
  compare against and the first fix is always accepted. The caller
  starts the history with the first accepted fix.
* **dt floor** — fixes spaced <2 s apart bypass the velocity check
  because TDOA error ellipses are typically tens of metres and noise
  alone can imply 100 m/s velocity at sub-second dt. This is a known
  weakness of velocity gates everywhere — kept conservative on
  purpose; the alternative is rejecting legitimate updates from a
  fast-moving fleet during a target chase.
* **RMS-spread classifier** — `classify_motion()` looks at the spread
  of the last N positions relative to the average TDOA error ellipse.
  Spread <= ellipse → stationary (the track hasn't moved more than the
  measurement noise). Spread > 3x ellipse → mobile. In between →
  whatever the prior state was (hysteresis, so a track doesn't flap
  between states each fix).
* **Configurable v_max** — default 100 m/s ≈ 360 km/h covers cars,
  boats, general aviation. Operators chasing a faster target (helicopter,
  jet) bump it up via `StationarityGate(v_max_mps=...)` at construct
  time. Setting it to inf disables the velocity gate while keeping the
  motion classifier.
"""
from __future__ import annotations

import logging
import math
import time
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

logger = logging.getLogger(__name__)

# ── Tunable defaults ──────────────────────────────────────────────────────

# 100 m/s ≈ 360 km/h. Captures cars, boats, light aircraft. Helicopters
# top out around 90 m/s; jets exceed this and operators chasing one need
# to override at construct time.
DEFAULT_V_MAX_MPS = 100.0

# Below this dt the velocity computation is too noise-sensitive to be
# meaningful — small TDOA error spikes alone imply tens of m/s.
DEFAULT_DT_FLOOR_S = 2.0

# Position history length. 20 is enough for the RMS classifier to settle
# (the classifier discards the oldest entries when the track exits the
# stationary state and a fresh trajectory begins). Bounded so a long-
# running track doesn't grow without bound.
DEFAULT_HISTORY_MAX = 20

# Motion classifier thresholds — multiples of the average ellipse a-axis
# over the history window. Stationary if spread <= 1.0x ellipse; mobile
# if spread > 3.0x. Between is hysteresis territory and inherits the
# previous state so a borderline track doesn't flap.
DEFAULT_STATIONARY_RATIO = 1.0
DEFAULT_MOBILE_RATIO     = 3.0

# When no ellipse has been published yet (e.g. RSSI-proximity fallback,
# or the first few fixes before TDOA stabilises) we fall back to a fixed
# scale so the classifier still produces a verdict instead of always
# returning "unknown". 250 m is the radius of a typical urban emitter
# coverage cell — anything tighter than this we'd call stationary even
# without an ellipse, anything wider we'd flag as mobile.
DEFAULT_FALLBACK_ELLIPSE_M = 250.0


# ── Result dataclasses ────────────────────────────────────────────────────


@dataclass
class FixCandidate:
    """One TDOA solver output ready for the gate to evaluate."""
    lat: float
    lon: float
    ellipse_a_m: Optional[float] = None    # 1-sigma semi-major; None = unknown
    timestamp_ns: int = field(default_factory=time.time_ns)


@dataclass
class HistoryPoint:
    """One accepted fix kept on the track's history buffer."""
    lat: float
    lon: float
    timestamp_ns: int
    ellipse_a_m: Optional[float] = None


@dataclass
class GateVerdict:
    """Result of evaluating one candidate against a track's history."""
    accepted: bool
    reason: str                                  # human-readable, for logs/UI
    implied_velocity_mps: Optional[float] = None  # None when no comparison
    motion_state: str = "unknown"                # forward of next classify
    bypass: str = ""                             # "new_track" | "dt_floor" | ""

    def to_dict(self) -> dict:
        return {
            "accepted": self.accepted,
            "reason": self.reason,
            "implied_velocity_mps": (
                round(self.implied_velocity_mps, 2)
                if self.implied_velocity_mps is not None else None),
            "motion_state": self.motion_state,
            "bypass": self.bypass,
        }


# ── Geo helper ────────────────────────────────────────────────────────────


def _haversine_m(a_lat: float, a_lon: float,
                 b_lat: float, b_lon: float) -> float:
    """Great-circle distance in metres between two WGS-84 points.
    Local copy (rather than importing fusion.cross_station_dedup) so
    this module stays free of fusion-layer cross-imports."""
    R = 6_371_000.0
    a_lat_r = math.radians(a_lat)
    b_lat_r = math.radians(b_lat)
    d_lat = b_lat_r - a_lat_r
    d_lon = math.radians(b_lon - a_lon)
    h = (math.sin(d_lat / 2) ** 2
         + math.cos(a_lat_r) * math.cos(b_lat_r) * math.sin(d_lon / 2) ** 2)
    return 2 * R * math.asin(math.sqrt(h))


# ── Gate ──────────────────────────────────────────────────────────────────


class StationarityGate:
    """Stateless evaluator. The caller owns the per-track history list
    and passes it in to each `evaluate()` call. Stateless was chosen
    deliberately: the gate is naturally per-track but TrackManager
    already owns track lifecycle, so making the gate own a parallel
    cache would just duplicate that bookkeeping (and create another
    forget()-on-archive contract like the CustodyElector has)."""

    def __init__(self,
                 *,
                 v_max_mps: float = DEFAULT_V_MAX_MPS,
                 dt_floor_s: float = DEFAULT_DT_FLOOR_S,
                 history_max: int = DEFAULT_HISTORY_MAX,
                 stationary_ratio: float = DEFAULT_STATIONARY_RATIO,
                 mobile_ratio: float = DEFAULT_MOBILE_RATIO,
                 fallback_ellipse_m: float = DEFAULT_FALLBACK_ELLIPSE_M):
        if v_max_mps <= 0:
            raise ValueError("v_max_mps must be > 0")
        if history_max < 2:
            raise ValueError("history_max must be >= 2")
        self.v_max_mps        = float(v_max_mps)
        self.dt_floor_s       = float(dt_floor_s)
        self.history_max      = int(history_max)
        self.stationary_ratio = float(stationary_ratio)
        self.mobile_ratio     = float(mobile_ratio)
        self.fallback_ellipse_m = float(fallback_ellipse_m)

        # Cheap counters for /metrics + tests.
        self.fixes_evaluated     = 0
        self.fixes_accepted      = 0
        self.fixes_rejected_velocity = 0
        self.fixes_rejected_invalid  = 0
        self.fixes_bypassed_new  = 0
        self.fixes_bypassed_dt   = 0

    # ── Public API ───────────────────────────────────────────────────────

    def evaluate(self,
                 candidate: FixCandidate,
                 history: List[HistoryPoint],
                 *,
                 prior_motion_state: str = "unknown") -> GateVerdict:
        """Decide whether to accept `candidate`.

        Args:
            candidate: New TDOA fix to evaluate.
            history: Bounded list of previously-accepted fixes (most
                recent last). Empty for a NEW track.
            prior_motion_state: The track's current motion_state (for
                hysteresis on borderline classifications).

        Returns:
            GateVerdict. The caller is responsible for appending the
            candidate to history when `accepted` is True (and for
            trimming the history to `history_max`).
        """
        self.fixes_evaluated += 1

        # Candidate validation runs FIRST. A NaN/inf lat/lon from the
        # solver makes every subsequent comparison evaluate False
        # (`nan > x` is always False), so the velocity gate would
        # silently accept the malformed fix and write it to
        # `track.estimated_lat/lon`, reintroducing exactly the
        # corruption mode the gate exists to prevent. Same for
        # out-of-range lat/lon (a singular covariance can produce
        # |lat| > 90 in degenerate solves). We reject before any
        # distance math runs.
        if not (math.isfinite(candidate.lat) and math.isfinite(candidate.lon)
                and -90.0 <= candidate.lat <= 90.0
                and -180.0 <= candidate.lon <= 180.0):
            self.fixes_rejected_invalid += 1
            return GateVerdict(
                accepted=False,
                reason=(f"invalid_coordinates "
                        f"(lat={candidate.lat}, lon={candidate.lon})"),
                motion_state=self.classify_motion(history, prior_motion_state),
            )
        # Timestamp sanity — must be positive and finite. A garbage
        # timestamp would feed straight into the dt computation and
        # mis-classify legitimate fixes as dt-floor bypasses.
        if (candidate.timestamp_ns <= 0
                or not math.isfinite(float(candidate.timestamp_ns))):
            self.fixes_rejected_invalid += 1
            return GateVerdict(
                accepted=False,
                reason=f"invalid_timestamp ({candidate.timestamp_ns})",
                motion_state=self.classify_motion(history, prior_motion_state),
            )

        if not history:
            # No comparison possible — accept and let the next call
            # establish a baseline. Motion classifier needs >=2 points.
            self.fixes_accepted += 1
            self.fixes_bypassed_new += 1
            return GateVerdict(
                accepted=True,
                reason="bypass_new_track",
                bypass="new_track",
                motion_state="unknown",
            )

        last = history[-1]
        dist_m = _haversine_m(last.lat, last.lon, candidate.lat, candidate.lon)
        dt_s = max(0.0, (candidate.timestamp_ns - last.timestamp_ns) / 1e9)

        if dt_s < self.dt_floor_s:
            # Sub-floor dt — accept without velocity check. Classifier
            # runs on history + candidate (NOT just history) so the
            # caller sees the post-acceptance motion_state without a
            # one-step lag. Without this, a track receiving frequent
            # sub-floor updates (e.g. high-cadence TDOA on a stationary
            # emitter) would have its motion_state stuck behind the
            # actual position spread by exactly one fix, which blurs
            # hysteresis at transition boundaries.
            self.fixes_accepted += 1
            self.fixes_bypassed_dt += 1
            new_hist = history + [HistoryPoint(
                lat=candidate.lat, lon=candidate.lon,
                timestamp_ns=candidate.timestamp_ns,
                ellipse_a_m=candidate.ellipse_a_m)]
            return GateVerdict(
                accepted=True,
                reason="bypass_dt_floor",
                bypass="dt_floor",
                motion_state=self.classify_motion(new_hist,
                                                  prior_motion_state),
                implied_velocity_mps=None,
            )

        velocity_mps = dist_m / dt_s
        if velocity_mps > self.v_max_mps:
            self.fixes_rejected_velocity += 1
            return GateVerdict(
                accepted=False,
                reason=(f"velocity_exceeds_limit "
                        f"({velocity_mps:.0f}m/s > {self.v_max_mps:.0f}m/s)"),
                implied_velocity_mps=velocity_mps,
                motion_state=self.classify_motion(history, prior_motion_state),
            )

        self.fixes_accepted += 1
        # Run the classifier on history + the new candidate so the
        # caller sees the post-acceptance state without having to
        # call classify_motion() separately.
        new_hist = history + [HistoryPoint(
            lat=candidate.lat, lon=candidate.lon,
            timestamp_ns=candidate.timestamp_ns,
            ellipse_a_m=candidate.ellipse_a_m)]
        return GateVerdict(
            accepted=True,
            reason="accepted",
            implied_velocity_mps=velocity_mps,
            motion_state=self.classify_motion(new_hist, prior_motion_state),
        )

    def classify_motion(self,
                        history: List[HistoryPoint],
                        prior_state: str = "unknown") -> str:
        """Compute motion_state from a position history. Pure function.

        Algorithm: compute the centroid of the history, then the RMS
        distance of each point to that centroid. Compare the RMS to the
        average ellipse a-axis (or fallback_ellipse_m if no ellipse is
        recorded on any point). Stationary if RMS <= stationary_ratio *
        ellipse_avg; mobile if > mobile_ratio * ellipse_avg; otherwise
        keep the prior_state (hysteresis).
        """
        if len(history) < 2:
            return "unknown"

        # Centroid in lat/lon — for the history sizes we use (≤ 20
        # points), the planar approximation introduces negligible error
        # vs proper spherical centroid math. The ratio-to-ellipse
        # comparison matters far more than absolute-metres precision.
        cx = sum(p.lat for p in history) / len(history)
        cy = sum(p.lon for p in history) / len(history)
        rms_m = math.sqrt(
            sum(_haversine_m(cx, cy, p.lat, p.lon) ** 2 for p in history)
            / len(history))

        ellipses = [p.ellipse_a_m for p in history if p.ellipse_a_m is not None]
        ellipse_avg = (sum(ellipses) / len(ellipses)
                       if ellipses else self.fallback_ellipse_m)
        if ellipse_avg <= 0:
            ellipse_avg = self.fallback_ellipse_m

        if rms_m <= self.stationary_ratio * ellipse_avg:
            return "stationary"
        if rms_m > self.mobile_ratio * ellipse_avg:
            return "mobile"
        # Borderline — keep prior state for hysteresis.
        return prior_state if prior_state in ("stationary", "mobile") \
            else "unknown"

    def stats(self) -> dict:
        return {
            "v_max_mps": self.v_max_mps,
            "dt_floor_s": self.dt_floor_s,
            "history_max": self.history_max,
            "fixes_evaluated":          self.fixes_evaluated,
            "fixes_accepted":           self.fixes_accepted,
            "fixes_rejected_velocity":  self.fixes_rejected_velocity,
            "fixes_rejected_invalid":   self.fixes_rejected_invalid,
            "fixes_bypassed_new":       self.fixes_bypassed_new,
            "fixes_bypassed_dt":        self.fixes_bypassed_dt,
        }
