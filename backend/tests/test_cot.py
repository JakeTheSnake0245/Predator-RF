"""
CoTEmitter tests — XML well-formedness, UDP transport, and the two-key
operator gate (COT_ENABLED + escalate_to_atak). Uses a localhost UDP
listener instead of multicast so the tests are hermetic and don't require
a TAK server.
"""
from __future__ import annotations

import asyncio
import os
import socket
import sys
import time
import unittest
from xml.etree import ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.output.cot_emitter import CoTEmitter, build_cot_xml


def _track(emitter_id="abc1234556789", lat=None, lon=None,
           location_confidence=0.0):
    return {
        "emitter_id": emitter_id,
        "primary_frequency": 462_612_500.0,
        "observation_count": 17,
        "confidence": 0.81,
        "estimated_lat": lat,
        "estimated_lon": lon,
        "location_confidence": location_confidence,
    }


def _report(emitter_id="abc1234556789", escalate=True, level="high"):
    return {
        "emitter_id": emitter_id,
        "threat_level": level,
        "summary": "Anomalous narrowband emission near a public-safety band.",
        "escalate_to_atak": escalate,
    }


class CoTXMLTests(unittest.TestCase):
    def test_xml_is_parseable_and_has_required_attrs(self):
        xml = build_cot_xml(
            uid="PREDATOR.x", lat=35.123, lon=-106.456,
            callsign="PREDATOR-x", remarks="hello & < > \" '")
        root = ET.fromstring(xml.decode("utf-8"))
        self.assertEqual(root.tag, "event")
        self.assertEqual(root.attrib["version"], "2.0")
        self.assertEqual(root.attrib["uid"], "PREDATOR.x")
        self.assertEqual(root.attrib["type"], "a-u-G")
        for k in ("time", "start", "stale", "how"):
            self.assertIn(k, root.attrib)

        point = root.find("point")
        self.assertIsNotNone(point)
        self.assertAlmostEqual(float(point.attrib["lat"]), 35.123, places=4)
        self.assertAlmostEqual(float(point.attrib["lon"]), -106.456, places=4)

        contact = root.find("./detail/contact")
        self.assertIsNotNone(contact)
        self.assertEqual(contact.attrib["callsign"], "PREDATOR-x")

        # Special chars in remarks survive escaping + parsing round trip
        remarks = root.find("./detail/remarks")
        self.assertIsNotNone(remarks)
        self.assertEqual(remarks.text, "hello & < > \" '")

    def test_stale_is_after_start(self):
        xml = build_cot_xml(uid="u", lat=0, lon=0, stale_seconds=60.0)
        root = ET.fromstring(xml.decode("utf-8"))
        # ISO timestamps sort lexicographically, so this works
        self.assertGreater(root.attrib["stale"], root.attrib["start"])


