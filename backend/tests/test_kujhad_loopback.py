"""End-to-end loopback test: stdlib HTTP server impersonates a Kujhad
node and KujhadClient (when aiohttp is present) drives /v1/identify,
/v1/gps, /v1/timing, /v1/events. Skips when aiohttp is missing — the
test target is a CI runner that has it installed."""
from __future__ import annotations

import asyncio
import json
import os
import socket
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

try:
    import aiohttp  # noqa: F401
    _HAVE_AIOHTTP = True
except ImportError:
    _HAVE_AIOHTTP = False


# ── Stdlib stub Kujhad node ─────────────────────────────────────────
class _StubHandler(BaseHTTPRequestHandler):
    """Implements just enough of the /v1/* surface to drive an end-to-
    end test of KujhadClient + RFEvent ingest."""

    def log_message(self, *_):  # quiet
        return

    def _ok(self, payload):
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/v1/identify":
            return self._ok({
                "device": "stub-kujhad", "version": "test",
                "role": "sensor", "hwProfile": {"hardware": "hackrf"}})
        if self.path == "/v1/gps":
            return self._ok({"hasFix": True, "lat": 35.1, "lon": -106.5,
                              "accuracy": 5.0})
        if self.path == "/v1/state":
            return self._ok({"missionMode": 0, "scanRunning": False,
                              "thresholdDb": -50.0, "searchBands": []})
        if self.path == "/v1/timing":
            return self._ok({"source": "gpsdo", "ppsLock": True,
                              "lastSyncSec": 5.0, "offsetMs": 2.0,
                              "driftPpm": 0.1})
        if self.path.startswith("/v1/events"):
            return self._ok({"events": [{
                "type": "hit", "frequency": 462.5e6,
                "strengthDb": -45.0, "snrDb": 18.0,
                "label": "stub-hit", "decoder": "FMNR",
                "ts_ns": time.time_ns(),
                "gpsFix": True, "lat": 35.1, "lon": -106.5,
            }], "lastId": 1})
        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        if self.path == "/v1/command":
            return self._ok({"ok": True})
        self.send_response(404)
        self.end_headers()


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


@unittest.skipUnless(_HAVE_AIOHTTP, "aiohttp not installed in this env")
class KujhadLoopbackTests(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = _free_port()
        cls.server = HTTPServer(("127.0.0.1", cls.port), _StubHandler)
        cls.thread = threading.Thread(
            target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    async def test_full_event_cycle(self):
        from backend.coordination.kujhad_client import KujhadClient
        from backend.models.sensor_node import SensorNodeTrust

        node = SensorNodeTrust(
            node_id="stub", hardware_code="rtlsdr",
            kujhad_host="127.0.0.1", kujhad_port=self.port,
            kujhad_api_key="x")
        events = []
        client = KujhadClient(node)
        await client.start(on_event=events.append)
        # Give the poll loop one cycle to fire identify+gps+events+timing
        await asyncio.sleep(1.5)
        await client.stop()

        # /v1/identify upgraded the hardware code
        self.assertEqual(node.hardware_code, "hackrf")
        # /v1/gps populated the position with a fresh timestamp
        self.assertEqual(node.location_gps, (35.1, -106.5))
        self.assertGreater(node.location_gps_updated_ns, 0)
        # /v1/timing populated the trust factor (gpsdo+pps+offset<10)
        self.assertGreaterEqual(node.timing_stability_trust, 0.9)
        self.assertEqual(node.timing_source, "gpsdo")
        self.assertTrue(node.timing_pps_lock)
        # /v1/events delivered an RFEvent through the on_event hook
        self.assertGreaterEqual(len(events), 1)
        ev = events[0]
        self.assertAlmostEqual(ev.frequency, 462.5e6)
        self.assertEqual(ev.node_id, "stub")
        self.assertEqual(ev.node_lat, 35.1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
