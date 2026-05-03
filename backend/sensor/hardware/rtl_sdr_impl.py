import asyncio
import logging
import numpy as np
from typing import Optional

from .sdr_interface import SDRInterface
from .capabilities import RTL_SDR_CAPABILITIES, GainMode

logger = logging.getLogger(__name__)

# Allowed RTL-SDR sample rates (Hz)
_ALLOWED_RATES = [250_000, 960_000, 1_024_000, 1_440_000,
                  1_920_000, 2_048_000, 2_400_000, 3_200_000]


class RTLSDRInterface(SDRInterface):
    """RTL-SDR via python-rtlsdr (librtlsdr wrapper)."""

    def __init__(self, device_index: int = 0):
        super().__init__(
            device_id=f"rtlsdr_{device_index}",
            capabilities=RTL_SDR_CAPABILITIES,
        )
        self._device_index = device_index
        self._sdr = None
        self._gain_values: list = []

    async def open(self):
        try:
            import rtlsdr
            self._sdr = rtlsdr.RtlSdr(self._device_index)
            self._gain_values = sorted(self._sdr.get_gains())
            logger.info("RTL-SDR #%d opened, gains: %s", self._device_index, self._gain_values)
        except Exception as exc:
            raise RuntimeError(f"Failed to open RTL-SDR #{self._device_index}: {exc}") from exc
        await super().open()

    async def close(self):
        if self._sdr:
            try:
                self._sdr.close()
            except Exception:
                pass
            self._sdr = None
        await super().close()

    async def set_frequency(self, freq_hz: float) -> float:
        freq_rounded = (int(freq_hz) // 1000) * 1000
        self._sdr.center_freq = int(freq_rounded)
        self.current_frequency = float(freq_rounded)
        return self.current_frequency

    async def set_sample_rate(self, rate_hz: int) -> int:
        closest = min(_ALLOWED_RATES, key=lambda x: abs(x - rate_hz))
        self._sdr.sample_rate = closest
        self.current_sample_rate = closest
        return closest

    async def set_gain(self, gain_db: float, mode: GainMode = GainMode.MANUAL) -> float:
        if mode == GainMode.AGC:
            self._sdr.gain = 'auto'
            self.current_gain = None
            return -1.0
        if not self._gain_values:
            self._gain_values = sorted(self._sdr.get_gains())
        closest = min(self._gain_values, key=lambda x: abs(x - gain_db))
        self._sdr.gain = closest
        self.current_gain = float(closest)
        return self.current_gain

    async def start_rx(self):
        pass  # RTL-SDR read_samples is synchronous/blocking

    async def stop_rx(self):
        pass

    async def read_samples(self, num_samples: int) -> np.ndarray:
        samples = await asyncio.to_thread(self._sdr.read_samples, num_samples)
        return np.array(samples, dtype=np.complex64)

    async def get_serial_number(self) -> str:
        try:
            import rtlsdr
            return rtlsdr.RtlSdr.get_device_serial_addresses()[self._device_index]
        except Exception:
            return self.device_id

    async def get_driver_version(self) -> str:
        try:
            import rtlsdr
            return getattr(rtlsdr, '__version__', 'unknown')
        except Exception:
            return 'unknown'
