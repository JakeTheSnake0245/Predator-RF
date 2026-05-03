"""AutoTasker global per-minute budget brake."""
import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.coordination.auto_tasker import AutoTasker


class _Client:
    def __init__(self, node_id):
        self.node_id = node_id
        self.tunes = []
        self.node = type("N", (), {"center_frequencies_monitored": []})()

    async def send_tune_command(self, freq, vfo="VFO A"):
        self.tunes.append(freq)
        return True


class _Fleet:
    def __init__(self, n=10):
        self.clients = {f"n{i}": _Client(f"n{i}") for i in range(n)}

    def get_client(self, nid):
        return self.clients.get(nid)


class GlobalBudgetTests(unittest.IsolatedAsyncioTestCase):
    async def test_budget_blocks_after_max(self):
        fleet = _Fleet(n=10)
        tasks = []
        # min_interval_s=0 so per-node rate-limit doesn't fire on us
        at = AutoTasker(fleet, min_interval_s=0,
                        global_max_per_minute=3, enabled=True,
                        spawn=lambda c: tasks.append(asyncio.create_task(c))
                            or tasks[-1])
        for i in range(10):
            at.handle_assessment(
                {"emitter_id": f"em-{i}", "primary_frequency": 462e6 + i*1e3},
                {"recommended_action": "increase_dwell_time",
                 "recommended_nodes": [f"n{i}"]})
        await asyncio.gather(*tasks, return_exceptions=True)
        self.assertEqual(at.tasks_issued, 3)
        self.assertEqual(at.tasks_skipped_global_budget, 7)

    async def test_budget_window_slides_after_60s(self):
        """Sliding-window math: simulate 'old' tunes by populating
        _global_tune_times with timestamps from > 60s ago."""
        import time
        fleet = _Fleet(n=2)
        at = AutoTasker(fleet, min_interval_s=0,
                        global_max_per_minute=2, enabled=True)
        # Fill the window with stale entries
        old = time.time() - 120.0
        at._global_tune_times = [old, old, old]
        # Should still allow new tunes — the trim happens on check
        self.assertTrue(at._check_global_budget(time.time()))
        self.assertEqual(at._global_tune_times, [])

    async def test_disabled_does_nothing(self):
        fleet = _Fleet(n=2)
        tasks = []
        at = AutoTasker(fleet, enabled=False,
                        global_max_per_minute=1,
                        spawn=lambda c: tasks.append(asyncio.create_task(c))
                            or tasks[-1])
        at.handle_assessment(
            {"emitter_id": "em", "primary_frequency": 462e6},
            {"recommended_action": "focus_all_nodes",
             "recommended_nodes": ["n1"]})
        # Disabled → no tasks queued, no counters incremented
        self.assertEqual(at.tasks_issued, 0)
        self.assertEqual(at.tasks_skipped_global_budget, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
