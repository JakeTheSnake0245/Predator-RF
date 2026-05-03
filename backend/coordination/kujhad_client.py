"""
KujhadClient — Python bridge to the C++ Predator-RF Kujhad Fleet HTTP API.

The C++ app (running on each sensor node) exposes:
  GET  /v1/identify      → {device, version, role, hwProfile}
  GET  /v1/gps           → {hasFix, lat, lon, accuracy}
  GET  /v1/state         → {vfos, markers, mission, decoders, hits}
  GET  /v1/events?since= → {events:[{serial,time,type,frequency,strengthDb,
                                     label,protocol,networkId,talkgroup,
                                     radioId,decoder,hitState,lat,lon,
                                     accuracyM,gpsFix,encrypted?,raw,...}],
                            lastId}
  POST /v1/command       → {ok, error?}  body: {class, action, args}

Wire schema verified against core/src/gui/main_window.cpp event-row builders
(appendPredatorEvent ~L1334, RTL433 ~L1490, native ~L1545, ADSB ~L1620, P25
~L1714) and kujhad_fleet.h dispatcher (L1062-1194). v1.
"""

import asyncio
import logging
import time
from datetime import datetime, timezone
from typing import Dict, Optional, Callable

try:
    import aiohttp
    _HAVE_AIOHTTP = True
except ImportError:
    _HAVE_AIOHTTP = False

from backend.models.rf_event import RFEvent
from backend.models.sensor_node import SensorNodeTrust

logger = logging.getLogger(__name__)

EventCallback = Callable[[RFEvent], None]

# C++ /v1/events `type` values we treat as RF detections.
# - "hit"     : auto-marker / threshold crossing (appendPredatorEvent path)
# - "decoder" : decoded protocol event (RTL433 / P25 / ADSB / native rtl_433)
# - "detection","peak" : reserved for future Python-side detector pushes
_RF_EVENT_TYPES = frozenset({"hit", "decoder", "detection", "peak"})


def _parse_iso_to_ns(s: str) -> Optional[int]:
    """Parse C++ `time` ISO-8601 string into UNIX nanoseconds.

    The C++ side emits `currentTimestamp()` which is a local-civil ISO string
    (e.g. "2026-05-03 12:34:56" or "2026-05-03T12:34:56Z"). Be tolerant of
    both Z-suffix UTC and naive local. On failure return None so the caller
    can fall back to receive-time."""
    if not s or not isinstance(s, str):
        return None
    # Normalise common separators
    candidate = s.strip().replace(" ", "T")
    if candidate.endswith("Z"):
        candidate = candidate[:-1] + "+00:00"
    try:
        dt = datetime.fromisoformat(candidate)
        if dt.tzinfo is None:
            # Naive: treat as local civil time. mktime returns int seconds;
            # add microsecond → ns separately (no double counting).
            return (int(time.mktime(dt.timetuple())) * 1_000_000_000
                    + dt.microsecond * 1000)
        # Aware: dt.timestamp() already includes the microseconds. Multiply
        # straight to ns and int(); we lose only sub-µs which datetime
        # itself doesn't carry. (Fixes architect-reported double-count.)
        return int(dt.timestamp() * 1_000_000_000)
    except (ValueError, OverflowError):
        return None