class CoTEmitterGateTests(unittest.IsolatedAsyncioTestCase):
    async def test_disabled_emitter_never_sends(self):
        emit = CoTEmitter(enabled=False)
        try:
            sent = await emit.emit_track(
                _track(lat=35.1, lon=-106.5), _report(escalate=True))
            self.assertFalse(sent)
        finally:
            emit.close()

    async def test_enabled_but_assessment_says_no(self):
        with _udp_listener() as (host, port, _recv):
            emit = CoTEmitter(dest_host=host, dest_port=port, enabled=True)
            try:
                sent = await emit.emit_track(
                    _track(lat=35.1, lon=-106.5),
                    _report(escalate=False))
                self.assertFalse(sent,
                    "escalate_to_atak=False must suppress send")
            finally:
                emit.close()

    async def test_enabled_and_escalated_sends_parseable_cot(self):
        with _udp_listener() as (host, port, recv):
            emit = CoTEmitter(dest_host=host, dest_port=port, enabled=True)
            try:
                sent = await emit.emit_track(
                    _track(lat=35.1, lon=-106.5, location_confidence=0.8),
                    _report(escalate=True, level="critical"))
                self.assertTrue(sent)

                data = await asyncio.wait_for(recv(), timeout=2.0)
                self.assertGreater(len(data), 0)

                root = ET.fromstring(data.decode("utf-8"))
                self.assertEqual(root.tag, "event")
                # CRITICAL level should appear in remarks
                remarks = root.find("./detail/remarks").text or ""
                self.assertIn("CRITICAL", remarks)
                self.assertIn("462.6125 MHz", remarks)
            finally:
                emit.close()

    async def test_no_location_no_fallback_suppresses_send(self):
        with _udp_listener() as (host, port, _recv):
            emit = CoTEmitter(dest_host=host, dest_port=port, enabled=True)
            try:
                sent = await emit.emit_track(
                    _track(lat=None, lon=None),
                    _report(escalate=True))
                self.assertFalse(sent,
                    "no TDOA fix and no fallback → cannot place on map")
            finally:
                emit.close()

    async def test_no_location_with_fallback_uses_node_position(self):
        with _udp_listener() as (host, port, recv):
            emit = CoTEmitter(dest_host=host, dest_port=port, enabled=True)
            try:
                sent = await emit.emit_track(
                    _track(lat=None, lon=None),
                    _report(escalate=True),
                    fallback_location=(35.5, -106.6))
                self.assertTrue(sent)
                data = await asyncio.wait_for(recv(), timeout=2.0)
                root = ET.fromstring(data.decode("utf-8"))
                # Type should switch to point-of-interest when fallback used
                self.assertEqual(root.attrib["type"], "b-m-p-s-p-loc")
                point = root.find("point")
                self.assertAlmostEqual(float(point.attrib["lat"]), 35.5, places=4)
                self.assertAlmostEqual(float(point.attrib["lon"]), -106.6, places=4)
            finally:
                emit.close()

    async def test_rate_limit_per_emitter(self):
        with _udp_listener() as (host, port, _recv):
            emit = CoTEmitter(dest_host=host, dest_port=port, enabled=True)
            emit._min_interval_s = 60.0   # large window
            try:
                t = _track(emitter_id="rl1", lat=35, lon=-106)
                r = _report(emitter_id="rl1", escalate=True)
                self.assertTrue(await emit.emit_track(t, r))
                # Second send within window must be suppressed
                self.assertFalse(await emit.emit_track(t, r))
                # Different emitter is independent
                t2 = _track(emitter_id="rl2", lat=35, lon=-106)
                r2 = _report(emitter_id="rl2", escalate=True)
                self.assertTrue(await emit.emit_track(t2, r2))
            finally:
                emit.close()

    async def test_stats_reports_correctly(self):
        with _udp_listener() as (host, port, _recv):
            emit = CoTEmitter(dest_host=host, dest_port=port, enabled=True)
            try:
                await emit.emit_track(
                    _track(emitter_id="stats1", lat=1, lon=2),
                    _report(emitter_id="stats1", escalate=True))
                s = emit.stats()
                self.assertEqual(s["enabled"], True)
                self.assertEqual(s["sent"], 1)
                self.assertEqual(s["errors"], 0)
                self.assertEqual(s["tracked_emitters"], 1)
                self.assertIn(host, s["destination"])
            finally:
                emit.close()


# ── Test helper: ephemeral UDP listener ─────────────────────────────────

class _udp_listener:
    """Context manager that binds an ephemeral UDP port on 127.0.0.1 and
    yields (host, port, async_recv) where async_recv() reads one datagram."""
    def __enter__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)
        self.sock.bind(("127.0.0.1", 0))
        host, port = self.sock.getsockname()

        async def recv() -> bytes:
            loop = asyncio.get_running_loop()
            while True:
                try:
                    data, _ = self.sock.recvfrom(8192)
                    return data
                except BlockingIOError:
                    await asyncio.sleep(0.02)
        return host, port, recv

    def __exit__(self, *exc):
        try:
            self.sock.close()
        except Exception:
            pass
        return False


if __name__ == "__main__":
    unittest.main(verbosity=2)
