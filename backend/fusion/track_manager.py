import asyncio
import logging
import math
import time
from typing import Dict, List, Optional, Callable

from backend.models.rf_event import RFEvent
from backend.models.emitter_track import EmitterTrack, TrackState
from backend.models.sensor_node import SensorNodeTrust
from backend.models.lob_measurement import LOBMeasurement
from backend.fusion.track_associator import HardwareAwareAssociator
from backend.fusion.confidence_engine import ConfidenceEngine
from backend.fusion.proximity_estimator import ProximityEstimator
from backend.fusion.lob_triangulator import LOBTriangulator
try:
    from backend.fusion.stationarity_gate import FixCandidate, HistoryPoint, StationarityGate as _StationarityGate
    _HAS_STATIONARITY_GATE = True
except ImportError:
    _HAS_STATIONARITY_GATE = False

logger = logging.getLogger(__name__)

# Confidence ceiling for an LOB+TDOA blend.  Above 0.85 implies sub-50 m
# accuracy which no current KrakenSDR+TDOA pipeline can reliably deliver.
MAX_LOB_TDOA_HYBRID_CONF = 0.85

# Track ages before state transition
COAST_AFTER_S = 30.0
LOSE_AFTER_S = 120.0
ARCHIVE_AFTER_S = 86_400.0   # 24 hours