class KujhadClient:
    """
    Async client for one C++ Predator-RF node via the Kujhad HTTP API.

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

        # Create the session BEFORE returning so callers may immediately call
        # send_tune_command / send_scan_command / send_mission_command without
        # racing the poll loop. (Architect-reported race fixed.)
        import aiohttp
        # TLS verification: opt-out only; insecure_skip_verify defaults to
        # False on SensorNodeTrust so self-signed fleets must explicitly set
        # it on each node. Plain HTTP nodes are unaffected.
        skip_verify = bool(getattr(self.node,
                                   'kujhad_tls_insecure_skip_verify', False))
        ssl_param = False if (self.node.kujhad_tls and skip_verify) else None
        connector = aiohttp.TCPConnector(ssl=ssl_param)
        self._session = aiohttp.ClientSession(connector=connector,
                                               headers=self._headers)

        self._task = asyncio.create_task(self._poll_loop(),
                                         name=f"kujhad_{self.node.node_id}")
        logger.info("KujhadClient started for %s at %s (tls=%s verify=%s)",
                    self.node.node_id, self._base_url,
                    self.node.kujhad_tls, not skip_verify)

    async def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
            self._task = None
        if self._session:
            await self._session.close()
            self._session = None

    # ── Polling loop ──────────────────────────────────────────────────────────

    async def _poll_loop(self):
        try:
            await self._identify()
            await self._poll_state()  # one shot at startup so callers see
                                       # mission_mode / search_bands quickly
            await self._poll_timing()  # one shot at startup so first-event
                                        # TDOA already has real timing trust
            state_tick = 0
            timing_tick = 0
            # /v1/timing also rarely changes (NTP sync rate, PPS lock
            # transitions); re-poll on its own cadence.
            from backend.config import config as _cfg
            timing_interval_cycles = max(
                1, int(_cfg.timing_poll_interval_s / self.POLL_INTERVAL_S))
            while self._running:
                try:
                    await self._poll_events()
                    await self._poll_gps()
                    state_tick += 1
                    timing_tick += 1
                    if state_tick >= 5:
                        await self._poll_state()
                        state_tick = 0
                    if timing_tick >= timing_interval_cycles:
                        await self._poll_timing()
                        timing_tick = 0
                    await asyncio.sleep(self.POLL_INTERVAL_S)
                except asyncio.CancelledError:
                    break
                except Exception as exc:
                    logger.warning("KujhadClient %s: poll error: %s — reconnecting in %ss",
                                   self.node.node_id, exc, self.RECONNECT_DELAY_S)
                    await asyncio.sleep(self.RECONNECT_DELAY_S)
        except asyncio.CancelledError:
            pass

    async def _identify(self):
        """Fetch device identity, populate hardware code, and infer
        decoder/detector capabilities from the SDR hardware table."""
        try:
            async with self._session.get(
                    f"{self._base_url}/v1/identify",
                    timeout=self.IDENTIFY_TIMEOUT_S) as resp:
                if resp.status == 200:
                    data = await resp.json()
                    logger.info("Node %s identified: %s",
                                self.node.node_id, data.get('device', '?'))
                    hw = data.get('hwProfile', {}) or {}
                    reported = hw.get('hardware')
                    if reported:
                        # Trust the device — operators do mis-configure the
                        # FLEET_NODES env, and the SDR is the only thing
                        # that knows what's actually plugged in. Log loudly
                        # on mismatch so the operator can fix the config.
                        if (self.node.hardware_code
                                and self.node.hardware_code != reported):
                            logger.warning(
                                "Node %s: hardware mismatch — config '%s' "
                                "vs device '%s'. Trusting device.",
                                self.node.node_id,
                                self.node.hardware_code, reported)
                        if self.node.hardware_code != reported:
                            self.node.hardware_code = reported
                            self.node.refresh_hardware_capabilities()
                elif resp.status == 401:
                    logger.error("Node %s: invalid API key", self.node.node_id)
                    return
        except Exception as exc:
            logger.warning("Node %s: identify failed: %s", self.node.node_id, exc)
            return

        # Infer runnable decoders/detectors from the SDR's freq range +
        # parallel detector budget. Cheap, pure, idempotent — safe to call
        # every identify since the hardware doesn't change underneath us.
        try:
            from backend.sensor.hardware.capability_inference import (
                infer_decoder_capabilities, infer_detector_capabilities)
            self.node.available_decoders = infer_decoder_capabilities(self.node)
            self.node.available_detectors = infer_detector_capabilities(self.node)
            if self.node.available_decoders:
                logger.info("Node %s capable of decoders: %s",
                            self.node.node_id,
                            ", ".join(self.node.available_decoders))
        except Exception as exc:
            logger.debug("Capability inference skipped on %s: %s",
                         self.node.node_id, exc)

    async def _poll_state(self):
        """Mirror C++ /v1/state mission/scan/threshold fields onto the node.

        Exact wire fields verified against main_window.cpp:1796-1816
        (kujhadServer.setStateProvider lambda)."""
        try:
            async with self._session.get(
                    f"{self._base_url}/v1/state", timeout=5.0) as resp:
                if resp.status != 200:
                    return
                state = await resp.json()
        except Exception as exc:
            logger.debug("Node %s: state poll failed: %s",
                         self.node.node_id, exc)
            return
        if not isinstance(state, dict):
            return

        from backend.sensor.hardware.capability_inference import search_bands_to_tuples

        try:
            self.node.mission_mode_active = int(state.get('missionMode', 0))
        except (TypeError, ValueError):
            pass
        self.node.scan_running = bool(state.get('scanRunning', False))
        self.node.scan_status = str(state.get('scanStatus', ''))
        try:
            self.node.threshold_db = float(state.get('thresholdDb', 0.0))
        except (TypeError, ValueError):
            pass
        self.node.record_audio = bool(state.get('recordAudio', False))
        self.node.active_search_bands_hz = search_bands_to_tuples(
            state.get('searchBands', []))

    async def _poll_events(self):
        """Poll /v1/events?since=<last_id> and convert to RFEvents."""
        url = f"{self._base_url}/v1/events?since={self._last_event_id}"
        async with self._session.get(url, timeout=10.0) as resp:
            if resp.status != 200:
                return
            data = await resp.json()

        events = data.get('events', []) or []
        last_id = data.get('lastId', self._last_event_id)

        for raw in events:
            rf_event = self._kujhad_event_to_rf(raw)
            if rf_event and self._on_event:
                try:
                    self._on_event(rf_event)
                except Exception as exc:
                    logger.exception("on_event callback raised on %s: %s",
                                     self.node.node_id, exc)

        if isinstance(last_id, int) and last_id > self._last_event_id:
            self._last_event_id = last_id

    async def _poll_gps(self):
        """Update node GPS location from /v1/gps (per-node fallback)."""
        async with self._session.get(
                f"{self._base_url}/v1/gps", timeout=5.0) as resp:
            if resp.status != 200:
                return
            gps = await resp.json()

        if gps.get('hasFix'):
            try:
                lat = float(gps.get('lat', 0))
                lon = float(gps.get('lon', 0))
                acc = float(gps.get('accuracy', 10))
                self.node.location_gps = (lat, lon)
                self.node.location_accuracy_m = acc
                # Stamp the lock-age clock — TDOACoordinator uses this
                # to drop stale-GPS nodes from solves.
                self.node.location_gps_updated_ns = time.time_ns()
            except (TypeError, ValueError):
                pass

    async def _poll_timing(self):
        """Pull timing telemetry from the C++ /v1/timing endpoint and
        recompute timing_stability_trust from the device's *measured*
        clock state instead of guessing from the hardware code.

        Wire shape (the C++ side returns 200 with this body when supported,
        404 otherwise — we treat 404 as "feature not present, leave the
        hardware-derived defaults in place"):
          { source: "gpsdo"|"ntp"|"system",
            ppsLock: bool, lastSyncSec: number, offsetMs: number,
            driftPpm: number }

        timing_stability_trust mapping:
          - gpsdo + ppsLock + |offset|<10 ms        → 0.98
          - gpsdo + ppsLock                         → 0.90
          - gpsdo (no PPS)                          → 0.75
          - ntp + |offset|<25 ms + sync<60 s        → 0.70
          - ntp + |offset|<100 ms                   → 0.55
          - ntp (any worse)                         → 0.40
          - system clock only                       → 0.30
        Lock age beyond 5 min penalises by an additional 0.2.
        """
        try:
            async with self._session.get(
                    f"{self._base_url}/v1/timing", timeout=5.0) as resp:
                if resp.status == 404:
                    return  # node doesn't expose timing telemetry
                if resp.status != 200:
                    return
                t = await resp.json()
        except Exception as exc:
            logger.debug("Node %s: timing poll failed: %s",
                         self.node.node_id, exc)
            return
        if not isinstance(t, dict):
            return
        src = str(t.get("source") or "").lower() or "system"
        pps = bool(t.get("ppsLock"))
        try:
            off_ms = float(t.get("offsetMs", 0.0))
        except (TypeError, ValueError):
            off_ms = 0.0
        try:
            drift_ppm = float(t.get("driftPpm", 0.0))
        except (TypeError, ValueError):
            drift_ppm = 0.0
        try:
            last_sync_s = float(t.get("lastSyncSec", 0.0))
        except (TypeError, ValueError):
            last_sync_s = 0.0
        # Reverse-engineer last_sync to UNIX ns — `lastSyncSec` is the
        # AGE of the sync, so subtract from now.
        if last_sync_s > 0:
            self.node.timing_last_sync_ns = (
                time.time_ns() - int(last_sync_s * 1e9))
        self.node.timing_source = src
        self.node.timing_pps_lock = pps
        self.node.timing_offset_ms = off_ms
        self.node.clock_drift_ppm = drift_ppm
        self.node.gps_synchronized = (src == "gpsdo" and pps)
        # Compute trust
        if src == "gpsdo" and pps and abs(off_ms) < 10:
            trust = 0.98
        elif src == "gpsdo" and pps:
            trust = 0.90
        elif src == "gpsdo":
            trust = 0.75
        elif src == "ntp" and abs(off_ms) < 25 and last_sync_s < 60:
            trust = 0.70
        elif src == "ntp" and abs(off_ms) < 100:
            trust = 0.55
        elif src == "ntp":
            trust = 0.40
        else:
            trust = 0.30
        if last_sync_s > 300:
            trust = max(0.2, trust - 0.2)
        self.node.timing_stability_trust = trust
        logger.debug("Node %s timing: src=%s pps=%s offset=%.1fms "
                     "drift=%.1fppm age=%.0fs → trust=%.2f",
                     self.node.node_id, src, pps, off_ms,
                     drift_ppm, last_sync_s, trust)

    # ── Event conversion (verified against C++ wire schema) ──────────────────

    def _kujhad_event_to_rf(self, raw: dict) -> Optional[RFEvent]:
        """Convert a C++ Kujhad event row into an RFEvent.

        See module docstring for the full wire schema and docs/2 for the
        contract reference."""
        if not isinstance(raw, dict):
            return None

        event_type = raw.get('type', '')
        if event_type not in _RF_EVENT_TYPES:
            return None

        try:
            freq = float(raw.get('frequency', 0.0))
        except (TypeError, ValueError):
            return None
        if freq <= 0:
            return None

        # Power: C++ emits `strengthDb` (canonical). Accept `strength` /
        # `power` for forward-compat with future Python-side producers.
        strength_raw = raw.get('strengthDb',
                       raw.get('strength',
                       raw.get('power', None)))
        try:
            strength = float(strength_raw) if strength_raw is not None else -80.0
        except (TypeError, ValueError):
            strength = -80.0

        # SNR not on the C++ wire today; default 0. Forward-compat.
        try:
            snr = float(raw.get('snr', raw.get('snrDb', 0.0)))
        except (TypeError, ValueError):
            snr = 0.0

        # Timestamp: prefer C++ ISO `time`, then `ts_ns`, then receive-time.
        ts_ns = None
        if 'ts_ns' in raw:
            try:
                ts_ns = int(raw['ts_ns'])
            except (TypeError, ValueError):
                ts_ns = None
        if ts_ns is None and 'time' in raw:
            ts_ns = _parse_iso_to_ns(raw.get('time', ''))
        if ts_ns is None:
            ts_ns = time.time_ns()

        # GPS: prefer per-event coords (captured at event time on the device)
        # before falling back to the 1 Hz-polled node position.
        lat = lon = None
        if raw.get('gpsFix') and 'lat' in raw and 'lon' in raw:
            try:
                lat = float(raw['lat'])
                lon = float(raw['lon'])
            except (TypeError, ValueError):
                lat = lon = None
        if lat is None and self.node.location_gps:
            lat, lon = self.node.location_gps

        # Detector tag: use the C++ decoder name when present so downstream
        # confidence weighting can distinguish RTL433 from a P25 trunk hit.
        decoder = raw.get('decoder') or 'kujhad_bridge'
        detector_tag = f"kujhad:{decoder.lower()}"

        # Decoded payload: prefer label, fall back to talkgroup/networkId
        # so the operator UI has *something* per-event even on bare hits.
        payload = raw.get('label') or None
        protocol = raw.get('protocol') or None

        return RFEvent(
            frequency=freq,
            power_dbfs=strength,
            snr_db=snr,
            timestamp_ns=ts_ns,
            node_id=self.node.node_id,
            node_trust_score=self.node.compute_trust_score(),
            hardware_id=self.node.hardware_serial,
            detector=detector_tag,
            modulation=raw.get('modulation'),
            protocol=protocol,
            decoded_payload=payload,
            node_lat=lat,
            node_lon=lon,
        )

    # ── Commands (matches C++ KujhadDeviceCommand: class+action+args) ────────

    async def send_tune_command(self, frequency_hz: float,
                                vfo: str = "VFO A") -> bool:
        """Task the node to tune to a frequency.

        Wire shape (verified against main_window.cpp:1936):
          {"class":"tune","action":"set","args":{"frequencyHz":Hz,"vfo":...}}
        """
        payload = {
            "class": "tune",
            "action": "set",
            "args": {"frequencyHz": float(frequency_hz), "vfo": vfo},
        }
        return await self._post_command(payload)

    async def send_scan_command(self, freq_start_hz: float,
                                freq_end_hz: float,
                                dwell_ms: int = 500,
                                start: bool = True) -> bool:
        """Task the node to start (or stop) a frequency scan.

        Wire shape (verified against main_window.cpp:1972):
          {"class":"scan","action":"start"|"stop","args":{...}}
        """
        payload = {
            "class": "scan",
            "action": "start" if start else "stop",
            "args": {
                "startHz": float(freq_start_hz),
                "endHz":   float(freq_end_hz),
                "dwellMs": int(dwell_ms),
            },
        }
        return await self._post_command(payload)

    async def send_mission_command(self, action: str, args: dict) -> bool:
        """Task the node with a mission.* command.

        action ∈ {setMode, setSearchBands, setTargets, setExcludes, setSettings}
        See main_window.cpp:1941-1967 for per-action arg requirements."""
        payload = {"class": "mission", "action": action, "args": args}
        return await self._post_command(payload)

    async def _post_command(self, payload: dict) -> bool:
        try:
            async with self._session.post(
                    f"{self._base_url}/v1/command",
                    json=payload, timeout=5.0) as resp:
                data = await resp.json()
                ok = bool(data.get('ok', False))
                if not ok:
                    logger.warning("Command rejected on %s: %s — %s",
                                   self.node.node_id, payload.get('class'),
                                   data.get('error', 'no reason'))
                return ok
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

    def get_client(self, node_id: str) -> Optional[KujhadClient]:
        return self._clients.get(node_id)
