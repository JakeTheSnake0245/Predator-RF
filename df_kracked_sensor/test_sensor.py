"""Unit tests for the DF-Kracked LOB sensor.

Covers:
  * doa_result → KRAKEN_LOB event-row conversion (shape + raw schema)
  * /v1/events?since= serial cursor semantics
  * X-Kujhad-Key auth rejection on /v1/*

Run: python3 -m pytest df_kracked_sensor/test_sensor.py
 or: python3 -m unittest df_kracked_sensor.test_sensor
"""
import asyncio
import json
import os
import stat
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sensor  # noqa: E402
from sensor import (  # noqa: E402
    EventRing,
    KrakenIngester,
    KujhadSensorApp,
    NodePosition,
    doa_result_to_event_row,
)


def make_doa(**over):
    base = {
        "type": "doa_result",
        "freq_hz": 433920000,
        "bearing_deg": 127.5,
        "bearing_std_deg": 5.2,
        "confidence": 0.83,
        "power_dbfs": -42.1,
        "snr_db": 12.3,
        "gps_lat": 37.4,
        "gps_lon": -122.1,
        "heading_deg": 0.0,
        "timestamp_unix": 1718035200.123,
        "node_id": "kraken-0",
    }
    base.update(over)
    return base


class TestConversion(unittest.TestCase):
    def test_full_row_shape(self):
        row = doa_result_to_event_row(
            make_doa(), serial=7, node_id="fallback", source_label="Sensor:x",
            fallback_lat=0.0, fallback_lon=0.0, fallback_heading=0.0,
            have_fix=False)
        self.assertIsNotNone(row)
        # Top-level fields the controller expects.
        self.assertEqual(row["type"], "decoder")
        self.assertEqual(row["decoder"], "KRAKEN_LOB")
        self.assertEqual(row["hitState"], "decoded")
        self.assertEqual(row["protocol"], "DOA")
        self.assertEqual(row["frequency"], 433920000)
        self.assertEqual(row["talkgroup"], "Unknown")
        self.assertEqual(row["radioId"], "Unknown")
        self.assertFalse(row["hasAudio"])
        self.assertTrue(row["hasData"])
        self.assertEqual(row["source"], "Sensor:x")
        self.assertTrue(row["gpsFix"])
        self.assertEqual(row["lat"], 37.4)
        self.assertEqual(row["lon"], -122.1)
        self.assertEqual(row["serial"], 7)
        self.assertIn("eventId", row)
        # time must be a formatted local-time STRING (controller reads it
        # via readJsonString), not an int epoch.
        self.assertIsInstance(row["time"], str)
        self.assertRegex(row["time"], r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$")
        # machine-readable time stays in raw.timestamp_unix
        self.assertAlmostEqual(row["raw"]["timestamp_unix"], 1718035200.123)

    def test_raw_schema_load_bearing_keys(self):
        row = doa_result_to_event_row(
            make_doa(), serial=1, node_id="n", source_label="s",
            fallback_lat=0.0, fallback_lon=0.0, fallback_heading=0.0,
            have_fix=False)
        raw = row["raw"]
        # These keys are read by the controller aggregator.
        self.assertAlmostEqual(raw["bearing_deg"], 127.5)
        self.assertAlmostEqual(raw["gps_lat"], 37.4)
        self.assertAlmostEqual(raw["gps_lon"], -122.1)
        self.assertAlmostEqual(raw["timestamp_unix"], 1718035200.123)
        self.assertAlmostEqual(raw["bearing_std_deg"], 5.2)
        self.assertAlmostEqual(raw["confidence"], 0.83)
        self.assertAlmostEqual(raw["freq_hz"], 433920000)
        self.assertAlmostEqual(raw["heading_deg"], 0.0)
        self.assertEqual(raw["node_id"], "kraken-0")

    def test_native_aliases(self):
        # doa_max_deg + frequency_hz + doa_std_deg (native producer form)
        msg = {
            "type": "doa_result",
            "doa_max_deg": 200.0,
            "frequency_hz": 100000000,
            "doa_std_deg": 3.3,
            "gps_lat": 1.0, "gps_lon": 2.0,
        }
        row = doa_result_to_event_row(
            msg, 1, "n", "s", 0.0, 0.0, 0.0, False)
        self.assertAlmostEqual(row["raw"]["bearing_deg"], 200.0)
        self.assertAlmostEqual(row["frequency"], 100000000)
        self.assertAlmostEqual(row["raw"]["bearing_std_deg"], 3.3)

    def test_reject_non_doa(self):
        self.assertIsNone(doa_result_to_event_row(
            {"type": "status"}, 1, "n", "s", 0, 0, 0, False))

    def test_reject_bad_bearing(self):
        self.assertIsNone(doa_result_to_event_row(
            make_doa(bearing_deg=400.0), 1, "n", "s", 0, 0, 0, False))

    def test_fallback_position_used(self):
        msg = make_doa()
        del msg["gps_lat"]
        del msg["gps_lon"]
        row = doa_result_to_event_row(
            msg, 1, "n", "s", 51.5, -0.12, 45.0, have_fix=True)
        self.assertEqual(row["lat"], 51.5)
        self.assertEqual(row["lon"], -0.12)

    def test_reject_no_position_no_fix(self):
        msg = make_doa()
        del msg["gps_lat"]
        del msg["gps_lon"]
        self.assertIsNone(doa_result_to_event_row(
            msg, 1, "n", "s", 0, 0, 0, have_fix=False))

    def test_reject_null_island(self):
        self.assertIsNone(doa_result_to_event_row(
            make_doa(gps_lat=0.0, gps_lon=0.0), 1, "n", "s", 0, 0, 0, False))


class TestEventRing(unittest.TestCase):
    def _row(self, ring):
        s = ring.next_serial()
        row = doa_result_to_event_row(
            make_doa(), s, "n", "s", 0, 0, 0, False)
        ring.append(row)
        return s

    def test_serial_monotonic(self):
        ring = EventRing()
        s1 = ring.next_serial()
        s2 = ring.next_serial()
        self.assertEqual(s2, s1 + 1)

    def test_since_cursor(self):
        ring = EventRing()
        for _ in range(5):
            self._row(ring)
        events, last_id = ring.since(0)
        self.assertEqual(len(events), 5)
        self.assertEqual(last_id, 5)
        # since=3 → only serials 4,5
        events, last_id = ring.since(3)
        self.assertEqual([e["serial"] for e in events], [4, 5])
        self.assertEqual(last_id, 5)
        # since at head → nothing new, lastId stays at since
        events, last_id = ring.since(5)
        self.assertEqual(events, [])
        self.assertEqual(last_id, 5)

    def test_since_oldest_first(self):
        ring = EventRing()
        for _ in range(3):
            self._row(ring)
        events, _ = ring.since(0)
        serials = [e["serial"] for e in events]
        self.assertEqual(serials, sorted(serials))

    def test_ring_bounded(self):
        ring = EventRing(maxlen=3)
        for _ in range(5):
            self._row(ring)
        events, last_id = ring.since(0)
        self.assertEqual(len(events), 3)          # only last 3 retained
        self.assertEqual([e["serial"] for e in events], [3, 4, 5])
        self.assertEqual(last_id, 5)


class TestThrottleDedup(unittest.TestCase):
    def test_dedup_identical_bearing(self):
        ring = EventRing()
        pos = NodePosition(0, 0, 0, False)
        ing = KrakenIngester("ws://x/ws", ring, pos, "n", "s")
        r1 = ing.handle_message(json.dumps(make_doa(bearing_deg=90.0,
                                                    gps_lat=1.0, gps_lon=2.0)))
        self.assertIsNotNone(r1)
        # Immediate identical bearing → coalesced (None).
        r2 = ing.handle_message(json.dumps(make_doa(bearing_deg=90.0,
                                                    gps_lat=1.0, gps_lon=2.0)))
        self.assertIsNone(r2)

    def test_rate_cap(self):
        ring = EventRing()
        pos = NodePosition(0, 0, 0, False)
        ing = KrakenIngester("ws://x/ws", ring, pos, "n", "s")
        ing.handle_message(json.dumps(make_doa(bearing_deg=10.0,
                                               gps_lat=1.0, gps_lon=2.0)))
        # Different bearing but within the rate window → suppressed.
        r = ing.handle_message(json.dumps(make_doa(bearing_deg=200.0,
                                                   gps_lat=1.0, gps_lon=2.0)))
        self.assertIsNone(r)


# ── aiohttp server auth + endpoints ─────────────────────────────────────────

@unittest.skipUnless(os.name == "posix", "POSIX file-mode semantics required")
class TestKeyFilePermissions(unittest.TestCase):
    def test_created_key_file_is_0600(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "df_kracked_sensor.json")
            key = sensor.load_or_create_key(path, None)
            self.assertTrue(key)
            self.assertTrue(os.path.isfile(path))
            mode = stat.S_IMODE(os.stat(path).st_mode)
            self.assertEqual(
                mode, 0o600,
                f"expected 0600, got {oct(mode)} — key file must be owner-only")

    def test_provided_key_file_is_0600(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "df_kracked_sensor.json")
            sensor.load_or_create_key(path, "operatorkey")
            mode = stat.S_IMODE(os.stat(path).st_mode)
            self.assertEqual(mode, 0o600, oct(mode))

    def test_loose_existing_file_is_tightened_on_load(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "df_kracked_sensor.json")
            # Simulate an older, world-readable file.
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"api_key": "legacy"}, f)
            os.chmod(path, 0o644)
            key = sensor.load_or_create_key(path, None)
            self.assertEqual(key, "legacy")
            mode = stat.S_IMODE(os.stat(path).st_mode)
            self.assertEqual(mode, 0o600, oct(mode))


