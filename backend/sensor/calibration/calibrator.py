import logging
import time
import numpy as np
from typing import Optional

from backend.models.sensor_node import SensorNodeTrust
from backend.sensor.hardware.sdr_interface import SDRInterface

logger = logging.getLogger(__name__)


class SensorCalibrator:
    """Frequency offset and gain calibration per sensor node."""

    async def calibrate_frequency(self, node: SensorNodeTrust, sdr: SDRInterface,
                                   reference_frequency_hz: float,
                                   num_samples: int = 100_000) -> float:
        """
        Measure frequency offset against a known reference.

        Common sources: GPS-disciplined oscillator, NIST WWV (10 MHz),
        known FM broadcast station, or signal generator.

        Returns offset in Hz (stored on node).
        """
        await sdr.set_frequency(reference_frequency_hz)
        samples = await sdr.read_samples(num_samples)

        fft = np.abs(np.fft.fft(samples))
        peak_idx = int(np.argmax(fft[:len(fft) // 2]))
        sr = sdr.current_sample_rate or node.max_sample_rate_hz
        measured_freq = (peak_idx / len(fft)) * sr

        offset_hz = measured_freq - reference_frequency_hz

        node.frequency_calibration_offset_hz = offset_hz
        node.last_calibration_ns = time.time_ns()

        logger.info("%s: frequency offset = %.1f Hz (%.2f ppm)",
                    node.node_id, offset_hz,
                    (offset_hz / reference_frequency_hz) * 1e6)
        return offset_hz

    async def calibrate_gain(self, node: SensorNodeTrust, sdr: SDRInterface,
                              reference_power_dbm: float,
                              reference_frequency_hz: float,
                              num_samples: int = 100_000) -> float:
        """
        Measure gain correction factor against a known reference signal power.

        Returns correction factor (multiply measured power by this value).
        """
        await sdr.set_frequency(reference_frequency_hz)
        samples = await sdr.read_samples(num_samples)

        power_linear = float(np.mean(np.abs(samples) ** 2))
        power_dbfs = 10.0 * np.log10(power_linear + 1e-12)

        correction_db = reference_power_dbm - power_dbfs
        correction_factor = 10 ** (correction_db / 10.0)

        node.gain_calibration_factor = correction_factor
        node.last_calibration_ns = time.time_ns()

        logger.info("%s: gain correction = %.1f dB (factor=%.4f)",
                    node.node_id, correction_db, correction_factor)
        return correction_factor

    async def auto_calibrate(self, node: SensorNodeTrust, sdr: SDRInterface):
        """
        Quick auto-calibration using NIST WWV station at 10 MHz.

        Only runs frequency calibration (no known power reference).
        """
        reference_freq = 10e6   # NIST WWV shortwave station
        try:
            await self.calibrate_frequency(node, sdr, reference_freq)
            logger.info("%s: auto-calibration complete", node.node_id)
        except Exception as exc:
            logger.warning("%s: auto-calibration failed: %s", node.node_id, exc)

    def is_calibration_stale(self, node: SensorNodeTrust,
                              max_age_hours: float = 24.0) -> bool:
        if node.last_calibration_ns == 0:
            return True
        age_ns = time.time_ns() - node.last_calibration_ns
        return age_ns > max_age_hours * 3600 * 1e9
