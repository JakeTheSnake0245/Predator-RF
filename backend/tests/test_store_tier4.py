"""Tier 4 store helpers: fetch_events_since + latest_assessments."""
from __future__ import annotations

import asyncio
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.persistence.store import MissionStore


class StoreTier4(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self._tmp = tempfile.NamedTemporaryFile(suffix=".db", delete=False)
        self._tmp.close()
        self.store = MissionStore(self._tmp.name)

    def tearDown(self):
        self.store.close()
        try:
            os.unlink(self._tmp.name)
        except OSError:
            pass

    async def test_fetch_events_since_strict_gt(self):
        await self.store.record_event({
            "event_id": "e1", "timestamp_ns": 100, "node_id": "n1",
            "frequency": 1.0, "power_dbfs": -1.0, "snr_db": 1.0,
        })
        await self.store.record_event({
            "event_id": "e2", "timestamp_ns": 200, "node_id": "n1",
            "frequency": 2.0, "power_dbfs": -2.0, "snr_db": 2.0,
        })
        # since_ns=100 must EXCLUDE the row at exactly 100 — the cursor
        # the client got back from a previous poll is server-now, so
        # re-including it would dup events on every poll.
        out = await self.store.fetch_events_since(since_ns=100, limit=10)
        self.assertEqual([e["event_id"] for e in out], ["e2"])

    async def test_fetch_events_since_limit_honored(self):
        for i in range(5):
            await self.store.record_event({
                "event_id": f"e{i}", "timestamp_ns": 100 + i, "node_id": "n",
                "frequency": 1.0, "power_dbfs": -1.0, "snr_db": 1.0,
            })
        out = await self.store.fetch_events_since(since_ns=0, limit=3)
        self.assertEqual(len(out), 3)
        # Must be ascending so the client's cursor advances monotonically.
        self.assertEqual([e["timestamp_ns"] for e in out], [100, 101, 102])

    async def test_latest_assessment_per_emitter_wins(self):
        # Two assessments for same emitter; we should see only the newer.
        await self.store.record_assessment({
            "emitter_id": "E1", "assessment_ns": 100,
            "threat_level": "low", "confidence": 0.5, "summary": "old",
            "recommended_action": "monitor", "escalate_to_atak": 0,
        })
        await self.store.record_assessment({
            "emitter_id": "E1", "assessment_ns": 200,
            "threat_level": "high", "confidence": 0.9, "summary": "new",
            "recommended_action": "escalate", "escalate_to_atak": 1,
        })
        # And one for a separate emitter.
        await self.store.record_assessment({
            "emitter_id": "E2", "assessment_ns": 50,
            "threat_level": "medium", "confidence": 0.6, "summary": "only",
            "recommended_action": "monitor", "escalate_to_atak": 0,
        })
        latest = await self.store.latest_assessments()
        self.assertEqual(set(latest.keys()), {"E1", "E2"})
        self.assertEqual(latest["E1"]["summary"], "new")
        self.assertTrue(latest["E1"]["escalate_to_atak"])
        self.assertFalse(latest["E2"]["escalate_to_atak"])

    async def test_latest_assessments_empty_when_none(self):
        self.assertEqual(await self.store.latest_assessments(), {})


if __name__ == "__main__":
    unittest.main(verbosity=2)
