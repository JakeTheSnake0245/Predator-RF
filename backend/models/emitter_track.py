from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional, Dict
import time
import uuid


class TrackState(Enum):
    NEW = "new"
    TRACKING = "tracking"
    STABLE = "stable"
    COASTING = "coasting"
    LOST = "lost"


@dataclass
class EmitterTrack:
    """Fused track representing a single RF emitter."""

    emitter_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    state: TrackState = TrackState.NEW

    # Primary frequency characteristics
    primary_frequency: float = 0.0      # Hz (most recent)
    frequency_history: List[float] = field(default_factory=list)
    frequency_variance_hz: float = 0.0

    # Power
    last_power_dbfs: Optional[float] = None
    power_history: List[float] = field(default_factory=list)

    # Timing
    first_seen_ns: int = field(default_factory=time.time_ns)
    last_seen_ns: int = field(default_factory=time.time_ns)
    observation_count: int = 0

    # Multi-node metadata
    detecting_nodes: List[str] = field(default_factory=list)
    most_trustworthy_node: Optional[str] = None
    node_agreement_score: float = 1.0

    # Confidence
    confidence: float = 0.1
    confidence_history: List[float] = field(default_factory=list)

    # Classification
    modulation: Optional[str] = None
    protocol: Optional[str] = None
    threat_level: str = "unknown"   # unknown / low / medium / high / critical

    # Anomaly flags
    anomaly_flags: List[str] = field(default_factory=list)

    # Location estimate. Populated by either:
    #   - TDOA solver (≥2 GPS-synced nodes hearing same emission): the
    #     real geolocation path. `location_method = "tdoa"`.
    #   - RSSI proximity estimator (single-node fallback, opt-in via
    #     RSSI_PROXIMITY_ENABLED): centres on the detecting node's GPS,
    #     uses free-space path-loss + assumed EIRP to estimate a range
    #     circle. `location_method = "rssi_proximity"`. The radius is
    #     wide and `location_confidence` is intentionally low (~0.15)
    #     because TX power is unknown and there is no bearing info.
    # `location_error_radius_m` is the rendered circle/ellipse radius
    # so the UI can show uncertainty without recomputing it.
    estimated_lat: Optional[float] = None
    estimated_lon: Optional[float] = None
    location_confidence: float = 0.0
    location_method: Optional[str] = None       # "tdoa" | "rssi_proximity" | None
    location_error_radius_m: Optional[float] = None
    # TDOA 1-sigma error ellipse (only set when location_method == "tdoa").
    tdoa_ellipse_a_m: Optional[float] = None    # semi-major axis (metres)
    tdoa_ellipse_b_m: Optional[float] = None    # semi-minor axis (metres)
    tdoa_ellipse_theta_deg: Optional[float] = None  # rotation, 0 = east-aligned

    # ── KrakenSDR LOB bearing fields ─────────────────────────────────────
    # Populated by TrackManager.ingest_lob() when one or more KrakenSDR
    # nodes provide direction-finding data for this emitter.
    #
    # `lob_bearing_deg` / `lob_bearing_uncert_deg`:
    #   Most recent bearing from the primary (highest-confidence) node.
    #   Displayed as the bearing wedge on the map when only one node is
    #   available (no crosscut possible yet).
    #
    # `lob_crosscut_*`:
    #   Populated by LOBTriangulator when ≥2 nodes produce intersecting
    #   bearing lines with a crossing angle ≥ MIN_CROSS_DEG.  When set,
    #   `estimated_lat` / `estimated_lon` / `location_error_radius_m` are
    #   also updated so the track appears on the main emitter dot layer at
    #   the crosscut point.  `location_method` is set to "lob_crosscut"
    #   or "lob_tdoa_hybrid" (when a TDOA fix and a LOB fix are blended).
    #
    # `lob_measurement_history`:
    #   Bounded list (max 20) of raw LOBMeasurement .to_dict() snapshots
    #   kept for re-triangulation when a new node joins mid-track.  Not
    #   serialised in to_dict() to keep the wire payload lean.

    lob_bearing_deg: Optional[float] = None
    lob_bearing_uncert_deg: Optional[float] = None
    lob_confidence: float = 0.0
    lob_node_ids: List[str] = field(default_factory=list)

    lob_crosscut_lat: Optional[float] = None
    lob_crosscut_lon: Optional[float] = None
    lob_crosscut_radius_m: Optional[float] = None
    lob_crosscut_confidence: float = 0.0

    # Internal history used by LOBTriangulator — NOT in to_dict() wire payload.
    lob_measurement_history: List[tuple] = field(default_factory=list)

    # Provenance: which cluster originated this track. None = local
    # fleet; otherwise the CoC peer URL (set on first ingest of a
    # remote-origin event). Used by CrossStationDedup to coalesce the
    # same physical emitter heard by both local + peer clusters.
    upstream_source: Optional[str] = None

    # Stationarity gate state. `location_history` is a bounded list of
    # accepted TDOA fixes (each entry is a 4-tuple
    # `(lat, lon, timestamp_ns, ellipse_a_m_or_None)`) that the
    # `StationarityGate` reads to compute `motion_state`. The list is
    # capped by `StationarityGate.history_max` (default 20). We store
    # tuples rather than HistoryPoint dataclasses so the field
    # serialises naturally via `to_dict()` without extra plumbing.
    # `motion_state ∈ {"unknown", "stationary", "mobile"}`. UNKNOWN
    # until the gate has seen >=2 fixes.
    location_history: List[tuple] = field(default_factory=list)
    motion_state: str = "unknown"

    def update(self, frequency: float, power_dbfs: float,
               node_id: str, trust_score: float, timestamp_ns: int):
        self.primary_frequency = frequency
        self.frequency_history.append(frequency)
        self.last_power_dbfs = power_dbfs
        self.power_history.append(power_dbfs)
        self.last_seen_ns = timestamp_ns
        self.observation_count += 1

        if node_id not in self.detecting_nodes:
            self.detecting_nodes.append(node_id)

        # Update most trustworthy node
        self.most_trustworthy_node = node_id if trust_score > 0.7 else self.most_trustworthy_node

        # Advance state machine
        self._advance_state()

        # Trim histories to last 100 samples
        if len(self.frequency_history) > 100:
            self.frequency_history = self.frequency_history[-100:]
        if len(self.power_history) > 100:
            self.power_history = self.power_history[-100:]

    def _advance_state(self):
        if self.state == TrackState.NEW and self.observation_count >= 3:
            self.state = TrackState.TRACKING
        elif self.state == TrackState.TRACKING:
            # STABLE promotion now factors in motion_state. Stationary
            # tracks promote at the original threshold (10 obs);
            # mobile tracks need more observations because their
            # position is by definition not converging, so the
            # "stable" label there means "we've been confidently
            # tracking the moving emitter for long enough" rather
            # than "the emitter has stopped moving". Unknown motion
            # state (no TDOA history yet) keeps the legacy threshold
            # so non-TDOA tracks aren't blocked from promotion.
            if self.motion_state == "mobile":
                threshold = 25
            else:
                threshold = 10
            if self.observation_count >= threshold:
                self.state = TrackState.STABLE

    def age_seconds(self) -> float:
        return (time.time_ns() - self.last_seen_ns) / 1e9

    def to_dict(self) -> dict:
        return {
            "emitter_id": self.emitter_id,
            "state": self.state.value,
            "primary_frequency": self.primary_frequency,
            "last_power_dbfs": self.last_power_dbfs,
            "first_seen_ns": self.first_seen_ns,
            "last_seen_ns": self.last_seen_ns,
            "observation_count": self.observation_count,
            "detecting_nodes": self.detecting_nodes,
            "confidence": self.confidence,
            "modulation": self.modulation,
            "protocol": self.protocol,
            "threat_level": self.threat_level,
            "anomaly_flags": self.anomaly_flags,
            "estimated_lat": self.estimated_lat,
            "estimated_lon": self.estimated_lon,
            "location_confidence": self.location_confidence,
            "location_method": self.location_method,
            "location_error_radius_m": self.location_error_radius_m,
            "tdoa_ellipse_a_m": self.tdoa_ellipse_a_m,
            "tdoa_ellipse_b_m": self.tdoa_ellipse_b_m,
            "tdoa_ellipse_theta_deg": self.tdoa_ellipse_theta_deg,
            "upstream_source": self.upstream_source,
            "motion_state": self.motion_state,
            # LOB bearing + crosscut fields (null when no KrakenSDR data)
            "lob_bearing_deg": self.lob_bearing_deg,
            "lob_bearing_uncert_deg": self.lob_bearing_uncert_deg,
            "lob_confidence": self.lob_confidence,
            "lob_node_ids": self.lob_node_ids,
            "lob_crosscut_lat": self.lob_crosscut_lat,
            "lob_crosscut_lon": self.lob_crosscut_lon,
            "lob_crosscut_radius_m": self.lob_crosscut_radius_m,
            "lob_crosscut_confidence": self.lob_crosscut_confidence,
            # node_lat / node_lon are kept for the map bearing wedge — they
            # tell the client where to anchor the LOB fan.  Taken from the
            # most recent measurement's node position.
            "lob_node_lat": getattr(self, "_lob_node_lat", None),
            "lob_node_lon": getattr(self, "_lob_node_lon", None),
        }
