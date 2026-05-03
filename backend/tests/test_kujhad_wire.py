"""
End-to-end smoke test for the Python backend ↔ C++ Kujhad wire contract.

Spins up a MockKujhadServer (aiohttp app implementing the same JSON shapes
the real C++ Kujhad device server emits — verified against
core/src/gui/main_window.cpp event-row builders L1334/L1490/L1545/L1620/L1714
and core/src/predator/kujhad_fleet.h dispatcher L1062-1194) and runs the
full PredatorBackend pipeline against it: KujhadClient → RFEvent →
TrackManager → AnomalyDetector → REST.

Run from repo root:
    python -m backend.tests.test_kujhad_wire

Or with pytest if installed:
    pytest backend/tests/test_kujhad_wire.py -v

Pure stdlib + aiohttp — no pytest required for the standalone runner.
"""
from __future__ import annotations

import asyncio
import json
import logging
import socket
import sys
import time
import unittest
from contextlib import closing
from typing import List

# Ensure repo root is on sys.path
import os
_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _root not in sys.path:
    sys.path.insert(0, _root)

import unittest

try:
    from aiohttp import web
    _HAVE_AIOHTTP = True
except ImportError:
    _HAVE_AIOHTTP = False


def load_tests(loader, tests, pattern):
    """Unittest hook — when aiohttp is missing, return an empty suite
    so the runner reports zero tests for this module instead of an
    ImportError. The module's class-level definitions below are still
    evaluated when aiohttp IS present."""
    if not _HAVE_AIOHTTP:
        return unittest.TestSuite()
    return tests


if not _HAVE_AIOHTTP:
    # Bare-stubs so the rest of the module body is syntactically
    # importable — load_tests above will then throw it all away.
    class _Stub:
        def __getattr__(self, _):
            return _Stub()
        def __call__(self, *a, **k):
            return _Stub()
    web = _Stub()  # type: ignore

from backend.coordination.kujhad_client import (
    KujhadClient, KujhadFleetManager, _parse_iso_to_ns)
from backend.models.rf_event import RFEvent
from backend.models.sensor_node import SensorNodeTrust


logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger("test.kujhad_wire")


# ─────────────────────────────────────────────────────────────────────────────
# Mock C++ Kujhad device server — emits the verified wire schema.
# ─────────────────────────────────────────────────────────────────────────────

API_KEY = "test-api-key-do-not-leak"


