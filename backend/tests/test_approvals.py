"""ApprovalQueue: enqueue, list pending, approve / reject / expire,
back-pressure when the queue is full."""
import asyncio
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.operator.approvals import ApprovalQueue


def _t(em="em-1", freq=462e6):
    return {"emitter_id": em, "primary_frequency": freq,
            "estimated_lat": 35.1, "estimated_lon": -106.5,
            "observation_count": 5, "confidence": 0.7}


def _r(action="alert_operator_immediately", threat="high"):
    return {"recommended_action": action, "threat_level": threat,
            "summary": "x", "escalate_to_atak": True}


class ApprovalQueueTests(unittest.IsolatedAsyncioTestCase):
    async def test_terminal_hook_fires_for_every_state(self):
        """Approved, rejected, expired, and dropped each emit a single
        terminal callback so the audit ledger is complete."""
        q = ApprovalQueue(max_pending=2, expiry_s=0.05)
        seen: list[tuple[str, str]] = []

        async def hook(p):
            seen.append((p.approval_id, p.state))

        q.on_terminal(hook)
        # approved
        a1 = await q.enqueue(_t("em-A"), _r(), None)
        await q.approve(a1)
        # rejected
        a2 = await q.enqueue(_t("em-B"), _r(), None)
        await q.reject(a2, reason="false-positive")
        # expired
        a3 = await q.enqueue(_t("em-C"), _r(), None)
        await asyncio.sleep(0.07)
        await q.expire_stale()
        # dropped — fill queue past max_pending=2
        a4 = await q.enqueue(_t("em-D"), _r(), None)
        a5 = await q.enqueue(_t("em-E"), _r(), None)
        a6 = await q.enqueue(_t("em-F"), _r(), None)  # evicts a4

        states = {sid: state for sid, state in seen}
        self.assertEqual(states.get(a1), "approved")
        self.assertEqual(states.get(a2), "rejected")
        self.assertEqual(states.get(a3), "expired")
        self.assertEqual(states.get(a4), "dropped")

    async def test_mission_provider_snapshots_at_enqueue(self):
        """Mission_id is captured at enqueue time, not decision time."""
        current = ["mission-X"]
        q = ApprovalQueue()
        q.set_mission_provider(lambda: current[0])
        aid = await q.enqueue(_t(), _r(), None)
        current[0] = "mission-Y"  # operator rolls mission
        approved = await q.approve(aid)
        self.assertEqual(approved.mission_id, "mission-X")

    async def test_enqueue_then_list_pending(self):
        q = ApprovalQueue()
        aid = await q.enqueue(_t(), _r(), (35.1, -106.5))
        pending = await q.list_pending()
        self.assertEqual(len(pending), 1)
        self.assertEqual(pending[0]["approval_id"], aid)
        self.assertEqual(pending[0]["state"], "pending")

    async def test_approve_invokes_callback_and_removes_from_pending(self):
        q = ApprovalQueue()
        seen = []
        q.on_approved(lambda p: seen.append(p.approval_id))
        aid = await q.enqueue(_t(), _r(), None)
        result = await q.approve(aid, operator="alice")
        self.assertIsNotNone(result)
        self.assertEqual(result.state, "approved")
        self.assertEqual(result.decided_by, "alice")
        self.assertEqual(seen, [aid])
        # Pending list no longer shows this item
        pending = await q.list_pending()
        self.assertEqual(pending, [])

    async def test_approve_unknown_id_returns_none(self):
        q = ApprovalQueue()
        self.assertIsNone(await q.approve("nope"))

    async def test_reject_with_reason(self):
        q = ApprovalQueue()
        aid = await q.enqueue(_t(), _r(), None)
        ok = await q.reject(aid, reason="false positive", operator="bob")
        self.assertTrue(ok)
        # second reject is a no-op
        self.assertFalse(await q.reject(aid, reason="x"))

    async def test_back_pressure_drops_oldest(self):
        q = ApprovalQueue(max_pending=2)
        a = await q.enqueue(_t("e1"), _r(), None)
        b = await q.enqueue(_t("e2"), _r(), None)
        c = await q.enqueue(_t("e3"), _r(), None)
        pending = await q.list_pending()
        self.assertEqual({p["approval_id"] for p in pending}, {b, c})

    async def test_expire_stale_marks_items(self):
        q = ApprovalQueue(expiry_s=0.05)
        await q.enqueue(_t(), _r(), None)
        await asyncio.sleep(0.06)
        n = await q.expire_stale()
        self.assertEqual(n, 1)
        self.assertEqual(await q.list_pending(), [])

    async def test_async_callback_is_awaited(self):
        q = ApprovalQueue()
        flag = {"done": False}

        async def cb(p):
            await asyncio.sleep(0.001)
            flag["done"] = True

        q.on_approved(cb)
        aid = await q.enqueue(_t(), _r(), None)
        await q.approve(aid)
        self.assertTrue(flag["done"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
