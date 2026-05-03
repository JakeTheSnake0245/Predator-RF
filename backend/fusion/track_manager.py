import asyncio
import logging
import time
from typing import Dict, List, Optional, Callable

from backend.models.rf_event import RFEvent
from backend.models.emitter_track import EmitterTrack, TrackState
from backend.models.sensor_node import SensorNodeTrust
from backend.fusion.track_associator import HardwareAwareAssociator
from backend.fusion.confidence_engine import ConfidenceEngine
from backend.fusion.proximity_estimator import ProximityEstimator

logger = logging.getLogger(__name__)

# Track ages before state transition
COAST_AFTER_S = 30.0
LOSE_AFTER_S = 120.0
ARCHIVE_AFTER_S = 86_400.0   # 24 hours


class TrackManager:
    """
    Lifecycle manager for all emitter tracks.
    Ingests RFEvents, associates them, creates/updates/retires tracks.
    """

    def __init__(self,
                 proximity_estimator: Optional[ProximityEstimator] = None):
        self.tracks: Dict[str, EmitterTrack] = {}
        self.sensor_nodes: Dict[str, SensorNodeTrust] = {}
        self._associator = HardwareAwareAssociator()
        self._confidence = ConfidenceEngine()
        # Optional single-node RSSI fallback. None = disabled (default
        # posture). When provided, every event whose track does NOT
        # already have a TDOA fix gets a coarse proximity estimate so
        # the map shows *something* instead of a missing dot.
        self._proximity = proximity_estimator
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
