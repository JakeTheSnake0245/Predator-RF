import logging
import time
import statistics
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from backend.models.rf_event import RFEvent
from backend.models.emitter_track import EmitterTrack

logger = logging.getLogger(__name__)

# Bucket size for grouping frequencies into "bands"
_FREQ_BUCKET_HZ = 500_000   # 500 kHz buckets


@dataclass
class FrequencyProfile:
    """Learned behavioral profile for a frequency bucket."""
    freq_bucket_hz: int
    observation_count: int = 0
    power_samples: List[float] = field(default_factory=list)
    active_hours: List[int] = field(default_factory=list)   # 0–23
    last_seen_ns: int = 0
    first_seen_ns: int = field(default_factory=time.time_ns)

    @property
    def mean_power(self) -> Optional[float]:
        return statistics.mean(self.power_samples) if self.power_samples else None

    @property
    def stdev_power(self) -> Optional[float]:
        return statistics.stdev(self.power_samples) if len(self.power_samples) >= 2 else None

    def expected_hours(self) -> List[int]:
        """Hours of day where activity is "normal" (seen ≥10% of observations)."""
        if not self.active_hours:
            return list(range(24))
        from collections import Counter
        counts = Counter(self.active_hours)
        threshold = self.observation_count * 0.10
        return [h for h, c in counts.items() if c >= threshold]


class RFBaseline:
    """
    Learn and store the normal RF environment.
    Provides anomaly context: what's new vs. what's always there.
    """

    def __init__(self, learning_window_hours: float = 24.0):
        self._learning_window_ns = int(learning_window_hours * 3600 * 1e9)
        self._profiles: Dict[int, FrequencyProfile] = defaultdict(
            lambda: FrequencyProfile(freq_bucket_hz=0))
        self._total_observations = 0

    def observe(self, event: RFEvent):
        """Feed an event into the baseline model."""
        bucket = int(event.frequency // _FREQ_BUCKET_HZ) * _FREQ_BUCKET_HZ
        profile = self._profiles[bucket]
        profile.freq_bucket_hz = bucket
        profile.observation_count += 1
        profile.power_samples.append(event.power_dbfs)
        profile.last_seen_ns = event.timestamp_ns

        hour = time.localtime(event.timestamp_ns // 1_000_000_000).tm_hour
        profile.active_hours.append(hour)

        self._total_observations += 1

        # Trim samples to last 500 to prevent unbounded growth
        if len(profile.power_samples) > 500:
            profile.power_samples = profile.power_samples[-500:]
        if len(profile.active_hours) > 500:
            profile.active_hours = profile.active_hours[-500:]

    def is_known_frequency(self, freq_hz: float,
                            min_observations: int = 5) -> bool:
        bucket = int(freq_hz // _FREQ_BUCKET_HZ) * _FREQ_BUCKET_HZ
        p = self._profiles.get(bucket)
        return p is not None and p.observation_count >= min_observations

    def is_abnormal_power(self, freq_hz: float, power_dbfs: float,
                           sigma_threshold: float = 3.0) -> bool:
        """True if power is >N sigma above baseline for this frequency."""
        bucket = int(freq_hz // _FREQ_BUCKET_HZ) * _FREQ_BUCKET_HZ
        p = self._profiles.get(bucket)
        if not p or p.stdev_power is None or p.mean_power is None:
            return False
        return power_dbfs > p.mean_power + sigma_threshold * p.stdev_power

    def is_abnormal_time(self, freq_hz: float) -> bool:
        """True if current hour is outside expected activity window."""
        bucket = int(freq_hz // _FREQ_BUCKET_HZ) * _FREQ_BUCKET_HZ
        p = self._profiles.get(bucket)
        if not p or p.observation_count < 10:
            return False
        current_hour = time.localtime().tm_hour
        return current_hour not in p.expected_hours()

    def frequency_profile(self, freq_hz: float) -> Optional[FrequencyProfile]:
        bucket = int(freq_hz // _FREQ_BUCKET_HZ) * _FREQ_BUCKET_HZ
        return self._profiles.get(bucket)

    def prune_stale(self, max_age_hours: float = 72.0):
        """Remove profiles not seen recently."""
        cutoff = time.time_ns() - int(max_age_hours * 3600 * 1e9)
        stale = [b for b, p in self._profiles.items() if p.last_seen_ns < cutoff]
        for b in stale:
            del self._profiles[b]
        if stale:
            logger.debug("Pruned %d stale frequency profiles", len(stale))

    def stats(self) -> dict:
        return {
            'total_observations': self._total_observations,
            'unique_frequency_buckets': len(self._profiles),
        }
