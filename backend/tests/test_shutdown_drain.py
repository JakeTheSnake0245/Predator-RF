"""
Shutdown-drain regression tests — proves AutoTasker's tune tasks (and
any other coroutine routed through `spawn`) are awaited or cancelled
within SHUTDOWN_DRAIN_TIMEOUT_S, never orphaned past stop().

Architect previously flagged: AutoTasker called raw asyncio.create_task,
so its in-flight tunes were untracked and could outlive shutdown. The
fix injects a `spawn` callback. These tests lock that in.
"""
from __future__ import annotations

import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.coordination.auto_tasker import AutoTasker


class _SlowClient:
    """Pretends a tune takes a long time — simulates a slow remote."""
    def __init__(self, delay_s: float = 5.0):
        self.delay_s = delay_s
        self.node = type("N", (), {"center_frequencies_monitored": []})()
        self.started = 0
        self.completed = 0
        self.cancelled = 0

    async def send_tune_command(self, frequency_hz: float, vfo: str = "VFO A"):
        self.started += 1
        try:
            await asyncio.sleep(self.delay_s)
            self.completed += 1
            return True
        except asyncio.CancelledError:
            self.cancelled += 1
            raise


class _FakeFleet:
    def __init__(self, clients):
        self.clients = clients

    def get_client(self, node_id):
        return self.clients.get(node_id)


def _track(emitter_id="em1", freq=462_612_500.0):
    return {"emitter_id": emitter_id, "primary_frequency": freq}


def _report(action="focus_all_nodes", nodes=("n1",), level="high"):
    return {"emitter_id": "em1", "threat_level": level,
            "recommended_action": action, "recommended_nodes": list(nodes)}


class AutoTaskerSpawnHookTests(unittest.IsolatedAsyncioTestCase):
    async def test_spawn_hook_receives_each_tune_coroutine(self):
        """When the orchestrator passes a `spawn` hook, every per-tune
        coroutine must go through it (so PredatorBackend can drain them
        on SIGTERM)."""
        c1 = _SlowClient(delay_s=0.01)
        c2 = _SlowClient(delay_s=0.01)
        fleet = _FakeFleet({"n1": c1, "n2": c2})

        registered: list[asyncio.Task] = []
        def hook(coro):
            t = asyncio.create_task(coro)
            registered.append(t)
            return t

        tasker = AutoTasker(fleet, min_interval_s=0.0,
                            enabled=True, spawn=hook)
        tasker.handle_assessment(_track(), _report(nodes=("n1", "n2")))
        await asyncio.gather(*registered)

        self.assertEqual(len(registered), 2,
            "spawn hook must receive one task per recommended node")
        self.assertEqual(c1.completed, 1)
        self.assertEqual(c2.completed, 1)

    async def test_drain_pattern_cancels_stragglers_within_timeout(self):
        """Simulates the PredatorBackend.stop() drain pattern. A slow
        remote must NOT block shutdown forever — once timeout elapses,
        the orchestrator cancels stragglers and they propagate
        CancelledError cleanly."""
        c = _SlowClient(delay_s=10.0)  # Way longer than the drain timeout
        fleet = _FakeFleet({"n1": c})

        pending: set[asyncio.Task] = set()
        def hook(coro):
            t = asyncio.create_task(coro)
            pending.add(t)
            t.add_done_callback(pending.discard)
            return t

        tasker = AutoTasker(fleet, min_interval_s=0.0,
                            enabled=True, spawn=hook)
        tasker.handle_assessment(_track(), _report(nodes=("n1",)))

        # Yield so the spawned task starts
        await asyncio.sleep(0.01)
        self.assertEqual(c.started, 1)
        self.assertEqual(c.completed, 0)

        # Mimic PredatorBackend.stop() drain
        snapshot = list(pending)
        done, still_pending = await asyncio.wait(snapshot, timeout=0.1)
        self.assertEqual(len(still_pending), 1,
            "the slow tune must not have completed within 0.1s")
        for t in still_pending:
            t.cancel()
        await asyncio.gather(*still_pending, return_exceptions=True)

        self.assertEqual(c.cancelled, 1,
            "cancelled tune must propagate CancelledError")
        self.assertEqual(c.completed, 0)
        # And nothing leaked past the drain
        self.assertEqual(len(pending), 0)

    async def test_default_spawn_is_create_task_for_standalone_use(self):
        """Without a spawn hook, AutoTasker still works (uses
        asyncio.create_task). This keeps standalone tests + scripts
        working — the drain hook is an opt-in for the orchestrator."""
        c = _SlowClient(delay_s=0.001)
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, min_interval_s=0.0, enabled=True)
        tasker.handle_assessment(_track(), _report(nodes=("n1",)))
        # Yield enough times for the task to complete
        for _ in range(20):
            await asyncio.sleep(0)
        self.assertEqual(c.completed, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