class MockKujhadServer:
    """aiohttp app mirroring the C++ Kujhad HTTP server's JSON contract."""

    def __init__(self):
        self.events: list = []          # newest events appended; serial=index+1
        self.command_log: list = []
        self.gps = {"hasFix": True, "lat": 35.123, "lon": -106.456, "accuracy": 4.2}
        # Default /v1/state — matches main_window.cpp:1796-1816 lambda shape.
        self.state = {
            "centerFreq": 462_612_500.0,
            "playing": True,
            "missionMode": 1,
            "scanRunning": True,
            "scanStatus": "scanning 420-450 MHz",
            "searchBands": [
                {"startHz": 420_000_000.0, "endHz": 450_000_000.0},
                {"startHz": 144_000_000.0, "endHz": 148_000_000.0},
            ],
            "targets": [], "excludes": [],
            "thresholdDb": -65.0,
            "dwellMs": 500,
            "quickScanDelayMs": 0,
            "quickScanDurationMs": 0,
            "recordAudio": True,
        }
        self.runner: web.AppRunner | None = None
        self.port: int = 0

    async def start(self) -> int:
        app = web.Application()
        app.router.add_get("/v1/identify", self._identify)
        app.router.add_get("/v1/state", self._state)
        app.router.add_get("/v1/gps", self._gps)
        app.router.add_get("/v1/events", self._events)
        app.router.add_post("/v1/command", self._command)

        self.runner = web.AppRunner(app)
        await self.runner.setup()

        # Pick a free port
        with closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]

        site = web.TCPSite(self.runner, "127.0.0.1", self.port)
        await site.start()
        return self.port

    async def stop(self):
        if self.runner:
            await self.runner.cleanup()

    # ── Auth helper ────────────────────────────────────────────────────────
    @staticmethod
    def _auth_ok(req: web.Request) -> bool:
        return req.headers.get("X-Kujhad-Key") == API_KEY

    # ── Endpoints ──────────────────────────────────────────────────────────
    async def _identify(self, req):
        if not self._auth_ok(req):
            return web.json_response({"error": "unauthorized"}, status=401)
        return web.json_response({
            "device": "predator-mock-001",
            "version": "1.3.0",
            "role": "Device",
            "hwProfile": {"hardware": "hackrf"},
        })

    async def _state(self, req):
        if not self._auth_ok(req):
            return web.json_response({"error": "unauthorized"}, status=401)
        return web.json_response(self.state)

    async def _gps(self, req):
        if not self._auth_ok(req):
            return web.json_response({"error": "unauthorized"}, status=401)
        return web.json_response(self.gps)

    async def _events(self, req):
        if not self._auth_ok(req):
            return web.json_response({"error": "unauthorized"}, status=401)
        try:
            since = int(req.query.get("since", "0"))
        except ValueError:
            since = 0
        out = [e for e in self.events if e["serial"] > since]
        last = max((e["serial"] for e in self.events), default=since)
        return web.json_response({"events": out, "lastId": last})

    async def _command(self, req):
        if not self._auth_ok(req):
            return web.json_response({"error": "unauthorized"}, status=401)
        try:
            body = await req.json()
        except Exception:
            body = {}
        self.command_log.append(body)
        # Mimic C++ validation in main_window.cpp:1936-1980
        cls = body.get("class", "")
        action = body.get("action", "")
        args = body.get("args", {}) or {}
        if cls.startswith("tx"):
            return web.json_response({"ok": False, "error": "tx disabled"}, status=403)
        if cls == "tune" and action == "set":
            if float(args.get("frequencyHz", 0)) <= 0:
                return web.json_response({"ok": False, "error": "frequencyHz required"}, status=400)
            return web.json_response({"ok": True})
        if cls == "scan" and action in ("start", "stop"):
            return web.json_response({"ok": True})
        if cls == "mission":
            return web.json_response({"ok": True})
        if cls == "identify":
            return web.json_response({"ok": True})
        return web.json_response({"ok": False, "error": "unknown"}, status=400)

    # ── Helpers for tests ─────────────────────────────────────────────────
    def push_hit(self, freq_hz: float, strength_db: float = -55.0,
                 label: str = "auto-hit") -> dict:
        """Append an event in the C++ `appendPredatorEvent` shape (type='hit')."""
        serial = len(self.events) + 1
        ev = {
            "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "eventId": f"mock_hit_{serial}",
            "type": "hit",
            "frequency": float(freq_hz),
            "label": label,
            "strengthDb": float(strength_db),
            "decoder": "None",
            "hitState": "active",
            "protocol": "Unknown",
            "networkId": "Unknown",
            "talkgroup": "Unknown",
            "radioId": "Unknown",
            "source": "Mock",
            "sourceDevice": "local",
            "mode": 0,
            "gpsFix": True,
            "lat": self.gps["lat"], "lon": self.gps["lon"],
            "accuracyM": self.gps["accuracy"],
            "serial": serial,
        }
        self.events.append(ev)
        return ev

    def push_decoder(self, freq_hz: float, decoder: str, protocol: str,
                     talkgroup: str = "TG 1234",
                     network_id: str = "WACN 0xBEE00",
                     strength_db: float = -62.0) -> dict:
        """Append a decoder event (RTL433/P25/ADSB pattern, type='decoder')."""
        serial = len(self.events) + 1
        ev = {
            "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "eventId": f"mock_dec_{serial}",
            "type": "decoder",
            "frequency": float(freq_hz),
            "label": f"{protocol} call",
            "strengthDb": float(strength_db),
            "decoder": decoder,
            "hitState": "decoded",
            "protocol": protocol,
            "networkId": network_id,
            "talkgroup": talkgroup,
            "radioId": "Unit 5001",
            "source": f"Bridge:{decoder}",
            "mode": 1,
            "gpsFix": True,
            "lat": self.gps["lat"], "lon": self.gps["lon"],
            "accuracyM": self.gps["accuracy"],
            "raw": {"encrypted": False},
            "serial": serial,
        }
        self.events.append(ev)
        return ev

    def push_garbage(self) -> dict:
        """An event with a type Python should drop (C++ may emit it later)."""
        serial = len(self.events) + 1
        ev = {"time": "x", "type": "telemetry", "frequency": 100e6,
              "strengthDb": -50.0, "serial": serial}
        self.events.append(ev)
        return ev


# ─────────────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────────────

def _make_node(port: int) -> SensorNodeTrust:
    return SensorNodeTrust(
        node_id="mock-node-1",
        hardware_code="hackrf",
        kujhad_host="127.0.0.1",
        kujhad_port=port,
        kujhad_api_key=API_KEY,
    )


