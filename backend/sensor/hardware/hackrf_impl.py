import asyncio
import logging
import numpy as np
from queue import Queue, Empty
from typing import Optional

from .sdr_interface import SDRInterface
from .capabilities import HACKRF_CAPABILITIES, GainMode

logger = logging.getLogger(__name__)


class HackRFInterface(SDRInterface):
    """HackRF One via pyhackrf / libhackrf."""

    # LNA gain: 0–40 dB in 8 dB steps
    # VGA gain: 0–62 dB in 2 dB steps
    _LNA_STEPS = list(range(0, 41, 8))
    _VGA_STEPS = list(range(0, 63, 2))

    def __init__(self, device_index: int = 0):
        super().__init__(
            device_id=f"hackrf_{device_index}",
            capabilities=HACKRF_CAPABILITIES,
        )
        self._device_index = device_index
        self._dev = None
        self._sample_queue: Queue = Queue(maxsize=64)
        self._rx_running = False

    async def open(self):
        try:
            import hackrf
            devices = hackrf.find_devices()
            if not devices:
                raise RuntimeError("No HackRF devices found")
            self._dev = devices[self._device_index]
            self._dev.open()
            logger.info("HackRF #%d opened", self._device_index)
        except Exception as exc:
            raise RuntimeError(f"Failed to open HackRF #{self._device_index}: {exc}") from exc
        await super().open()

    async def close(self):
        if self._rx_running:
            await self.stop_rx()
        if self._dev:
            try:
                self._dev.close()
            except Exception:
                pass
            self._dev = None
        await super().close()

    async def set_frequency(self, freq_hz: float) -> float:
        self._dev.frequency = int(freq_hz)
        self.current_frequency = float(freq_hz)
        return self.current_frequency

    async def set_sample_rate(self, rate_hz: int) -> int:
        # HackRF supports 2–20 MHz
        rate = max(2_000_000, min(20_000_000, rate_hz))
        self._dev.sample_rate = rate
        self.current_sample_rate = rate
        return rate

    async def set_gain(self, gain_db: float, mode: GainMode = GainMode.MANUAL) -> float:
        lna = min(self._LNA_STEPS, key=lambda x: abs(x - gain_db))
        remaining = gain_db - lna
        vga = min(self._VGA_STEPS, key=lambda x: abs(x - remaining))
        self._dev.lna_gain = lna
        self._dev.vga_gain = vga
        actual = lna + vga
        self.current_gain = float(actual)
        return actual

    async def start_rx(self):
        self._rx_running = True
        self._dev.start_rx(self._rx_callback)

    async def stop_rx(self):
        if self._rx_running:
            try:
                self._dev.stop_rx()
            except Exception:
                pass
        self._rx_running = False

    def _rx_callback(self, transfer):
        """C callback → push int8 data into queue."""
        raw = np.frombuffer(transfer.buffer, dtype=np.int8)
        iq = (raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)) / 128.0
        try:
            self._sample_queue.put_nowait(iq.astype(np.complex64))
        except Exception:
            pass

    async def read_samples(self, num_samples: int) -> np.ndarray:
        accumulated = np.empty(0, dtype=np.complex64)
        while len(accumulated) < num_samples:
            try:
                chunk = self._sample_queue.get(timeout=1.0)
                accumulated = np.concatenate([accumulated, chunk])
            except Empty:
                await asyncio.sleep(0.01)
        return accumulated[:num_samples]

    async def get_temperature_c(self) -> Optional[float]:
        try:
            return float(self._dev.board_temp_read())
        except Exception:
            return None
