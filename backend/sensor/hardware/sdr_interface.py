from abc import ABC, abstractmethod
from typing import Optional
import asyncio
import numpy as np

from .capabilities import SDRCapabilities, GainMode


class SDRInterface(ABC):
    """Abstract base for all SDR hardware drivers."""

    def __init__(self, device_id: str, capabilities: SDRCapabilities):
        self.device_id = device_id
        self.capabilities = capabilities
        self.is_open = False
        self.current_frequency: Optional[float] = None
        self.current_gain: Optional[float] = None
        self.current_sample_rate: Optional[int] = None

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    @abstractmethod
    async def open(self):
        self.is_open = True

    @abstractmethod
    async def close(self):
        self.is_open = False

    # ── Tuning ────────────────────────────────────────────────────────────────

    @abstractmethod
    async def set_frequency(self, freq_hz: float) -> float:
        self.current_frequency = freq_hz
        return freq_hz

    @abstractmethod
    async def set_sample_rate(self, rate_hz: int) -> int:
        self.current_sample_rate = rate_hz
        return rate_hz

    @abstractmethod
    async def set_gain(self, gain_db: float, mode: GainMode = GainMode.MANUAL) -> float:
        self.current_gain = gain_db
        return gain_db

    async def set_antenna(self, antenna_port: int = 0):
        pass

    # ── I/Q Streaming ─────────────────────────────────────────────────────────

    @abstractmethod
    async def start_rx(self):
        pass

    @abstractmethod
    async def stop_rx(self):
        pass

    @abstractmethod
    async def read_samples(self, num_samples: int) -> np.ndarray:
        """Return complex64 IQ samples."""
        pass

    # ── Optional capabilities ─────────────────────────────────────────────────

    async def enable_agc(self, enabled: bool = True):
        if GainMode.AGC not in self.capabilities.gain_modes:
            raise NotImplementedError(f"{self.capabilities.hardware_name} does not support AGC")

    async def get_rssi(self) -> Optional[float]:
        return None

    async def get_temperature_c(self) -> Optional[float]:
        return None

    async def get_serial_number(self) -> str:
        return self.device_id

    async def get_driver_version(self) -> str:
        return "unknown"

    # ── Context manager support ───────────────────────────────────────────────

    async def __aenter__(self):
        await self.open()
        return self

    async def __aexit__(self, *_):
        await self.close()
