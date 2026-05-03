import asyncio
import logging
import time
from typing import Dict, List, Optional, Callable

from backend.models.rf_event import RFEvent
from backend.models.sensor_node import SensorNodeTrust
from backend.sensor.hardware.sdr_interface import SDRInterface
from backend.sensor.modes import ModeConfig, SURVEY_MODE
from backend.sensor.dsp_engine import HardwareAdaptiveDSP
from backend.sensor.calibration.calibrator import SensorCalibrator
from backend.sensor.detection.detector_registry import detector_registry
from backend.sensor.decoders.decoder_registry import decoder_registry

logger = logging.getLogger(__name__)

# Type for event callback
EventCallback = Callable[[RFEvent], None]


class SensorNodeManager:
    """
    Manages one physical SDR node: hardware init, calibration,
    adaptive sensing loop, and event publication.

    For nodes connected via the Kujhad HTTP API (C++ app),
    use KujhadClient instead — this class is for direct Python SDR control.
    """

    def __init__(self, node: SensorNodeTrust, sdr: SDRInterface,
                 on_event: Optional[EventCallback] = None):
        self.node = node
        self.sdr = sdr
        self.on_event = on_event

        self.current_mode: ModeConfig = SURVEY_MODE
        self.dsp = HardwareAdaptiveDSP(node, SURVEY_MODE)
        self.calibrator = SensorCalibrator()

        self._running = False
        self._tasks: List[asyncio.Task] = []

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    async def start(self, auto_calibrate: bool = True):
        await self.sdr.open()
        await self.sdr.set_sample_rate(self.node.max_sample_rate_hz)

        if auto_calibrate and self.calibrator.is_calibration_stale(self.node):
            await self.calibrator.auto_calibrate(self.node, self.sdr)

        self._running = True
        self._tasks = [
            asyncio.create_task(self._sense_loop(), name=f"sense_{self.node.node_id}"),
            asyncio.create_task(self._mode_adapt_loop(), name=f"mode_{self.node.node_id}"),
        ]
        logger.info("SensorNodeManager started for %s (%s)",
                    self.node.node_id, self.node.hardware_code)

    async def stop(self):
        self._running = False
        for t in self._tasks:
            t.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        await self.sdr.close()
        logger.info("SensorNodeManager stopped for %s", self.node.node_id)

    # ── Sensing loop ──────────────────────────────────────────────────────────

    async def _sense_loop(self):
        chunk_samples = self.dsp.fft_size * 4

        while self._running:
            try:
                # Tune if we have a target frequency
                if self.node.center_frequencies_monitored:
                    freq = self.node.center_frequencies_monitored[0]
                    if self.sdr.current_frequency != freq:
                        await self.sdr.set_frequency(freq)

                samples = await self.sdr.read_samples(chunk_samples)
                ts = time.time_ns()

                events = await self.dsp.process_chunk(samples, ts)

                for event in events:
                    self.node.total_observations += 1
                    if self.on_event:
                        self.on_event(event)

            except asyncio.CancelledError:
                break
            except Exception as exc:
                logger.warning("%s: sense loop error: %s", self.node.node_id, exc)
                await asyncio.sleep(1.0)

    # ── Mode adaptation loop ──────────────────────────────────────────────────

    async def _mode_adapt_loop(self):
        from backend.coordination.mode_selector import AdaptiveModeSelector
        selector = AdaptiveModeSelector()

        while self._running:
            try:
                await asyncio.sleep(10.0)

                cpu_pct = self._cpu_available()
                power_pct = 100.0  # Assume mains power unless configured

                new_mode = selector.recommend_mode(
                    self.node, 'NORMAL', cpu_pct, power_pct)

                if new_mode.mode != self.current_mode.mode:
                    logger.info("%s: switching mode %s → %s",
                                self.node.node_id,
                                self.current_mode.mode.value,
                                new_mode.mode.value)
                    self.current_mode = new_mode
                    self.dsp = HardwareAdaptiveDSP(self.node, new_mode)
                    await self._reconfigure_sdr(new_mode)

            except asyncio.CancelledError:
                break
            except Exception as exc:
                logger.warning("%s: mode adapt error: %s", self.node.node_id, exc)

    async def _reconfigure_sdr(self, mode: ModeConfig):
        try:
            center = mode.center_frequency_hz
            if self.node.center_frequencies_monitored:
                center = self.node.center_frequencies_monitored[0]
            await self.sdr.set_frequency(center)
        except Exception as exc:
            logger.warning("SDR reconfigure failed: %s", exc)

    def _cpu_available(self) -> float:
        try:
            import psutil
            return max(0.0, 100.0 - psutil.cpu_percent(interval=0.1))
        except ImportError:
            return 60.0

    # ── Tasking ───────────────────────────────────────────────────────────────

    def set_target_frequencies(self, freqs: List[float]):
        self.node.center_frequencies_monitored = freqs

    def set_mode(self, mode: ModeConfig):
        self.current_mode = mode
        self.dsp = HardwareAdaptiveDSP(self.node, mode)
