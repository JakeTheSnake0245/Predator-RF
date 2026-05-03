"""
MissionStore tests — schema migration, append/upsert behaviour, and the
restart-replay round trip that the orchestrator depends on for crash
recovery.

These run on stdlib only (sqlite3) so they don't need numpy / aiohttp /
fastapi installed. They DO exercise asyncio.to_thread for the write path
to mirror how PredatorBackend uses the store in production.
"""
from __future__ import annotations

import asyncio
import os
import sys
import tempfile
import time
import unittest

# Path so 'backend.*' imports resolve when run directly
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from backend.persistence.store import MissionStore, SCHEMA_VERSION


def _ev(node_id="n1", freq=462_612_500.0, ts=None) -> dict:
    return {
        "event_id": f"ev-{ts or time.time_ns()}-{node_id}-{freq}",
        "timestamp_ns": ts or time.time_ns(),
        "node_id": node_id,
        "frequency": freq,
        "power_dbfs": -55.0,
        "snr_db": 18.0,
        "bandwidth_hz": 12500.0,
        "detector": "fft_peak",
        "modulation": "fm",
        "protocol": None,
        "node_lat": 35.123,
        "node_lon": -106.456,
        "node_alt_m": 1500.0,
        "node_trust_score": 0.85,
    }


def _tr(emitter_id="tr-1", freq=462_612_500.0, last_seen=None) -> dict:
    now = last_seen or time.time_ns()
    return {
        "emitter_id": emitter_id,
        "state": "tracking",
        "primary_frequency": freq,
        "last_power_dbfs": -55.0,
        "first_seen_ns": now - 30 * 10**9,
        "last_seen_ns": now,
        "observation_count": 12,
        "confidence": 0.78,
        "threat_level": "low",
        "modulation": "fm",
        "protocol": None,
        "estimated_lat": 35.20,
        "estimated_lon": -106.50,
        "location_confidence": 0.62,
        "detecting_nodes": ["n1", "n2"],
        "anomaly_flags": [],
    }


def _ar(emitter_id="tr-1", level="medium") -> dict:
    return {
        "emitter_id": emitter_id,
        "assessment_ns": time.time_ns(),
        "threat_level": level,
        "confidence": 0.7,
        "summary": "test assessment",
        "recommended_action": "deep_stare",
        "recommended_nodes": ["n2"],
        "escalate_to_atak": level in ("high", "critical"),
        "anomaly_flags": ["frequency_jump"] if level != "unknown" else [],
    }


class MissionStoreTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db_path = os.path.join(self.tmp.name, "mission.db")
        self.store = MissionStore(self.db_path)

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    async def test_schema_migrated_on_first_open(self):
        cur = self.store._conn.execute("PRAGMA user_version")
        self.assertEqual(cur.fetchone()[0], SCHEMA_VERSION)
        # All three tables exist
        for table in ("rf_events", "emitter_tracks", "assessment_reports"):
            cur = self.store._conn.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name=?",
                (table,))
            self.assertIsNotNone(cur.fetchone(), f"table {table} missing")

    async def test_event_append_idempotent(self):
        e = _ev()
        await self.store.record_event(e)
        await self.store.record_event(e)        # duplicate event_id
        self.assertEqual(self.store.event_count(), 1,
                         "event with same event_id must not duplicate")

    async def test_track_upsert_keeps_one_row_per_emitter(self):
        await self.store.record_track(_tr(emitter_id="abc"))
        # observation_count grows as more events arrive
        upd = _tr(emitter_id="abc")
        upd["observation_count"] = 99
        upd["confidence"] = 0.95
        await self.store.record_track(upd)

        self.assertEqual(self.store.track_count(), 1)
        cur = self.store._conn.execute(
            "SELECT observation_count, confidence FROM emitter_tracks WHERE emitter_id='abc'")
        row = cur.fetchone()
        self.assertEqual(row["observation_count"], 99)
        self.assertAlmostEqual(row["confidence"], 0.95, places=2)

    async def test_assessment_append_only(self):
        await self.store.record_assessment(_ar(level="medium"))
        await self.store.record_assessment(_ar(level="high"))
        self.assertEqual(self.store.assessment_count(), 2,
                         "assessments are append-only history")

    async def test_restart_replay_round_trip(self):
        """The killer test: write a track, close, reopen a fresh
        MissionStore on the same DB file, and prove load_active_tracks
        returns the persisted state."""
        recent = time.time_ns()
        await self.store.record_track(_tr(emitter_id="survivor", last_seen=recent))
        await self.store.record_event(_ev(ts=recent))

        # Simulate a crash + restart by closing and reopening
        self.store.close()
        store2 = MissionStore(self.db_path)
        try:
            self.assertEqual(store2.event_count(), 1)
            self.assertEqual(store2.track_count(), 1)

            rows = store2.load_active_tracks(window_s=3600.0)
            self.assertEqual(len(rows), 1)
            r = rows[0]
            self.assertEqual(r["emitter_id"], "survivor")
            self.assertEqual(r["state"], "tracking")
            self.assertEqual(r["detecting_nodes"], ["n1", "n2"])
            self.assertAlmostEqual(r["estimated_lat"], 35.20, places=4)
        finally:
            store2.close()
        # Re-open original ref so tearDown's close() is a no-op
        self.store = MissionStore(self.db_path)

    async def test_replay_window_excludes_old_tracks(self):
        old_ns = time.time_ns() - 48 * 3600 * 10**9   # 48 h ago
        await self.store.record_track(_tr(emitter_id="ancient", last_seen=old_ns))
        await self.store.record_track(_tr(emitter_id="fresh", last_seen=time.time_ns()))

        rows = self.store.load_active_tracks(window_s=24 * 3600.0)
        ids = {r["emitter_id"] for r in rows}
        self.assertIn("fresh", ids)
        self.assertNotIn("ancient", ids,
                         "tracks beyond replay window must not rehydrate")

    async def test_replay_excludes_lost_state(self):
        recent = time.time_ns()
        lost = _tr(emitter_id="lost-track", last_seen=recent)
        lost["state"] = "lost"
        await self.store.record_track(lost)

        rows = self.store.load_active_tracks(window_s=3600.0)
        self.assertEqual(rows, [],
                         "tracks in 'lost' state must not be rehydrated")

    async def test_concurrent_writes_dont_deadlock(self):
        """All three write methods called from many coroutines at once
        must complete without deadlocking the asyncio loop or SQLite."""
        async def burst():
            for i in range(20):
                await self.store.record_event(_ev(ts=time.time_ns() + i))
                await self.store.record_track(_tr(emitter_id=f"e{i}"))
                await self.store.record_assessment(_ar(emitter_id=f"e{i}"))
        await asyncio.wait_for(
            asyncio.gather(burst(), burst(), burst()),
            timeout=10.0)
        self.assertEqual(self.store.event_count(), 60)
        self.assertEqual(self.store.track_count(), 20)
        self.assertEqual(self.store.assessment_count(), 60)

    async def test_partial_write_crash_safety_via_wal(self):
        """WAL mode must keep the DB readable after an abrupt close
        mid-transaction (simulated by closing without commit since
        isolation_level=None autocommits each statement)."""
        for i in range(50):
            await self.store.record_event(_ev(ts=time.time_ns() + i))
        # Hard-close
        self.store.close()
        # Reopen — every event up to the close must be present
        store2 = MissionStore(self.db_path)
        try:
            self.assertEqual(store2.event_count(), 50)
        finally:
            store2.close()
        self.store = MissionStore(self.db_path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
