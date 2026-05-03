"""
KujhadClient — Python bridge to the C++ Predator-SDR Kujhad Fleet HTTP API.

The C++ app (running on each sensor node) exposes:
  GET  /v1/identify      → device info
  GET  /v1/gps           → GPS fix
  GET  /v1/state         → VFOs, markers, hits, decoders
  GET  /v1/events?since= → event stream (long-poll friendly)
  POST /v1/command       → issue tune/scan/mission commands

This client polls /v1/events continuously and converts Kujhad events
into RFEvent objects for the Python fusion backend.
"""

import asyncio
import logging
import time
from typing import Dict, List, Optional, Callable

try:
    import aiohttp
    _HAVE_AIOHTTP = True
except ImportError:
    _HAVE_AIOHTTP = False

from backend.models.rf_event import RFEvent
from backend.models.sensor_node import SensorNodeTrust

logger = logging.getLogger(__name__)

EventCallback = Callable[[RFEvent], None]


class KujhadClient:
    """
    Async client for one C++ Predator-SDR node via the Kujhad HTTP API.

    Usage:
        client = KujhadClient(node)
        await client.start(on_event=my_callback)
        ...
        await client.stop()
    """

    POLL_INTERVAL_S = 1.0
    RECONNECT_DELAY_S = 5.0
    IDENTIFY_TIMEOUT_S = 5.0

    def __init__(self, node: SensorNodeTrust):
        self.node = node
        self._base_url = node.kujhad_base_url()
        self._headers = {'X-Kujhad-Key': node.kujhad_api_key}
        self._on_event: Optional[EventCallback] = None
        self._running = False
        self._task: Optional[asyncio.Task] = None
        self._last_event_id: int = 0
        self._session: Optional[object] = None  # aiohttp.ClientSession

    async def start(self, on_event: Optional[EventCallback] = None):
        if not _HAVE_AIOHTTP:
            raise RuntimeError("aiohttp is required for KujhadClient. "
                               "Install it: pip install aiohttp")
        self._on_event = on_event
        self._running = True
        self._task = asyncio.create_task(self._poll_loop(),
                                         name=f"kujhad_{self.node.node_id}")
        logger.info("KujhadClient started for %s at %s",
                    self.node.node_id, self._base_url)

    async def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
        if self._session:
            await self._session.close()
            self._session = None

    # ── Polling loop ──────────────────────────────────────────────────────────

    async def _poll_loop(self):
        import aiohttp
        connector = aiohttp.TCPConnector(ssl=False)  # SSL handled by TLS option
        self._session = aiohttp.ClientSession(connector=connector,
                                               headers=self._headers)
        try:
            # Initial identify
            await self._identify()

            while self._running:
                try:
                    await self._poll_events()
                    await self._poll_gps()
                    await asyncio.sleep(self.POLL_INTERVAL_S)
                except asyncio.CancelledError:
                    break
                except Exception as exc:
                    logger.warning("KujhadClient %s: poll error: %s — reconnecting in %ss",
                                   self.node.node_id, exc, self.RECONNECT_DELAY_S)
                    await asyncio.sleep(self.RECONNECT_DELAY_S)
        finally:
            await self._session.close()
            self._session = None

    async def _identify(self):
        """Fetch device identity and populate node metadata."""
        try:
            async with self._session.get(
                    f"{self._base_url}/v1/identify",
                    timeout=self.IDENTIFY_TIMEOUT_S) as resp:
                if resp.status == 200:
                    data = await resp.json()
                    logger.info("Node %s identified: %s",
                                self.node.node_id, data.get('device', '?'))
                    # Update hardware info from device if available
                    hw = data.get('hwProfile', {})
                    if hw.get('hardware') and not self.node.hardware_code:
                        self.node.hardware_code = hw['hardware']
                elif resp.status == 401:
                    logger.error("Node %s: invalid API key", self.node.node_id)
        except Exception as exc:
            logger.warning("Node %s: identify failed: %s", self.node.node_id, exc)

    async def _poll_events(self):
        """Poll /v1/events?since=<last_id> and convert to RFEvents."""
        url = f"{self._base_url}/v1/events?since={self._last_event_id}"
        async with self._session.get(url, timeout=10.0) as resp:
            if resp.status != 200:
                return
            data = await resp.json()

        events = data.get('events', [])
        last_id = data.get('lastId', self._last_event_id)

        for raw in events:
            rf_event = self._kujhad_event_to_rf(raw)
            if rf_event and self._on_event:
                self._on_event(rf_event)

        self._last_event_id = last_id

    async def _poll_gps(self):
        """Update node GPS location from /v1/gps."""
        async with self._session.get(
                f"{self._base_url}/v1/gps", timeout=5.0) as resp:
            if resp.status != 200:
                return
            gps = await resp.json()

        if gps.get('hasFix'):
            lat = float(gps.get('lat', 0))
            lon = float(gps.get('lon', 0))
            acc = float(gps.get('accuracy', 10))
            self.node.location_gps = (lat, lon)
            self.node.location_accuracy_m = acc

    # ── Event conversion ──────────────────────────────────────────────────────

    def _kujhad_event_to_rf(self, raw: dict) -> Optional[RFEvent]:
        """
        Convert a Kujhad JSON event record (from /v1/events) to RFEvent.

        The C++ app emits events like:
          {"id": 123, "type": "hit", "frequency": 154.1e6, "strength": -75.3,
           "snr": 12.1, "ts_ns": 1714200000000000000, "label": "P25 voice",
           "protocol": "P25", "modulation": "C4FM"}
        """
        event_type = raw.get('type', '')
        if event_type not in ('hit', 'detection', 'peak'):
            return None  # Only RF detection events

        freq = raw.get('frequency', 0.0)
        if freq <= 0:
            return None

        strength = raw.get('strength', raw.get('power', -80.0))
        snr = raw.get('snr', 0.0)
        ts = raw.get('ts_ns', time.time_ns())

        lat = lon = None
        if self.node.location_gps:
            lat, lon = self.node.location_gps

        return RFEvent(
            frequency=float(freq),
            power_dbfs=float(strength),
            snr_db=float(snr),
            timestamp_ns=int(ts),
            node_id=self.node.node_id,
            node_trust_score=self.node.compute_trust_score(),
            hardware_id=self.node.hardware_serial,
            detector="kujhad_bridge",
            modulation=raw.get('modulation'),
            protocol=raw.get('protocol'),
            decoded_payload=raw.get('label'),
            node_lat=lat,
            node_lon=lon,
        )

    # ── Commands ──────────────────────────────────────────────────────────────

    async def send_tune_command(self, frequency_hz: float, vfo: str = "VFO A") -> bool:
        """Task the C++ node to tune to a frequency."""
        payload = {"class": "tune", "vfo": vfo, "frequency": frequency_hz}
        return await self._post_command(payload)

    async def send_scan_command(self, freq_start_hz: float,
                                 freq_end_hz: float, dwell_ms: int = 500) -> bool:
        """Task the C++ node to run a frequency scan."""
        payload = {
            "class": "scan",
            "start": freq_start_hz,
            "end": freq_end_hz,
            "dwellMs": dwell_ms,
        }
        return await self._post_command(payload)

    async def _post_command(self, payload: dict) -> bool:
        try:
            async with self._session.post(
                    f"{self._base_url}/v1/command",
                    json=payload, timeout=5.0) as resp:
                data = await resp.json()
                return bool(data.get('ok', False))
        except Exception as exc:
            logger.warning("Command failed on %s: %s", self.node.node_id, exc)
            return False


class KujhadFleetManager:
    """
    Manages a fleet of KujhadClient instances (one per C++ sensor node).

    Aggregates all events into a single callback for the fusion backend.
    """

    def __init__(self):
        self._clients: Dict[str, KujhadClient] = {}
        self._on_event: Optional[EventCallback] = None

    def on_event(self, fn: EventCallback):
        self._on_event = fn

    async def add_node(self, node: SensorNodeTrust):
        if node.node_id in self._clients:
            return
        client = KujhadClient(node)
        self._clients[node.node_id] = client
        await client.start(on_event=self._on_event)
        logger.info("Fleet: added node %s (%s:%d)",
                    node.node_id, node.kujhad_host, node.kujhad_port)

    async def remove_node(self, node_id: str):
        client = self._clients.pop(node_id, None)
        if client:
            await client.stop()

    async def stop_all(self):
        for client in self._clients.values():
            await client.stop()
        self._clients.clear()

    async def broadcast_tune(self, frequency_hz: float):
        """Tune all nodes to the same frequency."""
        for client in self._clients.values():
            await client.send_tune_command(frequency_hz)

    def node_count(self) -> int:
        return len(self._clients)
