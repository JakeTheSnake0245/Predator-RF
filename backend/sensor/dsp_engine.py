import logging
import time
from typing import List, Optional
import numpy as np

from backend.models.rf_event import RFEvent
from backend.models.sensor_node import SensorNodeTrust
from backend.sensor.modes import ModeConfig

logger = logging.getLogger(__name__)


class HardwareAdaptiveDSP:
    """DSP pipeline that adapts processing parameters to hardware capabilities."""

    def __init__(self, node: SensorNodeTrust, config: ModeConfig):
        self.node = node
        self.config = config
        self.capabilities = node.hardware_capabilities

        self.fft_size = self._select_fft_size()
        self.snr_threshold = self._adjust_snr_threshold()
        self.freq_resolution = self._compute_freq_resolution()
        self.dedup_window_ns = self._compute_dedup_window()

        self._seen_freqs: dict = {}  # freq_bucket → last_seen_ns (dedup)

    # ── Parameter selection ───────────────────────────────────────────────────

    def _select_fft_size(self) -> int:
        requested = self.config.fft_size
        if not self.capabilities:
            return min(requested, 4096)
        if self.capabilities.hardware_code == 'rtlsdr':
            return min(requested, 8192)
        if self.capabilities.hardware_code in ('limesdr', 'bladerf'):
            return requested
        return min(requested, 16384)

    def _adjust_snr_threshold(self) -> float:
        base = self.config.snr_threshold_db
        if not self.capabilities:
            return base
        nf = self.capabilities.noise_figure_db
        # 3 dB NF → +5 dB (sensitive), 10 dB NF → -3 dB (lenient)
        adjustment = (3.0 - nf) * 0.4
        return base + adjustment

    def _compute_freq_resolution(self) -> float:
        sr = self.node.max_sample_rate_hz
        resolution = sr / self.fft_size
        if not self.capabilities:
            return resolution
        freq_accuracy_hz = (self.capabilities.freq_accuracy_ppm / 1e6) * sr
        return max(resolution, freq_accuracy_hz / 2.0)

    def _compute_dedup_window(self) -> int:
        if not self.capabilities:
            return 5_000_000_000
        ppm = self.capabilities.freq_accuracy_ppm
        window_s = 2.0 + ((ppm - 10) / 40.0) * 8.0
        return int(max(1.0, min(window_s, 15.0)) * 1e9)

    # ── Processing ────────────────────────────────────────────────────────────

    async def process_chunk(self, iq_samples: np.ndarray,
                            timestamp_ns: int) -> List[RFEvent]:
        """FFT peak detection on one IQ chunk. Returns detected RFEvents."""
        events: List[RFEvent] = []

        if len(iq_samples) == 0:
            return events

        # Window selection: poorer hardware → Blackman (wider main lobe, less spectral leakage artifacts)
        ppm = self.capabilities.freq_accuracy_ppm if self.capabilities else 30
        window = np.blackman(len(iq_samples)) if ppm > 30 else np.hanning(len(iq_samples))

        windowed = iq_samples * window
        fft = np.abs(np.fft.fftshift(np.fft.fft(windowed, n=self.fft_size)))

        max_val = float(np.max(fft)) or 1.0
        fft_dbfs = 20.0 * np.log10(fft / max_val + 1e-12)

        noise_floor = float(np.median(fft_dbfs))

        try:
            from scipy.signal import find_peaks
            peaks, _ = find_peaks(fft_dbfs, height=self.snr_threshold, distance=3)
        except ImportError:
            # Fallback: threshold-only detection
            peaks = np.where(fft_dbfs > self.snr_threshold)[0]

        center_freq = self.node.center_frequencies_monitored[0] \
            if self.node.center_frequencies_monitored else (self.node.max_sample_rate_hz / 2.0)
        sample_rate = self.node.max_sample_rate_hz

        for peak_idx in peaks:
            half = len(fft) // 2
            if peak_idx == 0 or peak_idx >= len(fft) - 1:
                continue

            # Frequency of this bin relative to center
            bin_offset = (peak_idx - half) / len(fft)
            freq_hz = center_freq + bin_offset * sample_rate

            # Apply frequency calibration
            freq_hz += self.node.frequency_calibration_offset_hz

            # Validate range
            if self.capabilities:
                lo, hi = self.capabilities.freq_range_hz
                if not (lo <= freq_hz <= hi):
                    continue

            power_dbfs = float(fft_dbfs[peak_idx])
            snr = power_dbfs - noise_floor

            # Dedup: skip if we just saw this freq bucket
            bucket = int(freq_hz // self.freq_resolution)
            last_seen = self._seen_freqs.get(bucket, 0)
            if timestamp_ns - last_seen < self.dedup_window_ns:
                continue
            self._seen_freqs[bucket] = timestamp_ns

            event = RFEvent(
                frequency=freq_hz,
                power_dbfs=power_dbfs * self.node.gain_calibration_factor,
                snr_db=snr,
                timestamp_ns=timestamp_ns,
                node_id=self.node.node_id,
                node_trust_score=self.node.compute_trust_score(),
                hardware_id=self.node.hardware_serial,
                detector="hardware_adaptive_fft",
                node_lat=self.node.location_gps[0] if self.node.location_gps else None,
                node_lon=self.node.location_gps[1] if self.node.location_gps else None,
            )
            events.append(event)

        # Trim dedup table to prevent unbounded growth
        if len(self._seen_freqs) > 10_000:
            cutoff = timestamp_ns - self.dedup_window_ns * 2
            self._seen_freqs = {k: v for k, v in self._seen_freqs.items() if v > cutoff}

        return events
