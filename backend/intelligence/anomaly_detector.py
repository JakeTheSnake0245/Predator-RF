import logging
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from backend.models.emitter_track import EmitterTrack
from backend.models.rf_event import RFEvent
from backend.intelligence.rf_baseline import RFBaseline

logger = logging.getLogger(__name__)


@dataclass
class AnomalyFlag:
    method: str
    description: str
    severity: str          # 'low' / 'medium' / 'high' / 'critical'
    timestamp_ns: int = field(default_factory=time.time_ns)


class AnomalyDetector:
    """
    Six parallel anomaly detection methods applied to emitter tracks.

    Methods:
    1. New frequency      — frequency never seen in baseline
    2. Power anomaly      — power >3σ above baseline for this freq
    3. Time anomaly       — active outside normal operating hours
    4. Frequency hop      — unexpected rapid frequency change
    5. Single node only   — appears on only one node (possible noise)
    6. Burst pattern      — short burst characteristic (tactical radio)
    """

    def __init__(self, baseline: Optional[RFBaseline] = None):
        self._baseline = baseline or RFBaseline()
        self._freq_history: Dict[str, List[float]] = {}  # track_id → freq history

    def analyze(self, track: EmitterTrack,
                latest_event: RFEvent) -> List[AnomalyFlag]:
        """Run all 6 anomaly methods against a track. Returns list of flags."""
        flags: List[AnomalyFlag] = []

        # Feed event to baseline (learning)
        self._baseline.observe(latest_event)

        freq = latest_event.frequency

        # 1. New frequency
        flag = self._check_new_frequency(freq, track)
        if flag:
            flags.append(flag)

        # 2. Power anomaly
        flag = self._check_power_anomaly(freq, latest_event.power_dbfs, track)
        if flag:
            flags.append(flag)

        # 3. Time anomaly
        flag = self._check_time_anomaly(freq)
        if flag:
            flags.append(flag)

        # 4. Frequency hop
        flag = self._check_frequency_hop(track, latest_event)
        if flag:
            flags.append(flag)

        # 5. Single node only
        flag = self._check_single_node(track)
        if flag:
            flags.append(flag)

        # 6. Burst pattern
        flag = self._check_burst_pattern(track)
        if flag:
            flags.append(flag)

        # Store flags on track
        for f in flags:
            if f.description not in track.anomaly_flags:
                track.anomaly_flags.append(f.description)

        return flags

    # ── Detection methods ─────────────────────────────────────────────────────

    def _check_new_frequency(self, freq: float,
                              track: EmitterTrack) -> Optional[AnomalyFlag]:
        if not self._baseline.is_known_frequency(freq, min_observations=5):
            return AnomalyFlag(
                method='new_frequency',
                description=f"New frequency {freq/1e6:.4f} MHz (not in baseline)",
                severity='medium',
            )
        return None

    def _check_power_anomaly(self, freq: float, power: float,
                              track: EmitterTrack) -> Optional[AnomalyFlag]:
        if self._baseline.is_abnormal_power(freq, power, sigma_threshold=3.0):
            return AnomalyFlag(
                method='power_anomaly',
                description=f"Power anomaly at {freq/1e6:.4f} MHz ({power:.1f} dBFS)",
                severity='high',
            )
        return None

    def _check_time_anomaly(self, freq: float) -> Optional[AnomalyFlag]:
        if self._baseline.is_abnormal_time(freq):
            return AnomalyFlag(
                method='time_anomaly',
                description=f"Activity at unusual time for {freq/1e6:.4f} MHz",
                severity='medium',
            )
        return None

    def _check_frequency_hop(self, track: EmitterTrack,
                              event: RFEvent) -> Optional[AnomalyFlag]:
        tid = track.emitter_id
        hist = self._freq_history.setdefault(tid, [])
        hist.append(event.frequency)
        if len(hist) > 10:
            hist.pop(0)

        if len(hist) < 3:
            return None

        # Frequency hop: large jump between consecutive observations
        for i in range(1, len(hist)):
            delta = abs(hist[i] - hist[i - 1])
            if delta > 500_000:   # > 500 kHz hop
                return AnomalyFlag(
                    method='frequency_hop',
                    description=f"Frequency hop detected ({delta/1e3:.0f} kHz)",
                    severity='high',
                )
        return None

    def _check_single_node(self, track: EmitterTrack) -> Optional[AnomalyFlag]:
        if (len(track.detecting_nodes) == 1 and
                track.observation_count >= 10):
            return AnomalyFlag(
                method='single_node',
                description="Seen by only one node — possible local noise",
                severity='low',
            )
        return None

    def _check_burst_pattern(self, track: EmitterTrack) -> Optional[AnomalyFlag]:
        # Burst: many observations in short time then silence
        if track.observation_count < 5:
            return None
        total_span_s = max(1.0,
            (track.last_seen_ns - track.first_seen_ns) / 1e9)
        obs_rate = track.observation_count / total_span_s

        # > 10 obs/sec in first phase = burst
        if obs_rate > 10.0 and total_span_s < 10.0:
            return AnomalyFlag(
                method='burst_pattern',
                description=f"Burst pattern ({obs_rate:.1f} obs/s over {total_span_s:.1f}s)",
                severity='medium',
            )
        return None

    def set_baseline(self, baseline: RFBaseline):
        self._baseline = baseline