from aiohttp.test_utils import AioHTTPTestCase  # noqa: E402


class TestServer(AioHTTPTestCase):
    async def get_application(self):
        self.ring = EventRing()
        pos = NodePosition(37.0, -122.0, 0.0, True)
        self.ing = KrakenIngester("ws://x/ws", self.ring, pos, "kraken-0", "Sensor:t")
        self.app_obj = KujhadSensorApp("SECRETKEY", "df-test", self.ring, pos,
                                       self.ing)
        return self.app_obj.build()

    async def _seed(self, n):
        for _ in range(n):
            s = self.ring.next_serial()
            self.ring.append(doa_result_to_event_row(
                make_doa(), s, "kraken-0", "Sensor:t", 0, 0, 0, False))

    async def test_identify_ok(self):
        resp = await self.client.get("/v1/identify",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        self.assertEqual(resp.status, 200)
        body = await resp.json()
        self.assertEqual(body["role"], "sensor")
        self.assertEqual(body["device"], "df-test")
        self.assertIn("hwProfile", body)
        self.assertEqual(set(["device", "version", "role"]) - set(body), set())

    async def test_auth_rejected_missing_key(self):
        for path in ("/v1/identify", "/v1/state", "/v1/gps", "/v1/events"):
            resp = await self.client.get(path)
            self.assertEqual(resp.status, 401, path)
            body = await resp.json()
            self.assertEqual(body["error"], "unauthorized")

    async def test_auth_rejected_wrong_key(self):
        resp = await self.client.get("/v1/state",
                                     headers={"X-Kujhad-Key": "WRONG"})
        self.assertEqual(resp.status, 401)

    async def test_state_shape(self):
        resp = await self.client.get("/v1/state",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        self.assertEqual(resp.status, 200)
        body = await resp.json()
        for key in ("scanStatus", "searchBands", "targets", "hits"):
            self.assertIn(key, body)
        self.assertEqual(body["searchBands"], [])

    async def test_events_since_cursor(self):
        await self._seed(3)
        resp = await self.client.get("/v1/events?since=0",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        body = await resp.json()
        self.assertEqual(body["lastId"], 3)
        self.assertEqual(len(body["events"]), 3)
        self.assertEqual(body["events"][0]["decoder"], "KRAKEN_LOB")

        resp = await self.client.get("/v1/events?since=2",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        body = await resp.json()
        self.assertEqual([e["serial"] for e in body["events"]], [3])
        self.assertEqual(body["lastId"], 3)

    async def test_command_graceful_501(self):
        resp = await self.client.post(
            "/v1/command", headers={"X-Kujhad-Key": "SECRETKEY"},
            data=json.dumps({"class": "tune", "action": "set"}))
        self.assertEqual(resp.status, 501)
        body = await resp.json()
        self.assertFalse(body["ok"])

    async def test_root_public(self):
        resp = await self.client.get("/")
        self.assertEqual(resp.status, 200)


if __name__ == "__main__":
    unittest.main()
