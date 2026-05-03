"""
Cross-station emitter deduplication for CoC mode.

Two field stations (Alpha, Bravo) hearing the same physical emitter
otherwise produce two tracks with two different emitter_ids — one per
station — because TrackAssociator keys on (frequency, modulation,
detecting_node_set) and the node sets disagree.

CrossStationDedup runs as a periodic maintenance pass over the
TrackManager. For every pair of tracks where:
  - |freq_a - freq_b| <= freq_tolerance_hz, AND
  - both have estimated_lat/lon AND distance <= location_tolerance_m,
    OR (only one has a location AND modulation/protocol match)
  - last_seen times overlap inside corr_window_s
…it merges the younger track into the older one, unioning
detecting_nodes / participating_nodes and preserving upstream_source.

Conservative-by-default: only fires when BOTH tracks are flagged from
distinct origins (one local + one CoC, or two distinct CoC sources).
Two locally-tracked emitters never get merged here — that's
TrackAssociator's job, and a false-merge there would be silent.
"""
from __future__ import annotations

import logging
import math
import time
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)


def _haversine_m(a_lat: float, a_lon: float,
                 b_lat: float, b_lon: float) -> float:
    R = 6_371_000.0
    p1, p2 = math.radians(a_lat), math.radians(b_lat)
    dp = math.radians(b_lat - a_lat)
    dl = math.radians(b_lon - a_lon)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * R * math.asin(math.sqrt(h))


class CrossStationDedup:
    def __init__(self,
                 freq_tolerance_hz: float = 5_000.0,
                 location_tolerance_m: float = 500.0,
                 corr_window_s: float = 30.0):
        self.freq_tolerance_hz = freq_tolerance_hz
        self.location_tolerance_m = location_tolerance_m
        self.corr_window_s = corr_window_s
        self._merges_total = 0

    @property
    def merges_total(self) -> int:
        return self._merges_total

    def _is_cross_station(self, t_a, t_b) -> bool:
        """True iff at least one of the pair came from a different
        origin than the other (local vs upstream, or two distinct
        upstreams). Never merges two purely-local tracks."""
        a = getattr(t_a, "upstream_source", None) or "_local"
        b = getattr(t_b, "upstream_source", None) or "_local"
        return a != b

    def _is_similar(self, t_a, t_b, now_s: float) -> bool:
        # Frequency
        if abs(t_a.primary_frequency - t_b.primary_frequency) > self.freq_tolerance_hz:
            return False
        # Time co-occurrence
        last_a = (t_a.last_seen_ns or 0) / 1e9
        last_b = (t_b.last_seen_ns or 0) / 1e9
        if abs(last_a - last_b) > self.corr_window_s and \
           min(now_s - last_a, now_s - last_b) > self.corr_window_s:
            return False
        # Location: if both have it, compare; else fall back to mod/proto
        a_loc = (t_a.estimated_lat is not None
                 and t_a.estimated_lon is not None)
        b_loc = (t_b.estimated_lat is not None
                 and t_b.estimated_lon is not None)
        if a_loc and b_loc:
            d = _haversine_m(t_a.estimated_lat, t_a.estimated_lon,
                             t_b.estimated_lat, t_b.estimated_lon)
            return d <= self.location_tolerance_m
        # No location on at least one side — require modulation match
        if t_a.modulation and t_b.modulation:
            return t_a.modulation == t_b.modulation
        if t_a.protocol and t_b.protocol:
            return t_a.protocol == t_b.protocol
        # Conservative: don't merge if we can't corroborate
        return False

    def _merge(self, keep, drop) -> None:
        # Union node lists
        for n in drop.detecting_nodes:
            if n not in keep.detecting_nodes:
                keep.detecting_nodes.append(n)
        # Promote the higher confidence
        if drop.confidence > keep.confidence:
            keep.confidence = drop.confidence
        # Promote a TDOA fix if we don't have one
        if (keep.estimated_lat is None
                and drop.estimated_lat is not None):
            keep.estimated_lat = drop.estimated_lat
            keep.estimated_lon = drop.estimated_lon
            keep.location_confidence = drop.location_confidence
        # Last-seen → newer of the two
        if drop.last_seen_ns > keep.last_seen_ns:
            keep.last_seen_ns = drop.last_seen_ns
        keep.observation_count += drop.observation_count

    def run(self, track_manager) -> int:
        """Single pass over all tracks. Returns the count of merges
        performed in this pass."""
        merges = 0
        now_s = time.time()
        tracks = list(track_manager.tracks.values())
        # Sort by first_seen so the older one wins
        tracks.sort(key=lambda t: t.first_seen_ns)
        i = 0
        while i < len(tracks):
            j = i + 1
            while j < len(tracks):
                a, b = tracks[i], tracks[j]
                if (self._is_cross_station(a, b)
                        and self._is_similar(a, b, now_s)):
                    self._merge(a, b)
                    track_manager.tracks.pop(b.emitter_id, None)
                    tracks.pop(j)
                    merges += 1
                    continue  # don't advance j; check next against new tail
                j += 1
            i += 1
        if merges:
            self._merges_total += merges
            logger.info("Cross-station dedup merged %d pair(s); "
                        "total since startup: %d",
                        merges, self._merges_total)
        return merges
