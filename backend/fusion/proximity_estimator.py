"""
Single-node RSSI proximity estimator.

Fallback geolocation for the case where the system has only ONE sensor
node hearing an emitter (or no GPS-synced pair within the TDOA window).
Converts the received power into a coarse range using a free-space
path-loss model + an assumed transmitter EIRP, and centres a wide
circle on the detecting node's GPS position.

This is NOT a real geolocation. It is "the emitter is somewhere within
roughly N hundred metres of this sensor." The operator must see it
labelled as `location_method = "rssi_proximity"` and treat the radius
as the search area, not the position.

Why an estimate is still useful:
  - Without it, a single-phone operator gets no map dot at all for
    detected emitters — only the phone's own position. This destroys
    the "walk the perimeter" workflow.
  - With it, the dot lands at the phone with a wide error circle that
    visibly shrinks as the operator walks closer (signal gets stronger
    → estimated range shrinks). The operator can DF visually.

Why we won't pretend it's better than it is:
  - TX power is unknown — we assume `rssi_assumed_eirp_dbm`.
  - No bearing info — circle, never a dot.
  - Free-space path loss ignores clutter, multipath, antenna pattern.
  - power_dbfs → dBm conversion is approximate without per-node
    absolute calibration.

Confidence is hard-capped at 0.20 so any downstream consumer (CoT
emitter, dedup, UI) treats it as low-quality.
"""
from __future__ import annotations

import logging
import math
import time
from dataclasses import dataclass
from typing import Optional, Tuple

logger = logging.getLogger(__name__)

# Speed of light, m/s — for dBm-to-distance via free-space path loss.
_SPEED_OF_LIGHT = 299_792_458.0

# Hard ceiling on confidence regardless of signal strength. Even a
# strong RSSI doesn't tell us the emitter's actual TX power, so we
# cannot ever be highly confident about distance.
_MAX_CONFIDENCE = 0.20


@dataclass
class ProximityFix:
    """Result of a single-node RSSI proximity estimate."""
    estimated_lat: float
    estimated_lon: float
    estimated_range_m: float        # raw path-loss range estimate
    error_radius_m: float           # rendered circle radius (range × factor, clamped)
    location_confidence: float      # in [0, _MAX_CONFIDENCE]
    method: str = "rssi_proximity"
    sensor_node_id: str = ""
    timestamp_ns: int = 0


class ProximityEstimator:
    """Free-space path-loss → range, centred on the detecting node."""

    def __init__(self,
                 assumed_eirp_dbm: float = 30.0,
                 dbfs_to_dbm_offset: float = -30.0,
                 radius_uncertainty_factor: float = 2.0,
                 min_radius_m: float = 50.0,
                 max_radius_m: float = 5000.0):
        self.assumed_eirp_dbm = assumed_eirp_dbm
        self.dbfs_to_dbm_offset = dbfs_to_dbm_offset
        self.radius_uncertainty_factor = max(1.0, radius_uncertainty_factor)
        self.min_radius_m = max(1.0, min_radius_m)
        self.max_radius_m = max(self.min_radius_m, max_radius_m)

    # ── Core estimate ────────────────────────────────────────────────────

    def estimate(self,
                 power_dbfs: float,
                 frequency_hz: float,
                 node_lat: Optional[float],
                 node_lon: Optional[float],
                 node_id: str = "",
                 timestamp_ns: Optional[int] = None) -> Optional[ProximityFix]:
        """
        Return a ProximityFix or None if we can't produce one (e.g. no
        node GPS, invalid frequency, pathological power).

        The fix is centred on the detecting node — we have NO bearing
        information, so the circle around `node_lat, node_lon` is the
        operator's actual situational picture: "the emitter is within
        `error_radius_m` of this sensor, in some unknown direction."
        """
        if node_lat is None or node_lon is None:
            return None
        if frequency_hz <= 0:
            return None

        pr_dbm = self._dbfs_to_dbm(power_dbfs)
        # Avoid pathological math when the receiver is in deep noise
        # OR when the receiver is being saturated by a co-located TX.
        # Either way the range estimate would be meaningless.
        if pr_dbm > self.assumed_eirp_dbm + 5.0:
            # We're "hearing" more power than we assumed the emitter
            # transmits — almost certainly RX is being de-sensed by a
            # nearby strong source, or the EIRP assumption is way off.
            # Pin to the minimum radius to flag "extremely close."
            range_m = 1.0
        elif pr_dbm < self.assumed_eirp_dbm - 200.0:
            # Free-space path loss > 200 dB ≈ tens of millions of km;
            # that's a noise-floor read, not a real detection.
            return None
        else:
            range_m = self._free_space_range_m(
                pt_dbm=self.assumed_eirp_dbm,
                pr_dbm=pr_dbm,
                frequency_hz=frequency_hz)

        radius_m = max(self.min_radius_m,
                       min(self.max_radius_m,
                           range_m * self.radius_uncertainty_factor))

        # Confidence falls off as the radius grows toward max — wide
        # circles are inherently low-confidence because they could be
        # either "very far" or "we have no idea." Hard-capped at 0.20.
        radius_span = max(1.0, self.max_radius_m - self.min_radius_m)
        radius_normalised = (radius_m - self.min_radius_m) / radius_span
        confidence = _MAX_CONFIDENCE * (1.0 - radius_normalised)
        confidence = max(0.05, min(_MAX_CONFIDENCE, confidence))

        return ProximityFix(
            estimated_lat=float(node_lat),
            estimated_lon=float(node_lon),
            estimated_range_m=float(range_m),
            error_radius_m=float(radius_m),
            location_confidence=float(confidence),
            sensor_node_id=node_id,
            timestamp_ns=timestamp_ns if timestamp_ns is not None else time.time_ns(),
        )

    # ── Helpers ──────────────────────────────────────────────────────────

    def _dbfs_to_dbm(self, power_dbfs: float) -> float:
        """Approximate dBm at the antenna from the SDR's dBFS reading.

        Without per-node absolute power calibration this is a constant
        offset. The Calibrator module tightens this per-node when a
        known reference is available; otherwise we fall back to the
        operator-supplied default."""
        return float(power_dbfs) + self.dbfs_to_dbm_offset

    @staticmethod
    def _free_space_range_m(pt_dbm: float, pr_dbm: float,
                             frequency_hz: float) -> float:
        """Free-space path loss, solved for distance.

        FSPL_dB = 20·log10(4·π·d·f / c)
        Pr_dBm  = Pt_dBm - FSPL_dB    (assuming unity antenna gains)
        ⇒ d_m = c / (4·π·f) · 10 ^ ((Pt_dBm - Pr_dBm) / 20)
        """
        loss_db = pt_dbm - pr_dbm
        # Floor the loss so we don't divide by zero or take log of negative.
        loss_db = max(0.0, loss_db)
        return (_SPEED_OF_LIGHT / (4.0 * math.pi * frequency_hz)) * \
               (10.0 ** (loss_db / 20.0))
