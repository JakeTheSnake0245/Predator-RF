import logging
import time
from typing import Dict, List, Optional, Tuple

from backend.models.rf_event import RFEvent
from backend.models.emitter_track import EmitterTrack, TrackState
from backend.models.sensor_node import SensorNodeTrust

logger = logging.getLogger(__name__)

# Hash bucket size for frequency-indexed lookup (100 kHz)
_BUCKET_HZ = 100_000


class HardwareAwareAssociator:
    """
    Associate incoming RFEvents with existing EmitterTracks,
    accounting for hardware frequency accuracy and sensitivity differences.
    """

    DEFAULT_FREQ_TOLERANCE_HZ = 5_000
    TIME_WINDOW_S = 60.0
    POWER_VARIATION_DB = 20.0
    MATCH_THRESHOLD = 0.4

    def __init__(self):
        # freq_bucket → list of track IDs (O(1) candidate lookup)
        self._freq_index: Dict[int, List[str]] = {}

    def associate(self, event: RFEvent,
                  tracks: Dict[str, EmitterTrack],
                  sensor_nodes: Dict[str, SensorNodeTrust]) -> Tuple[Optional[str], float]:
        """
        Return (track_id, score) for best matching track, or (None, 0) to create new.
        """
        candidates = self._candidate_tracks(event.frequency, tracks)
        if not candidates:
            return None, 0.0

        detecting_node = sensor_nodes.get(event.node_id)
        scored = [
            (tid, self._score(event, tracks[tid], detecting_node, sensor_nodes))
            for tid in candidates
        ]
        scored.sort(key=lambda x: x[1], reverse=True)

        best_id, best_score = scored[0]
        if best_score < self.MATCH_THRESHOLD:
            return None, 0.0
        return best_id, best_score

    # ── Candidate generation ──────────────────────────────────────────────────

    def _candidate_tracks(self, freq_hz: float,
                          tracks: Dict[str, EmitterTrack]) -> List[str]:
        bucket = int(freq_hz // _BUCKET_HZ)
        candidates = []
        for b in (bucket - 1, bucket, bucket + 1):
            for tid in self._freq_index.get(b, []):
                if tid in tracks:
                    candidates.append(tid)
        return list(set(candidates))

    # ── Scoring ───────────────────────────────────────────────────────────────

    def _score(self, event: RFEvent, track: EmitterTrack,
               detecting_node: Optional[SensorNodeTrust],
               all_nodes: Dict[str, SensorNodeTrust]) -> float:
        score = 0.0

        # Frequency match (0.30)
        tol = self._freq_tolerance(detecting_node)
        delta_f = abs(track.primary_frequency - event.frequency)
        score += max(0.0, 1.0 - delta_f / tol) * 0.30

        # Time match (0.20)
        age_s = (event.timestamp_ns - track.last_seen_ns) / 1e9
        score += max(0.0, 1.0 - age_s / self.TIME_WINDOW_S) * 0.20

        # Power match (0.20)
        if track.last_power_dbfs is not None and detecting_node:
            sens_diff = self._sensitivity_diff(detecting_node,
                                               track.most_trustworthy_node, all_nodes)
            delta_p = abs(event.power_dbfs - track.last_power_dbfs)
            score += max(0.0, 1.0 - delta_p / (self.POWER_VARIATION_DB + sens_diff)) * 0.20
        else:
            score += 0.20

        # Track state bonus (0.15)
        state_bonus = {TrackState.NEW: 0.0, TrackState.TRACKING: 0.10, TrackState.STABLE: 0.15}
        score += state_bonus.get(track.state, 0.0)

        # Multi-hardware corroboration bonus (0.15)
        if (track.most_trustworthy_node and detecting_node and
                detecting_node.node_id != track.most_trustworthy_node):
            score += 0.15

        return min(score, 1.0)

    def _freq_tolerance(self, node: Optional[SensorNodeTrust]) -> float:
        if not node or not node.hardware_capabilities:
            return self.DEFAULT_FREQ_TOLERANCE_HZ
        ppm = node.hardware_capabilities.freq_accuracy_ppm
        tol = (ppm / 1e6) * node.max_sample_rate_hz
        return max(tol, 1000.0)

    def _sensitivity_diff(self, node1: SensorNodeTrust,
                          node2_id: Optional[str],
                          all_nodes: Dict[str, SensorNodeTrust]) -> float:
        if not node2_id or node2_id not in all_nodes:
            return 0.0
        node2 = all_nodes[node2_id]
        if not node1.hardware_capabilities or not node2.hardware_capabilities:
            return 0.0
        nf_diff = abs(node1.hardware_capabilities.noise_figure_db -
                      node2.hardware_capabilities.noise_figure_db)
        return nf_diff * 2.0

    # ── Index maintenance ─────────────────────────────────────────────────────

    def index_track(self, track: EmitterTrack):
        bucket = int(track.primary_frequency // _BUCKET_HZ)
        self._freq_index.setdefault(bucket, [])
        if track.emitter_id not in self._freq_index[bucket]:
            self._freq_index[bucket].append(track.emitter_id)

    def remove_track(self, track: EmitterTrack):
        bucket = int(track.primary_frequency // _BUCKET_HZ)
        bucket_list = self._freq_index.get(bucket, [])
        if track.emitter_id in bucket_list:
            bucket_list.remove(track.emitter_id)