class TestIsoTimestampParse(unittest.TestCase):
    """B3 — ISO timestamp string from C++ → ns."""

    def test_zulu(self):
        ns = _parse_iso_to_ns("2026-05-03T12:34:56Z")
        self.assertIsNotNone(ns)
        # Reasonable range: > 2026-01-01 and < 2027-01-01
        assert ns is not None
        self.assertGreater(ns, 1767225600_000_000_000)
        self.assertLess(ns,    1798761600_000_000_000)

    def test_space_separator(self):
        ns = _parse_iso_to_ns("2026-05-03 12:34:56")
        self.assertIsNotNone(ns)
        self.assertGreater(ns, 1767225600_000_000_000)

    def test_garbage(self):
        self.assertIsNone(_parse_iso_to_ns(""))
        self.assertIsNone(_parse_iso_to_ns("not-a-date"))
        self.assertIsNone(_parse_iso_to_ns(None))  # type: ignore[arg-type]


class TestKujhadWireSchema(unittest.IsolatedAsyncioTestCase):
    """B1-B5 + GPS — full wire round-trip against MockKujhadServer."""

    async def asyncSetUp(self):
        self.server = MockKujhadServer()
        self.port = await self.server.start()
        self.received: List[RFEvent] = []
        self.node = _make_node(self.port)
        self.client = KujhadClient(self.node)

    async def asyncTearDown(self):
        await self.client.stop()
        await self.server.stop()

    async def test_hit_event_full_round_trip(self):
        """B1 (type='hit' accepted), B2 (strengthDb→power), B3 (time→ts_ns)."""
        self.server.push_hit(154_400_000.0, strength_db=-48.5, label="VHF marine 16")

        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(2.0)  # one poll cycle + slack

        self.assertEqual(len(self.received), 1, "exactly one RFEvent expected")
        ev = self.received[0]
        self.assertAlmostEqual(ev.frequency, 154_400_000.0, places=1)
        self.assertAlmostEqual(ev.power_dbfs, -48.5, places=2)
        self.assertEqual(ev.node_id, "mock-node-1")
        # B3: ts_ns parsed from C++ ISO string, NOT receive-time
        self.assertGreater(ev.timestamp_ns, 1767225600_000_000_000)
        # GPS lifted from per-event payload
        self.assertAlmostEqual(ev.node_lat, 35.123, places=3)
        self.assertAlmostEqual(ev.node_lon, -106.456, places=3)

    async def test_decoder_event_accepted_with_protocol(self):
        """B1 — type='decoder' (the C++ canonical decoder shape) MUST flow."""
        self.server.push_decoder(773.21875e6, decoder="P25",
                                 protocol="P25 Phase 1",
                                 talkgroup="TG 31337")

        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(2.0)

        self.assertEqual(len(self.received), 1)
        ev = self.received[0]
        self.assertEqual(ev.protocol, "P25 Phase 1")
        self.assertEqual(ev.detector, "kujhad:p25")
        self.assertEqual(ev.decoded_payload, "P25 Phase 1 call")

    async def test_unknown_type_dropped_silently(self):
        """Unknown event types must NOT appear as RFEvents."""
        self.server.push_garbage()
        self.server.push_hit(100e6, -50)

        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(2.0)

        self.assertEqual(len(self.received), 1, "garbage type must be dropped")
        self.assertEqual(self.received[0].frequency, 100e6)

    async def test_since_cursor_advances(self):
        """lastId cursor — second poll only returns new events."""
        self.server.push_hit(100e6, -55)

        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(1.5)
        self.assertEqual(len(self.received), 1)

        self.server.push_hit(200e6, -60)
        await asyncio.sleep(2.0)

        self.assertEqual(len(self.received), 2)
        self.assertEqual(self.received[1].frequency, 200e6)

    async def test_tune_command_wire_shape(self):
        """B4 — tune command goes out in the C++ class+action+args shape."""
        await self.client.start(on_event=self.received.append)
        ok = await self.client.send_tune_command(146.52e6, vfo="VFO A")
        self.assertTrue(ok, "tune command should be accepted by mock")

        self.assertEqual(len(self.server.command_log), 1)
        cmd = self.server.command_log[0]
        self.assertEqual(cmd["class"], "tune")
        self.assertEqual(cmd["action"], "set")
        self.assertAlmostEqual(cmd["args"]["frequencyHz"], 146_520_000.0, places=1)
        self.assertEqual(cmd["args"]["vfo"], "VFO A")

    async def test_scan_command_wire_shape(self):
        """B5 — scan command goes out as class:scan action:start args:{...}."""
        await self.client.start(on_event=self.received.append)
        ok = await self.client.send_scan_command(420e6, 450e6, dwell_ms=750)
        self.assertTrue(ok)

        cmd = self.server.command_log[0]
        self.assertEqual(cmd["class"], "scan")
        self.assertEqual(cmd["action"], "start")
        self.assertEqual(cmd["args"]["startHz"], 420e6)
        self.assertEqual(cmd["args"]["endHz"], 450e6)
        self.assertEqual(cmd["args"]["dwellMs"], 750)

    async def test_tx_class_rejected_at_wire(self):
        """Hard RX-only — any tx.* class must be rejected (matches C++ L1165)."""
        await self.client.start(on_event=self.received.append)
        ok = await self.client._post_command({"class": "tx", "action": "send"})
        self.assertFalse(ok, "tx class must be rejected")

    async def test_command_immediately_after_start_no_race(self):
        """Architect bug — calling a command method right after start() must
        not race the poll loop's session creation. start() must guarantee
        the session is ready before returning."""
        await self.client.start(on_event=self.received.append)
        # No sleep — fire command immediately.
        ok = await self.client.send_tune_command(146.52e6)
        self.assertTrue(ok, "command immediately after start() must work")
        self.assertEqual(len(self.server.command_log), 1)

    async def test_invalid_time_falls_back_to_receive_time(self):
        """B3 fallback chain — unparseable `time` must fall back to time.time_ns(),
        not 0 / None."""
        # Hand-craft an event with a broken time string
        broken = {
            "time": "totally-not-a-date",
            "type": "hit",
            "frequency": 100e6,
            "strengthDb": -50.0,
            "gpsFix": False,
            "serial": 1,
        }
        self.server.events.append(broken)

        before_ns = time.time_ns()
        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(2.0)
        after_ns = time.time_ns()

        self.assertEqual(len(self.received), 1)
        ts = self.received[0].timestamp_ns
        self.assertGreaterEqual(ts, before_ns)
        self.assertLessEqual(ts, after_ns)

    async def test_state_mirror_populates_node_fields(self):
        """/v1/state response → node.mission_mode_active, scan_running,
        threshold_db, active_search_bands_hz, record_audio mirrored."""
        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(2.0)  # one poll cycle

        self.assertEqual(self.node.mission_mode_active, 1)
        self.assertTrue(self.node.scan_running)
        self.assertEqual(self.node.scan_status, "scanning 420-450 MHz")
        self.assertAlmostEqual(self.node.threshold_db, -65.0, places=2)
        self.assertTrue(self.node.record_audio)
        self.assertEqual(len(self.node.active_search_bands_hz), 2)
        self.assertEqual(self.node.active_search_bands_hz[0],
                         (420_000_000.0, 450_000_000.0))
        self.assertEqual(self.node.active_search_bands_hz[1],
                         (144_000_000.0, 148_000_000.0))

    async def test_capability_inference_from_identify(self):
        """/v1/identify hwProfile=hackrf → node.available_decoders includes
        every registered decoder whose declared bands overlap HackRF's
        1 MHz-6 GHz range; available_detectors includes both algorithms."""
        from backend.sensor.decoders.decoder_registry import decoder_registry
        from backend.sensor.hardware.capability_inference import _reset_cache_for_tests
        _reset_cache_for_tests()

        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(1.5)  # identify happens first thing in poll loop

        # All registered decoders MUST appear (HackRF spans VHF + UHF).
        # No decoder name may appear that isn't in the registry — that was
        # the architect-caught 'adsb' bug.
        registered = set(decoder_registry.list_decoders())
        inferred = set(self.node.available_decoders)
        self.assertTrue(inferred.issubset(registered),
                        f"inferred {inferred - registered} not in registry")
        self.assertEqual(inferred, registered,
                         "HackRF should host every registered decoder")
        # max_parallel_detectors=2 on HackRF, both detectors need 1
        self.assertIn("fft_peak", self.node.available_detectors)
        self.assertIn("energy",   self.node.available_detectors)

    async def test_identify_unauthorized_leaves_capabilities_empty(self):
        """401 from /v1/identify must NOT populate inferred capability
        lists with stale or partial data."""
        # Use a wrong API key so the mock returns 401
        bad_node = SensorNodeTrust(
            node_id="mock-node-bad",
            hardware_code="",       # empty so identify is the only source
            kujhad_host="127.0.0.1",
            kujhad_port=self.port,
            kujhad_api_key="WRONG-KEY",
        )
        bad_client = KujhadClient(bad_node)
        try:
            await bad_client.start(on_event=lambda _e: None)
            await asyncio.sleep(1.5)
            self.assertEqual(bad_node.available_decoders, [])
            self.assertEqual(bad_node.available_detectors, [])
            self.assertEqual(bad_node.hardware_code, "")
        finally:
            await bad_client.stop()

    async def test_state_malformed_does_not_crash(self):
        """Non-dict /v1/state response must be handled silently — the poll
        loop must not propagate the error and kill the client."""
        # Replace the mock's state with something the converter will reject
        self.server.state = ["not", "a", "dict"]  # type: ignore[assignment]

        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(2.0)

        # Defaults remain in place (no crash, no partial population)
        self.assertEqual(self.node.mission_mode_active, 0)
        self.assertFalse(self.node.scan_running)
        self.assertEqual(self.node.active_search_bands_hz, [])

    async def test_state_malformed_search_bands_dropped_gracefully(self):
        """A malformed entry in searchBands must be silently dropped
        without poisoning the rest of the array."""
        self.server.state = dict(self.server.state)  # detach from default
        self.server.state["searchBands"] = [
            {"startHz": 100e6, "endHz": 200e6},   # good
            "garbage-string",                       # bad, dropped
            {"startHz": "not-a-number"},            # bad, dropped
            {"startHz": 300e6, "endHz": 400e6},   # good
        ]
        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(2.0)

        self.assertEqual(len(self.node.active_search_bands_hz), 2)
        self.assertEqual(self.node.active_search_bands_hz[0], (100e6, 200e6))
        self.assertEqual(self.node.active_search_bands_hz[1], (300e6, 400e6))

    async def test_per_event_gps_missing_falls_back_to_node_gps(self):
        """When the event row has no per-event GPS, _poll_gps()-fed
        node.location_gps must populate node_lat/node_lon."""
        # Pre-seed node GPS as if /v1/gps had already been polled
        self.node.location_gps = (40.0, -111.5)
        # Event without per-event GPS fields
        self.server.events.append({
            "time": "2026-05-03T12:00:00Z",
            "type": "hit", "frequency": 100e6, "strengthDb": -50.0,
            "gpsFix": False,  # no per-event lat/lon
            "serial": 1,
        })

        await self.client.start(on_event=self.received.append)
        await asyncio.sleep(2.0)

        self.assertEqual(len(self.received), 1)
        ev = self.received[0]
        self.assertAlmostEqual(ev.node_lat, 40.0, places=3)
        self.assertAlmostEqual(ev.node_lon, -111.5, places=3)


