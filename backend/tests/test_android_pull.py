"""Snapshot endpoint tests. The endpoint is graceful-degrading by
design: missing backend / track manager / store should still produce
a usable JSON shape for the phone."""
from __future__ import annotations

import asyncio
import os
import sys
import time
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.api.routes import android_pull as ap


def _reset():
    ap.track_manager = None
    ap.fleet_manager = None
    ap.backend_ref = None
    ap._preflight_cache.update({"ts": 0.0, "go": True})


class _FakeStore:
    def __init__(self, evs):
        self._evs = evs

    async def fetch_events_since(self, *, since_ns, limit):
        return [e for e in self._evs if e["timestamp_ns"] > since_ns][:limit]

    async def latest_assessments(self):
        return {}


class _FakeQueue:
    def __init__(self, items):
        self._items = items

    def list_pending(self):
        return list(self._items)


class _FakeMissions:
    active_id = "M-001"

    def get(self, _id):
        return SimpleNamespace(name="OVERWATCH-1",
                                operator="K9-Actual",
                                started_ts_ns=1)


class _FakeTrack:
    def __init__(self, eid, last_ns, lat=None, lon=None):
        self.emitter_id = eid
        self.last_seen_ns = last_ns
        self.estimated_lat = lat
        self.estimated_lon = lon

    def to_dict(self):
        return {
            "emitter_id": self.emitter_id,
            "last_seen_ns": self.last_seen_ns,
            "estimated_lat": self.estimated_lat,
            "estimated_lon": self.estimated_lon,
            "frequency_history": [1.0, 2.0],   # should be stripped
            "power_history":     [-50.0, -49.0],
        }


class _FakeTM:
    def __init__(self, tracks):
        self.tracks = {t.emitter_id: t for t in tracks}


class _FakeNode:
    def __init__(self, **kw):
        for k, v in kw.items():
            setattr(self, k, v)


class _FakeFleet:
    def __init__(self, nodes):
        self._nodes = nodes

    def all_nodes(self):
        return list(self._nodes)


class AndroidPullSnapshot(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        _reset()

    async def test_empty_state_returns_valid_shape(self):
        snap = await ap._build_snapshot(
            since_ns=0, max_events=10, include_history=False)
        self.assertEqual(snap["schema"], 2)
        self.assertEqual(snap["tracks"], [])
        self.assertEqual(snap["events"], [])
        self.assertEqual(snap["nodes"], [])
        self.assertEqual(snap["approvals_pending"], [])
        self.assertIsNone(snap["mission"])
        self.assertIsInstance(snap["cursor"], int)

    async def test_delta_filters_old_tracks(self):
        cutoff = time.time_ns()
        old = _FakeTrack("old", cutoff - 10_000)
        new = _FakeTrack("new", cutoff + 10_000)
        ap.track_manager = _FakeTM([old, new])
        snap = await ap._build_snapshot(
            since_ns=cutoff, max_events=10, include_history=False)
        ids = [t["emitter_id"] for t in snap["tracks"]]
        self.assertEqual(ids, ["new"])

    async def test_history_stripped_by_default(self):
        ap.track_manager = _FakeTM([_FakeTrack("e1", time.time_ns() + 1)])
        snap = await ap._build_snapshot(
            since_ns=0, max_events=10, include_history=False)
        self.assertNotIn("frequency_history", snap["tracks"][0])
        self.assertNotIn("power_history", snap["tracks"][0])

    async def test_history_included_when_requested(self):
        ap.track_manager = _FakeTM([_FakeTrack("e1", time.time_ns() + 1)])
        snap = await ap._build_snapshot(
            since_ns=0, max_events=10, include_history=True)
        self.assertIn("frequency_history", snap["tracks"][0])

    async def test_events_pulled_from_store_with_cap(self):
        evs = [{"timestamp_ns": i, "node_id": "n", "frequency": 1.0,
                 "power_dbfs": -1.0} for i in range(1, 21)]
        ap.backend_ref = SimpleNamespace(
            store=_FakeStore(evs),
            approvals=_FakeQueue([]),
            missions=_FakeMissions(),
        )
        snap = await ap._build_snapshot(
            since_ns=5, max_events=3, include_history=False)
        # since_ns=5 → events with timestamp > 5; max_events=3 → first 3.
        self.assertEqual(len(snap["events"]), 3)
        self.assertEqual(snap["events"][0]["timestamp_ns"], 6)

    async def test_approvals_always_full(self):
        ap.backend_ref = SimpleNamespace(
            store=_FakeStore([]),
            approvals=_FakeQueue([{"approval_id": "A1"}, {"approval_id": "A2"}]),
            missions=_FakeMissions(),
        )
        snap = await ap._build_snapshot(
            since_ns=time.time_ns(), max_events=1, include_history=False)
        self.assertEqual(len(snap["approvals_pending"]), 2)

    async def test_nodes_always_full(self):
        ap.fleet_manager = _FakeFleet([
            _FakeNode(node_id="alpha", trust_score=0.9, gps_lock=True,
                      gps_age_s=1.2, lat=38.0, lon=-77.0,
                      hardware_code="hackrf", last_seen_ns=1),
            _FakeNode(node_id="bravo", trust_score=0.8, gps_lock=False,
                      gps_age_s=99.0, lat=None, lon=None,
                      hardware_code="rtlsdr", last_seen_ns=1),
        ])
        snap = await ap._build_snapshot(
            since_ns=0, max_events=1, include_history=False)
        self.assertEqual(len(snap["nodes"]), 2)
        self.assertEqual(snap["nodes"][0]["node_id"], "alpha")

    async def test_mission_present_when_active(self):
        ap.backend_ref = SimpleNamespace(
            store=_FakeStore([]),
            approvals=_FakeQueue([]),
            missions=_FakeMissions(),
        )
        snap = await ap._build_snapshot(
            since_ns=0, max_events=1, include_history=False)
        self.assertIsNotNone(snap["mission"])
        self.assertEqual(snap["mission"]["mission_id"], "M-001")
        self.assertEqual(snap["mission"]["operator"], "K9-Actual")

    async def test_subsystem_failure_is_isolated(self):
        # A blowing-up store must not take down the whole snapshot.
        class _Boom:
            async def fetch_events_since(self, **_kw):
                raise RuntimeError("disk full")
        ap.backend_ref = SimpleNamespace(
            store=_Boom(), approvals=_FakeQueue([]), missions=_FakeMissions())
        snap = await ap._build_snapshot(
            since_ns=0, max_events=1, include_history=False)
        self.assertEqual(snap["events"], [])
        self.assertEqual(snap["schema"], 2)


class CoTExportXmlBuilder(unittest.TestCase):
    def test_envelope_wraps_multiple_events(self):
        from backend.api.routes.cot_export import _multi_event_envelope
        body = _multi_event_envelope([
            b'<?xml version="1.0"?><event version="2.0"/>',
            b'<event version="2.0"/>',
        ])
        self.assertIn(b"<events>", body)
        self.assertIn(b"</events>", body)
        # Per-event xml prolog should be stripped from inside the envelope.
        self.assertEqual(body.count(b"<?xml"), 1)
        # Two <event ...> child elements + the wrapping <events> root
        # → "<event" substring appears 3 times. Count event-with-version
        # to disambiguate from the root.
        self.assertEqual(body.count(b'<event version='), 2)
        self.assertEqual(body.count(b"<events>"), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
