import asyncio
import logging
import numpy as np
from typing import Optional

from .sdr_interface import SDRInterface
from .capabilities import SOAPY_GENERIC_CAPABILITIES, HARDWARE_REGISTRY, GainMode

logger = logging.getLogger(__name__)


class SoapySDRInterface(SDRInterface):
    """Generic SoapySDR driver — covers LimeSDR, PlutoSDR, Airspy, bladeRF, etc."""

    def __init__(self, driver: str, device_args: str = "", channel: int = 0):
        # Try to find matching capabilities
        caps = HARDWARE_REGISTRY.get(driver.lower(), SOAPY_GENERIC_CAPABILITIES)
        super().__init__(
            device_id=f"soapy_{driver}_{device_args or 'default'}",
            capabilities=caps,
        )
        self._driver = driver
        self._device_args = device_args
        self._channel = channel
        self._sdr = None
        self._stream = None
        self._mtu = 1024

    async def open(self):
        try:
            import SoapySDR
            args_str = f"driver={self._driver}"
            if self._device_args:
                args_str += f",{self._device_args}"
            self._sdr = SoapySDR.Device(SoapySDR.KwargsFromString(args_str))
            self._mtu = self._sdr.getStreamMTU(SoapySDR.SOAPY_SDR_RX, self._channel)
            logger.info("SoapySDR '%s' opened (MTU=%d)", self._driver, self._mtu)
        except Exception as exc:
            raise RuntimeError(f"Failed to open SoapySDR '{self._driver}': {exc}") from exc
        await super().open()

    async def close(self):
        if self._stream:
            await self.stop_rx()
        if self._sdr:
            try:
                self._sdr = None
            except Exception:
                pass
        await super().close()

    async def set_frequency(self, freq_hz: float) -> float:
        import SoapySDR
        self._sdr.setFrequency(SoapySDR.SOAPY_SDR_RX, self._channel, float(freq_hz))
        actual = self._sdr.getFrequency(SoapySDR.SOAPY_SDR_RX, self._channel)
        self.current_frequency = actual
        return actual

    async def set_sample_rate(self, rate_hz: int) -> int:
        import SoapySDR
        self._sdr.setSampleRate(SoapySDR.SOAPY_SDR_RX, self._channel, float(rate_hz))
        actual = int(self._sdr.getSampleRate(SoapySDR.SOAPY_SDR_RX, self._channel))
        self.current_sample_rate = actual
        return actual

    async def set_gain(self, gain_db: float, mode: GainMode = GainMode.MANUAL) -> float:
        import SoapySDR
        if mode == GainMode.AGC:
            try:
                self._sdr.setGainMode(SoapySDR.SOAPY_SDR_RX, self._channel, True)
                return -1.0
            except Exception:
                pass
        self._sdr.setGainMode(SoapySDR.SOAPY_SDR_RX, self._channel, False)
        self._sdr.setGain(SoapySDR.SOAPY_SDR_RX, self._channel, float(gain_db))
        actual = self._sdr.getGain(SoapySDR.SOAPY_SDR_RX, self._channel)
        self.current_gain = actual
        return actual

    async def set_antenna(self, antenna_port: int = 0):
        import SoapySDR
        antennas = self._sdr.listAntennas(SoapySDR.SOAPY_SDR_RX, self._channel)
        if antenna_port < len(antennas):
            self._sdr.setAntenna(SoapySDR.SOAPY_SDR_RX, self._channel, antennas[antenna_port])

    async def start_rx(self):
        import SoapySDR
        self._stream = self._sdr.setupStream(SoapySDR.SOAPY_SDR_RX,
                                              SoapySDR.SOAPY_SDR_CF32,
                                              [self._channel])
        self._sdr.activateStream(self._stream)

    async def stop_rx(self):
        if self._stream:
            try:
                self._sdr.deactivateStream(self._stream)
                self._sdr.closeStream(self._stream)
            except Exception:
                pass
            self._stream = None

    async def read_samples(self, num_samples: int) -> np.ndarray:
        import SoapySDR
        buf = np.zeros(num_samples, dtype=np.complex64)
        received = 0
        while received < num_samples:
            chunk_size = min(self._mtu, num_samples - received)
            chunk = np.zeros(chunk_size, dtype=np.complex64)
            sr = self._sdr.readStream(self._stream, [chunk], chunk_size, timeoutUs=1_000_000)
            if sr.ret > 0:
                buf[received:received + sr.ret] = chunk[:sr.ret]
                received += sr.ret
            elif sr.ret == SoapySDR.SOAPY_SDR_TIMEOUT:
                await asyncio.sleep(0.001)
        return buf

    async def get_temperature_c(self) -> Optional[float]:
        try:
            sensors = self._sdr.listSensors()
            for s in sensors:
                if 'temp' in s.lower():
                    return float(self._sdr.readSensor(s))
        except Exception:
            pass
        return None

    async def get_serial_number(self) -> str:
        try:
            info = self._sdr.getHardwareInfo()
            return info.get('serial', self.device_id)
        except Exception:
            return self.device_id


def create_soapy_interface(driver: str, device_args: str = "") -> SoapySDRInterface:
    """Factory convenience function."""
    return SoapySDRInterface(driver=driver, device_args=device_args)
