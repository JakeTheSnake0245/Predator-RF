"""Coordinator failure recovery — mission-store snapshot export and
backup bootstrap. Verifies:

  * snapshot_to_file / snapshot_to_bytes produce a valid, consistent
    SQLite DB (correct schema version, rows intact) while the source
    store stays open;
  * a MissionStore started from a snapshot rehydrates tracks and
    replaying overlapping events/tracks does NOT duplicate rows
    (INSERT OR IGNORE on event_id, upsert on emitter_id);
  * the standby preflight check (check_backup_readiness) gates on
    FLEET_NODES presence and snapshot freshness.
"""
from __future__ import annotations

import asyncio
import os
import sqlite3
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.persistence.store import MissionStore, SCHEMA_VERSION
from deploy.preflight import check_backup_readiness


def _event(i: int) -> dict:
    return {"event_id": f"ev-{i}", "timestamp_ns": 1_000 + i,
            "node_id": "alpha", "frequency": 433.9e6,
            "power_dbfs": -40.0, "snr_db": 12.0}


def _track(i: int, obs: int = 1) -> dict:
    return {"emitter_id": f"em-{i}", "state": "active",
            "primary_frequency": 433.9e6,
            "first_seen_ns": time.time_ns(),
            "last_seen_ns": time.time_ns(),
            "observation_count": obs}


class SnapshotExportTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = MissionStore(os.path.join(self.dir.name, "mission.db"))
        for i in range(5):
            await self.store.record_event(_event(i))
        for i in range(3):
            await self.store.record_track(_track(i))

    async def asyncTearDown(self):
        self.store.close()
        self.dir.cleanup()

    async def test_snapshot_to_file_is_valid_sqlite_at_schema_v2(self):
        snap = os.path.join(self.dir.name, "snap.db")
        await self.store.snapshot_to_file(snap)
        conn = sqlite3.connect(snap)
        try:
            ver = conn.execute("PRAGMA user_version").fetchone()[0]
            self.assertEqual(ver, SCHEMA_VERSION)
            self.assertEqual(conn.execute(
                "SELECT COUNT(*) FROM rf_events").fetchone()[0], 5)
            self.assertEqual(conn.execute(
                "SELECT COUNT(*) FROM emitter_tracks").fetchone()[0], 3)
        finally:
            conn.close()

    async def test_snapshot_to_file_overwrites_existing(self):
        snap = os.path.join(self.dir.name, "snap.db")
        with open(snap, "w") as f:
            f.write("stale")
        await self.store.snapshot_to_file(snap)  # must not raise
        with open(snap, "rb") as f:
            self.assertTrue(f.read(16).startswith(b"SQLite format 3"))

    async def test_snapshot_to_bytes_round_trips(self):
        blob = await self.store.snapshot_to_bytes()
        self.assertTrue(blob.startswith(b"SQLite format 3"))
        # Write it out and open it as a store — the exact backup
        # bootstrap path a standby kit performs.
        restored_path = os.path.join(self.dir.name, "restored.db")
        with open(restored_path, "wb") as f:
            f.write(blob)
        restored = MissionStore(restored_path)
        try:
            self.assertEqual(restored.event_count(), 5)
            self.assertEqual(restored.track_count(), 3)
            tracks = restored.load_active_tracks()
            self.assertEqual({t["emitter_id"] for t in tracks},
                             {"em-0", "em-1", "em-2"})
        finally:
            restored.close()

    async def test_source_store_still_writable_after_snapshot(self):
        await self.store.snapshot_to_bytes()
        await self.store.record_event(_event(99))
        self.assertEqual(self.store.event_count(), 6)

    async def test_replay_after_restore_does_not_duplicate(self):
        """A promoted backup re-polls node rings; events/tracks the
        snapshot already contained get re-ingested. Rows must not
        duplicate."""
        blob = await self.store.snapshot_to_bytes()
        restored_path = os.path.join(self.dir.name, "restored.db")
        with open(restored_path, "wb") as f:
            f.write(blob)
        restored = MissionStore(restored_path)
        try:
            # Replay the same 5 events and 3 tracks, plus one new each.
            for i in range(5):
                await restored.record_event(_event(i))
            await restored.record_event(_event(5))
            for i in range(3):
                await restored.record_track(_track(i, obs=7))
            await restored.record_track(_track(3))
            self.assertEqual(restored.event_count(), 6)   # 5 old + 1 new
            self.assertEqual(restored.track_count(), 4)   # 3 old + 1 new
            # Upsert semantics: replayed track updated in place.
            tracks = {t["emitter_id"]: t
                      for t in restored.load_active_tracks()}
            self.assertEqual(tracks["em-0"]["observation_count"], 7)
        finally:
            restored.close()


class BackupReadinessCheckTests(unittest.TestCase):
    def _make_snapshot(self, d: str, age_s: float) -> str:
        p = os.path.join(d, "mission-snapshot-20260718T000000Z.db")
        with open(p, "wb") as f:
            f.write(b"SQLite format 3\x00")
        old = time.time() - age_s
        os.utime(p, (old, old))
        return p

    def test_non_standby_no_snapshots_passes(self):
        with tempfile.TemporaryDirectory() as d:
            r = check_backup_readiness("a@1.2.3.4:5259",
                                       os.path.join(d, "missing"),
                                       standby=False)
            self.assertEqual(r["severity"], "PASS")

    def test_non_standby_stale_snapshot_warns(self):
        with tempfile.TemporaryDirectory() as d:
            self._make_snapshot(d, age_s=7200)
            r = check_backup_readiness("a@1.2.3.4:5259", d,
                                       standby=False, max_age_s=3600)
            self.assertEqual(r["severity"], "WARN")

    def test_standby_missing_snapshot_fails(self):
        with tempfile.TemporaryDirectory() as d:
            r = check_backup_readiness("a@1.2.3.4:5259", d, standby=True)
            self.assertEqual(r["severity"], "FAIL")
            self.assertIn("no mission-snapshot", r["message"])

    def test_standby_no_fleet_nodes_fails(self):
        with tempfile.TemporaryDirectory() as d:
            self._make_snapshot(d, age_s=60)
            r = check_backup_readiness("", d, standby=True)
            self.assertEqual(r["severity"], "FAIL")
            self.assertIn("FLEET_NODES", r["message"])

    def test_standby_stale_snapshot_fails(self):
        with tempfile.TemporaryDirectory() as d:
            self._make_snapshot(d, age_s=7200)
            r = check_backup_readiness("a@1.2.3.4:5259", d,
                                       standby=True, max_age_s=3600)
            self.assertEqual(r["severity"], "FAIL")
            self.assertIn("stale", r["message"])

    def test_standby_ready_passes(self):
        with tempfile.TemporaryDirectory() as d:
            self._make_snapshot(d, age_s=60)
            r = check_backup_readiness("a@1.2.3.4:5259", d,
                                       standby=True, max_age_s=3600)
            self.assertEqual(r["severity"], "PASS")


if __name__ == "__main__":
    unittest.main(verbosity=2)
