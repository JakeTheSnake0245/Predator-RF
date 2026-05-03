import logging
from abc import ABC, abstractmethod
from typing import Dict, List, Optional, Type
import numpy as np

from backend.models.rf_event import RFEvent
from backend.models.sensor_node import SensorNodeTrust

logger = logging.getLogger(__name__)


class DetectionAlgorithm(ABC):
    """Base class for RF detection algorithms."""

    @abstractmethod
    async def detect(self, iq_samples: np.ndarray,
                     center_freq: float,
                     sample_rate: int) -> List[RFEvent]:
        pass

    @abstractmethod
    def get_capability(self) -> dict:
        pass


class FFTPeakDetector(DetectionAlgorithm):
    """Standard FFT-based spectral peak detection."""

    def __init__(self, snr_threshold_db: float = -15.0, fft_size: int = 4096):
        self.snr_threshold_db = snr_threshold_db
        self.fft_size = fft_size

    async def detect(self, iq_samples: np.ndarray,
                     center_freq: float, sample_rate: int) -> List[RFEvent]:
        import time
        events: List[RFEvent] = []

        window = np.hanning(len(iq_samples))
        fft = np.abs(np.fft.fftshift(np.fft.fft(iq_samples * window, n=self.fft_size)))
        max_val = float(np.max(fft)) or 1.0
        fft_db = 20.0 * np.log10(fft / max_val + 1e-12)
        noise = float(np.median(fft_db))

        try:
            from scipy.signal import find_peaks
            peaks, _ = find_peaks(fft_db, height=self.snr_threshold_db, distance=3)
        except ImportError:
            peaks = np.where(fft_db > self.snr_threshold_db)[0]

        ts = time.time_ns()
        for idx in peaks:
            half = len(fft) // 2
            freq = center_freq + ((idx - half) / len(fft)) * sample_rate
            events.append(RFEvent(
                frequency=freq,
                power_dbfs=float(fft_db[idx]),
                snr_db=float(fft_db[idx]) - noise,
                timestamp_ns=ts,
                node_id="local",
                detector="fft_peak",
            ))
        return events

    def get_capability(self) -> dict:
        return {
            'name': 'fft_peak',
            'hardware_requirements': ['any'],
            'processing_delay_ms': 10,
            'cpu_percent': 20,
        }


class EnergyDetector(DetectionAlgorithm):
    """Simple energy-threshold detector — very low CPU, good for weak hardware."""

    def __init__(self, threshold_dbfs: float = -30.0, block_size: int = 1024):
        self.threshold_dbfs = threshold_dbfs
        self.block_size = block_size

    async def detect(self, iq_samples: np.ndarray,
                     center_freq: float, sample_rate: int) -> List[RFEvent]:
        import time
        events: List[RFEvent] = []
        ts = time.time_ns()

        for i in range(0, len(iq_samples) - self.block_size, self.block_size):
            block = iq_samples[i:i + self.block_size]
            power_linear = float(np.mean(np.abs(block) ** 2))
            if power_linear <= 0:
                continue
            power_db = 10.0 * np.log10(power_linear)
            if power_db > self.threshold_dbfs:
                # Report at center freq (energy detector has no freq resolution)
                events.append(RFEvent(
                    frequency=center_freq,
                    power_dbfs=power_db,
                    snr_db=power_db - self.threshold_dbfs,
                    timestamp_ns=ts + i * 1000,
                    node_id="local",
                    detector="energy",
                ))
                break  # One event per chunk for energy detector

        return events

    def get_capability(self) -> dict:
        return {
            'name': 'energy',
            'hardware_requirements': ['low_power'],
            'processing_delay_ms': 5,
            'cpu_percent': 5,
        }


class DetectorRegistry:
    """Plugin registry for detection algorithms."""

    def __init__(self):
        self._detectors: Dict[str, Type[DetectionAlgorithm]] = {}
        self._register_defaults()

    def _register_defaults(self):
        self.register('fft_peak', FFTPeakDetector)
        self.register('energy', EnergyDetector)

    def register(self, name: str, cls: Type[DetectionAlgorithm]):
        self._detectors[name] = cls
        logger.debug("Registered detector: %s", name)

    def get_detector(self, name: str) -> Optional[DetectionAlgorithm]:
        cls = self._detectors.get(name)
        return cls() if cls else None

    def select_optimal(self, node: SensorNodeTrust,
                       cpu_available: float) -> DetectionAlgorithm:
        if not node.hardware_capabilities or cpu_available < 20:
            return EnergyDetector()
        return FFTPeakDetector()

    def list_detectors(self) -> List[str]:
        return list(self._detectors.keys())


detector_registry = DetectorRegistry()