class TrackManager:
    """
    Lifecycle manager for all emitter tracks.
    Ingests RFEvents, associates them, creates/updates/retires tracks.
    """

    # Maximum LOBMeasurement history entries kept per track.  Older
    # entries are trimmed on each ingest_lob() call.  20 measurements
    # is enough for multi-node re-triangulation without blowing memory
    # on long-running missions.
    LOB_HISTORY_MAX = 20

    def __init__(self,
                 proximity_estimator: Optional[ProximityEstimator] = None,
                 custody_elector=None,
                 lob_triangulator: Optional[LOBTriangulator] = None,
                 stationarity_gate=None):
        self.tracks: Dict[str, EmitterTrack] = {}
        self.sensor_nodes: Dict[str, SensorNodeTrust] = {}
        self._associator = HardwareAwareAssociator()
        self._confidence = ConfidenceEngine()
        # Optional single-node RSSI fallback. None = disabled (default
        # posture). When provided, every event whose track does NOT
        # already have a TDOA fix gets a coarse proximity estimate so
        # the map shows *something* instead of a missing dot.
        self._proximity = proximity_estimator
        # Optional CustodyElector — when provided, _age_tracks() calls
        # `forget(track_id)` at archive time so the elector's per-track
        # decision cache can't grow without bound across long missions.
        # Typed loosely (no import) to avoid pulling
        # backend.coordination into the fusion-layer imports — the
        # elector is duck-typed via its `forget` method.
        self._custody_elector = custody_elector
        # LOBTriangulator — stateless; shared across all tracks.
        # None = KrakenSDR not configured (ingest_lob is a no-op).
        self._lob_triangulator: LOBTriangulator = (
            lob_triangulator if lob_triangulator is not None else LOBTriangulator()
        )
        # Optional StationarityGate — when provided, LOB crosscut fixes are
        # sanity-checked before being promoted to the primary track location.
        # The gate is shared across all tracks (stateless scoring fn);
        # the per-track history list is stored on the track itself.
        # Typed loosely so that the fusion-layer import doesn't hard-fail
        # when stationarity_gate.py is unavailable.
        self._lob_stationarity_gate = None
        if stationarity_gate is not None:
            self._lob_stationarity_gate = stationarity_gate
        self._on_new_track: Optional[Callable[[EmitterTrack], None]] = None
        self._on_update: Optional[Callable[[EmitterTrack], None]] = None
        self._archived: Dict[str, EmitterTrack] = {}

    def register_node(self, node: SensorNodeTrust):
        self.sensor_nodes[node.node_id] = node

    def on_new_track(self, fn: Callable[[EmitterTrack], None]):
        self._on_new_track = fn

    def on_update(self, fn: Callable[[EmitterTrack], None]):
        self._on_update = fn

    # ── Event ingestion ───────────────────────────────────────────────────────

    def ingest(self, event: RFEvent) -> EmitterTrack:
        """Process one RFEvent → associate or create track. Returns affected track."""

        track_id, score = self._associator.associate(
            event, self.tracks, self.sensor_nodes)

        upstream = getattr(event, "upstream_source", None)
        if track_id:
            track = self.tracks[track_id]
            track.update(event.frequency, event.power_dbfs,
                         event.node_id, event.node_trust_score,
                         event.timestamp_ns)
            # Once a track has been seen locally (upstream_source=None),
            # it stays local; we only stamp upstream_source if the track
            # has *only* ever been seen via that single peer cluster.
            if track.upstream_source is not None and track.upstream_source != upstream:
                track.upstream_source = None  # heard from >1 origin → local-equivalent
        else:
            track = EmitterTrack(
                primary_frequency=event.frequency,
                last_power_dbfs=event.power_dbfs,
                first_seen_ns=event.timestamp_ns,
                last_seen_ns=event.timestamp_ns,
                observation_count=1,
                upstream_source=upstream,
            )
            track.detecting_nodes = [event.node_id]
            track.frequency_history = [event.frequency]
            track.power_history = [event.power_dbfs]
            self.tracks[track.emitter_id] = track
            self._associator.index_track(track)
            if self._on_new_track:
                self._on_new_track(track)

        # Update confidence
        node = self.sensor_nodes.get(event.node_id)
        self._confidence.update(track, event, node)

        if node:
            node.total_observations += 1

        # Single-node RSSI proximity fallback. Only runs when:
        #   - estimator is configured (RSSI_PROXIMITY_ENABLED=true), AND
        #   - track does NOT already have a TDOA fix (TDOA always wins
        #     once it produces one — proximity never overwrites a real
        #     geolocation). Proximity-derived locations may be refreshed
        #     by later proximity estimates so the radius shrinks as the
        #     operator walks closer to a strong source.
        if self._proximity is not None and (
                track.location_method is None or
                track.location_method == "rssi_proximity"):
            node_lat = event.node_lat if event.node_lat is not None else (
                node.location_gps[0] if node and node.location_gps else None)
            node_lon = event.node_lon if event.node_lon is not None else (
                node.location_gps[1] if node and node.location_gps else None)
            fix = self._proximity.estimate(
                power_dbfs=event.power_dbfs,
                frequency_hz=event.frequency,
                node_lat=node_lat,
                node_lon=node_lon,
                node_id=event.node_id,
                timestamp_ns=event.timestamp_ns)
            if fix is not None:
                track.estimated_lat = fix.estimated_lat
                track.estimated_lon = fix.estimated_lon
                track.location_confidence = fix.location_confidence
                track.location_method = fix.method
                track.location_error_radius_m = fix.error_radius_m

        if self._on_update:
            self._on_update(track)

        return track

    # ── LOB ingestion ─────────────────────────────────────────────────────────

    def ingest_lob(self, measurement: LOBMeasurement) -> Optional[EmitterTrack]:
        """
        Ingest one KrakenSDR LOB measurement.

        Associates the measurement with an existing track by frequency, or
        creates a new one.  Updates the track's LOB bearing fields and, when
        ≥2 nodes have contributed measurements with a sufficient crossing
        angle, updates the crosscut geolocation.

        Returns the affected track, or None if the measurement is invalid.
        """
        if not measurement.frequency_hz or not measurement.node_id:
            return None

        # Find the best frequency-match track.
        candidates = self.tracks_near_frequency(
            measurement.frequency_hz, tolerance_hz=25_000.0)

        if candidates:
            # Pick the most recently updated candidate.
            track = max(candidates, key=lambda t: t.last_seen_ns)
        else:
            # No existing track — create one with minimal info.
            track = EmitterTrack(
                primary_frequency=measurement.frequency_hz,
                first_seen_ns=measurement.timestamp_ns,
                last_seen_ns=measurement.timestamp_ns,
                observation_count=1,
            )
            if measurement.node_id not in track.detecting_nodes:
                track.detecting_nodes.append(measurement.node_id)
            self.tracks[track.emitter_id] = track
            self._associator.index_track(track)
            if self._on_new_track:
                self._on_new_track(track)

        # Update the most-recent-bearing fields using this measurement.
        if (track.lob_bearing_deg is None or
                measurement.confidence >= track.lob_confidence):
            track.lob_bearing_deg = measurement.bearing_deg
            track.lob_bearing_uncert_deg = measurement.bearing_uncert_deg
            track.lob_confidence = measurement.confidence
            # Cache node position for map wedge anchor.
            track._lob_node_lat = measurement.node_lat
            track._lob_node_lon = measurement.node_lon

        if measurement.node_id not in track.lob_node_ids:
            track.lob_node_ids.append(measurement.node_id)
        if measurement.node_id not in track.detecting_nodes:
            track.detecting_nodes.append(measurement.node_id)

        # Append to bounded per-track history.
        track.lob_measurement_history.append(measurement)
        if len(track.lob_measurement_history) > self.LOB_HISTORY_MAX:
            track.lob_measurement_history = (
                track.lob_measurement_history[-self.LOB_HISTORY_MAX:])

        track.last_seen_ns = measurement.timestamp_ns

        # Attempt triangulation using only recent measurements (TTL filtering
        # is now handled inside LOBTriangulator.triangulate()).
        fix = self._lob_triangulator.triangulate(track.lob_measurement_history)
        if fix is not None:
            track.lob_crosscut_lat       = fix.estimated_lat
            track.lob_crosscut_lon       = fix.estimated_lon
            track.lob_crosscut_radius_m  = fix.error_radius_m
            track.lob_crosscut_confidence = fix.location_confidence

            # ── Stationarity gate for LOB crosscut fix ────────────────────
            # When a gate is configured, sanity-check the LOB crosscut against
            # the track's accepted location history before promoting it.
            gate_accepted = True
            if self._lob_stationarity_gate is not None and _HAS_STATIONARITY_GATE:
                try:
                    candidate = FixCandidate(
                        lat=fix.estimated_lat,
                        lon=fix.estimated_lon,
                        ellipse_a_m=fix.error_radius_m,
                        timestamp_ns=measurement.timestamp_ns,
                    )
                    verdict = self._lob_stationarity_gate.evaluate(
                        candidate,
                        getattr(track, "location_history", []),
                    )
                    if not verdict.accepted:
                        logger.info(
                            "Track %s LOB crosscut rejected by stationarity gate: %s",
                            track.emitter_id, verdict.reason)
                        gate_accepted = False
                    else:
                        # Append accepted fix to the track's location history.
                        hp = HistoryPoint(
                            lat=fix.estimated_lat,
                            lon=fix.estimated_lon,
                            timestamp_ns=measurement.timestamp_ns,
                            ellipse_a_m=fix.error_radius_m,
                        )
                        if not hasattr(track, "location_history"):
                            track.location_history = []
                        track.location_history.append(hp)
                        history_max = getattr(
                            self._lob_stationarity_gate, "history_max", 20)
                        if len(track.location_history) > history_max:
                            track.location_history = (
                                track.location_history[-history_max:])
                except Exception as exc:
                    logger.debug("LOB stationarity gate error — fix accepted: %s", exc)

            if gate_accepted:
                # ── LOB+TDOA hybrid merge ─────────────────────────────────
                # When a TDOA fix already exists, blend LOB crosscut and TDOA
                # using inverse-variance weighting (w = 1/r²).  This raises
                # effective position accuracy when both sources agree, and
                # downweights either source in proportion to its error radius.
                if (track.location_method == "tdoa" and
                        track.estimated_lat is not None and
                        track.estimated_lon is not None and
                        track.location_error_radius_m and
                        track.location_error_radius_m > 0):
                    tdoa_r = track.location_error_radius_m
                    lob_r  = fix.error_radius_m
                    w_tdoa = 1.0 / (tdoa_r ** 2)
                    w_lob  = 1.0 / (lob_r ** 2)
                    w_sum  = w_tdoa + w_lob
                    blend_lat  = (w_tdoa * track.estimated_lat  + w_lob * fix.estimated_lat)  / w_sum
                    blend_lon  = (w_tdoa * track.estimated_lon  + w_lob * fix.estimated_lon)  / w_sum
                    blend_r    = math.sqrt(1.0 / w_sum)
                    blend_conf = min(
                        MAX_LOB_TDOA_HYBRID_CONF,
                        (w_tdoa * track.location_confidence + w_lob * fix.location_confidence)
                        / w_sum,
                    )
                    track.estimated_lat           = round(blend_lat, 7)
                    track.estimated_lon           = round(blend_lon, 7)
                    track.location_confidence     = round(blend_conf, 3)
                    track.location_error_radius_m = round(blend_r, 1)
                    track.location_method         = "lob_tdoa_hybrid"
                    logger.debug(
                        "Track %s LOB+TDOA blend: r_lob=%.0fm r_tdoa=%.0fm → r_blend=%.0fm",
                        track.emitter_id, lob_r, tdoa_r, blend_r)

                # ── Promote LOB crosscut to primary location ──────────────
                # Only when the current primary fix is weaker:
                #   a) No fix yet (location_method is None)
                #   b) Current fix is an RSSI proximity estimate (weakest)
                #   c) Current fix is also an LOB crosscut AND the new one
                #      has higher confidence (improvement only — never
                #      overwrite a better LOB with a worse one)
                elif (track.location_method is None or
                        track.location_method == "rssi_proximity" or
                        (track.location_method == "lob_crosscut" and
                         fix.location_confidence > (track.location_confidence or 0.0))):
                    track.estimated_lat           = fix.estimated_lat
                    track.estimated_lon           = fix.estimated_lon
                    track.location_confidence     = fix.location_confidence
                    track.location_method         = fix.location_method
                    track.location_error_radius_m = fix.error_radius_m

        # ── Custody election hook ─────────────────────────────────────────
        # Notify the custody elector that this track has been updated by a
        # LOB measurement so it can re-score sensor assignment if needed.
        # The elector is duck-typed — no direct import dependency.
        if self._custody_elector is not None:
            try:
                assess_fn = getattr(self._custody_elector, "assess", None)
                if callable(assess_fn):
                    assess_fn(track, list(self.sensor_nodes.values()))
            except Exception as exc:
                logger.debug("LOB custody assess error — ignored: %s", exc)

        if self._on_update:
            self._on_update(track)

        return track

    # ── Lifecycle maintenance ─────────────────────────────────────────────────

    async def maintenance_loop(self, interval_s: float = 10.0):
        """Periodic task: coast and retire stale tracks."""
        while True:
            await asyncio.sleep(interval_s)
            self._age_tracks()

    def _age_tracks(self):
        now_ns = time.time_ns()
        to_archive = []

        for tid, track in list(self.tracks.items()):
            age_s = (now_ns - track.last_seen_ns) / 1e9

            if age_s > ARCHIVE_AFTER_S:
                to_archive.append(tid)
            elif age_s > LOSE_AFTER_S and track.state != TrackState.LOST:
                track.state = TrackState.LOST
                logger.debug("Track LOST: %s (%.0f s ago)", tid, age_s)
            elif age_s > COAST_AFTER_S and track.state == TrackState.STABLE:
                track.state = TrackState.COASTING

        for tid in to_archive:
            track = self.tracks.pop(tid)
            self._associator.remove_track(track)
            self._archived[tid] = track
            # Drop the CustodyElector's cached decision for this track
            # so the per-track cache doesn't grow with archived ids.
            # `forget` is idempotent — safe to call even if the elector
            # never elected for this track.
            if self._custody_elector is not None:
                try:
                    self._custody_elector.forget(tid)
                except Exception:
                    logger.exception(
                        "custody_elector.forget(%s) raised — ignored", tid)
            # LOBTriangulator.forget() is a no-op (stateless) but called
            # for API parity so callers don't need to know the distinction.
            try:
                self._lob_triangulator.forget(tid)
            except Exception:
                pass
            logger.debug("Track ARCHIVED: %s", tid)

    # ── Merge duplicate tracks ────────────────────────────────────────────────

    def merge_duplicates(self, freq_tolerance_hz: float = 5000.0,
                          time_window_s: float = 10.0):
        """Merge tracks that are too similar (frequency + time overlap)."""
        ids = list(self.tracks.keys())
        merged: set = set()

        for i, id1 in enumerate(ids):
            if id1 in merged:
                continue
            t1 = self.tracks[id1]
            for id2 in ids[i + 1:]:
                if id2 in merged:
                    continue
                t2 = self.tracks[id2]
                freq_diff = abs(t1.primary_frequency - t2.primary_frequency)
                time_diff_s = abs(t1.last_seen_ns - t2.last_seen_ns) / 1e9
                if freq_diff < freq_tolerance_hz and time_diff_s < time_window_s:
                    # Merge t2 into t1 (keep the older one)
                    primary = t1 if t1.first_seen_ns <= t2.first_seen_ns else t2
                    secondary = t2 if primary is t1 else t1
                    secondary_id = secondary.emitter_id

                    primary.detecting_nodes = list(
                        set(primary.detecting_nodes + secondary.detecting_nodes))
                    primary.observation_count += secondary.observation_count
                    primary.frequency_history.extend(secondary.frequency_history)

                    self.tracks.pop(secondary_id, None)
                    self._associator.remove_track(secondary)
                    merged.add(secondary_id)
                    logger.debug("Merged track %s into %s", secondary_id, primary.emitter_id)

    # ── Queries ───────────────────────────────────────────────────────────────

    def active_tracks(self) -> List[EmitterTrack]:
        return [t for t in self.tracks.values()
                if t.state not in (TrackState.LOST, TrackState.COASTING)]

    def high_confidence_tracks(self, min_confidence: float = 0.6) -> List[EmitterTrack]:
        return [t for t in self.active_tracks() if t.confidence >= min_confidence]

    def tracks_near_frequency(self, freq_hz: float,
                               tolerance_hz: float = 10_000.0) -> List[EmitterTrack]:
        return [t for t in self.tracks.values()
                if abs(t.primary_frequency - freq_hz) <= tolerance_hz]