class TestEndToEndPipeline(unittest.IsolatedAsyncioTestCase):
    """Smoke: events arriving on the wire actually create EmitterTracks
    and run through the AnomalyDetector."""

    async def asyncSetUp(self):
        self.server = MockKujhadServer()
        self.port = await self.server.start()

        from backend.fusion.track_manager import TrackManager
        from backend.intelligence.anomaly_detector import AnomalyDetector
        from backend.intelligence.rf_baseline import RFBaseline

        self.baseline = RFBaseline(learning_window_hours=24.0)
        self.anomaly = AnomalyDetector(self.baseline)
        self.tracks = TrackManager()
        self.fleet = KujhadFleetManager()
        self.new_tracks: list = []
        self.tracks.on_new_track(self.new_tracks.append)

        self._anomaly_log: list = []

        def on_event(ev: RFEvent):
            self.baseline.observe(ev)
            track = self.tracks.ingest(ev)
            flags = self.anomaly.analyze(track, ev)
            if flags:
                self._anomaly_log.extend(flags)

        self.fleet.on_event(on_event)
        node = _make_node(self.port)
        self.tracks.register_node(node)
        await self.fleet.add_node(node)

    async def asyncTearDown(self):
        await self.fleet.stop_all()
        await self.server.stop()

    async def test_three_hits_form_one_track(self):
        # Three hits at the same frequency → one EmitterTrack with 3 obs
        for i in range(3):
            self.server.push_hit(462.6125e6, strength_db=-55 + i)
            await asyncio.sleep(1.2)

        self.assertEqual(len(self.tracks.tracks), 1,
                         "three hits at same freq should fuse into one track")
        track = next(iter(self.tracks.tracks.values()))
        self.assertEqual(track.observation_count, 3)
        self.assertAlmostEqual(track.primary_frequency, 462.6125e6, places=1)
        self.assertAlmostEqual(track.last_power_dbfs, -53.0, places=1)
        self.assertGreater(len(self._anomaly_log), 0,
                           "first-ever frequency should anomaly-flag")


# ─────────────────────────────────────────────────────────────────────────────
# Standalone runner (no pytest needed)
# ─────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    unittest.main(verbosity=2)
