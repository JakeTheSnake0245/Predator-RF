"""AutoTasker budget gates must hold under concurrent bursts.

The race the architect flagged: two coroutines both check the gate,
both pass, then both await the network call → overshoot. Fixed by
holding a lock across the check-and-reserve."""
import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.coordination.auto_tasker import AutoTasker


class _FakeClient:
    def __init__(self, node_id):
        self.node_id = node_id
        self.tunes = []
        self.node = None

    async def send_tune_command(self, freq_hz: float) -> bool:
        # Simulate network latency so concurrent calls overlap.
        await asyncio.sleep(0.01)
        self.tunes.append(freq_hz)
        return True


class _FakeFleet:
    def __init__(self, node_ids):
        self._clients = {nid: _FakeClient(nid) for nid in node_ids}

    def get_client(self, nid):
        return self._clients.get(nid)


class AutoTaskerConcurrencyTests(unittest.IsolatedAsyncioTestCase):
    async def test_global_budget_holds_under_concurrent_burst(self):
        fleet = _FakeFleet([f"n{i}" for i in range(20)])
        at = AutoTasker(fleet, min_interval_s=0.0,
                        global_max_per_minute=5)
        # 20 concurrent tunes, all distinct nodes (so per-node rate
        # limit doesn't help). Without the lock, several would race
        # past _check_global_budget before _global_tune_times grew.
        results = await asyncio.gather(*[
            at._tune_one(f"n{i}", 462.6e6, "em", "focus_all_nodes")
            for i in range(20)])
        issued = sum(1 for r in results if r)
        self.assertEqual(issued, 5,
            f"expected exactly 5 tunes under budget=5, got {issued}")
        self.assertEqual(at.tasks_skipped_global_budget, 15)

    async def test_per_node_rate_limit_holds_under_concurrent_burst(self):
        fleet = _FakeFleet(["solo"])
        at = AutoTasker(fleet, min_interval_s=10.0,
                        global_max_per_minute=100)
        results = await asyncio.gather(*[
            at._tune_one("solo", 462.6e6, "em", "focus_all_nodes")
            for _ in range(8)])
        issued = sum(1 for r in results if r)
        self.assertEqual(issued, 1,
            f"per-node rate limit should permit exactly 1 tune, got {issued}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
