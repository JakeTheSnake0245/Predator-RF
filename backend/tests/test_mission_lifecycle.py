"""MissionRegistry + schema-v2 mission_id wiring + export."""
import asyncio
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.persistence.store import MissionStore
from backend.operator.missions import MissionRegistry


def _ev(em="em-1"):
    import uuid, time
    return {"event_id": str(uuid.uuid4()), "timestamp_ns": time.time_ns(),
            "node_id": "n1", "frequency": 462e6, "power_dbfs": -30.0,
            "snr_db": 10.0}


def _tr(em="em-1"):
    import time
    return {"emitter_id": em, "state": "tracking", "primary_frequency": 462e6,
            "first_seen_ns": time.time_ns(), "last_seen_ns": time.time_ns(),
            "observation_count": 1}


class MissionLifecycleTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.db = os.path.join(self.tmp, "m.db")
        self.store = MissionStore(self.db)

    def tearDown(self):
        self.store.close()

    async def test_start_tags_subsequent_events_with_mission_id(self):
        reg = MissionRegistry(store=self.store)
        m = await reg.start("test-mission")
        self.store.set_mission_provider(lambda: reg.active_id)
        await self.store.record_event(_ev())
        await self.store.record_track(_tr())
        # Read back
        cur = self.store._conn.execute(
            "SELECT mission_id FROM rf_events")
        row = cur.fetchone()
        self.assertEqual(row["mission_id"], m.mission_id)

    async def test_starting_a_new_mission_auto_ends_the_previous(self):
        reg = MissionRegistry(store=self.store)
        m1 = await reg.start("first")
        m2 = await reg.start("second")
        self.assertNotEqual(m1.mission_id, m2.mission_id)
        # m1 is now ended
        rows = reg.list_missions()
        first = [r for r in rows if r["mission_id"] == m1.mission_id][0]
        self.assertIsNotNone(first["ended_ns"])
        self.assertEqual(reg.active_id, m2.mission_id)

    async def test_explicit_end(self):
        reg = MissionRegistry(store=self.store)
        await reg.start("m1")
        ended = await reg.end()
        self.assertIsNotNone(ended)
        self.assertIsNone(reg.active)
        # End-with-no-active returns None
        self.assertIsNone(await reg.end())

    async def test_active_mission_survives_restart(self):
        reg = MissionRegistry(store=self.store)
        m = await reg.start("survives")
        # Simulate restart by closing+reopening + new registry
        self.store.close()
        self.store = MissionStore(self.db)
        reg2 = MissionRegistry(store=self.store)
        self.assertEqual(reg2.active_id, m.mission_id)

    async def test_export_mission_bundles_rows(self):
        reg = MissionRegistry(store=self.store)
        m = await reg.start("export-me")
        self.store.set_mission_provider(lambda: reg.active_id)
        await self.store.record_event(_ev())
        await self.store.record_event(_ev())
        await self.store.record_track(_tr())
        bundle = self.store.export_mission(m.mission_id)
        self.assertEqual(bundle["mission"]["mission_id"], m.mission_id)
        self.assertEqual(len(bundle["events"]), 2)
        self.assertEqual(len(bundle["tracks"]), 1)


class SchemaMigrationTests(unittest.TestCase):
    def test_v0_db_migrates_to_v2(self):
        """Open a fresh DB (no user_version), confirm both migrations
        run and end state is v2 with all the new tables."""
        tmp = tempfile.mkdtemp()
        db = os.path.join(tmp, "fresh.db")
        store = MissionStore(db)
        try:
            cur = store._conn.execute("PRAGMA user_version")
            self.assertEqual(cur.fetchone()[0], 2)
            # Mission table exists
            cur = store._conn.execute(
                "SELECT name FROM sqlite_master "
                "WHERE type='table' AND name IN "
                "('missions', 'op_friendly', 'op_blacklist', "
                " 'op_manual_location', 'op_approvals_log')")
            names = {r["name"] for r in cur.fetchall()}
            self.assertEqual(names, {"missions", "op_friendly",
                                       "op_blacklist", "op_manual_location",
                                       "op_approvals_log"})
            # mission_id column on existing tables
            cur = store._conn.execute("PRAGMA table_info(rf_events)")
            cols = {r["name"] for r in cur.fetchall()}
            self.assertIn("mission_id", cols)
            self.assertIn("gps_age_s", cols)
            self.assertIn("upstream_source", cols)
        finally:
            store.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
