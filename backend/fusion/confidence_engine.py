import logging
import time
from typing import Dict, Optional

from backend.models.emitter_track import EmitterTrack
from backend.models.rf_event import RFEvent
from backend.models.sensor_node import SensorNodeTrust

logger = logging.getLogger(__name__)


class ConfidenceEngine:
    """
    Multi-factor Bayesian confidence scoring for emitter tracks.

    Confidence = f(observation_count, node_trust, multi_node_agreement,
                   temporal_consistency, frequency_stability, hardware_quality)
    """

    # Weight factors (must sum to 1.0)
    W_OBSERVATION = 0.25
    W_NODE_TRUST  = 0.20
    W_MULTI_NODE  = 0.20
    W_TEMPORAL    = 0.15
    W_FREQ_STAB   = 0.10
    W_HW_QUALITY  = 0.10

    def update(self, track: EmitterTrack, event: RFEvent,
               node: Optional[SensorNodeTrust] = None) -> float:
        """Recompute and store track confidence. Returns new confidence."""

        # 1. Observation saturation (logarithmic growth)
        obs_score = min(1.0, track.observation_count / 20.0)
        # Floor at 0.05 for new tracks
        obs_score = max(0.05, obs_score)

        # 2. Node trust
        trust = event.node_trust_score
        if node:
            trust = node.compute_trust_score()

        # 3. Multi-node agreement
        multi = self._multi_node_score(track)

        # 4. Temporal consistency
        temporal = self._temporal_score(track)

        # 5. Frequency stability
        freq_stab = self._frequency_stability_score(track)

        # 6. Hardware quality
        hw_quality = self._hardware_quality_score(node)

        confidence = (
            self.W_OBSERVATION * obs_score +
            self.W_NODE_TRUST  * trust +
            self.W_MULTI_NODE  * multi +
            self.W_TEMPORAL    * temporal +
            self.W_FREQ_STAB   * freq_stab +
            self.W_HW_QUALITY  * hw_quality
        )

        confidence = max(0.01, min(0.99, confidence))
        track.confidence = confidence
        track.confidence_history.append(confidence)

        return confidence

    def _multi_node_score(self, track: EmitterTrack) -> float:
        """More unique nodes → higher confidence."""
        n = len(set(track.detecting_nodes))
        if n == 0:
            return 0.0
        if n == 1:
            return 0.4
        if n == 2:
            return 0.7
        return min(1.0, 0.7 + (n - 2) * 0.1)

    def _temporal_score(self, track: EmitterTrack) -> float:
        """Consistent timing increases confidence."""
        if track.observation_count < 2:
            return 0.3
        age_s = (time.time_ns() - track.first_seen_ns) / 1e9
        # Seen for >60 s steadily → high temporal confidence
        return min(1.0, age_s / 60.0)

    def _frequency_stability_score(self, track: EmitterTrack) -> float:
        """Low frequency variance → higher confidence."""
        if len(track.frequency_history) < 3:
            return 0.5
        import statistics
        try:
            stdev = statistics.stdev(track.frequency_history)
        except Exception:
            return 0.5
        # stdev < 500 Hz → 1.0; stdev > 10 kHz → 0.2
        return max(0.2, min(1.0, 1.0 - (stdev / 10_000.0)))

    def _hardware_quality_score(self, node: Optional[SensorNodeTrust]) -> float:
        if not node or not node.hardware_capabilities:
            return 0.5
        caps = node.hardware_capabilities
        # Use noise figure as proxy for quality
        # 2.5 dB (Airspy) → 1.0; 10 dB (HackRF) → 0.5
        score = 1.0 - (caps.noise_figure_db - 2.5) / 15.0
        return max(0.3, min(1.0, score))

    def apply_anomaly_penalty(self, track: EmitterTrack, penalty: float = 0.1):
        """Reduce confidence when anomalies are flagged."""
        track.confidence = max(0.01, track.confidence - penalty)
