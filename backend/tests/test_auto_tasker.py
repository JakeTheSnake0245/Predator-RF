"""
AutoTasker tests — verify the intelligence-to-action loop:

* `increase_dwell_time` / `focus_all_nodes` → tune issued
* `continue_monitoring` / `alert_operator_immediately` → no tune
* Per-node rate limit suppresses thrashing
* Already-tuned nodes are skipped
* Missing client / send failure logged but not raised
"""
from __future__ import annotations

import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.coordination.auto_tasker import AutoTasker


class _FakeNode:
    def __init__(self, freqs=None):
        self.center_frequencies_monitored = list(freqs or [])


class _FakeClient:
    def __init__(self, *, node=None, return_value=True, raises=False):
        self.node = node or _FakeNode()
        self.return_value = return_value
        self.raises = raises
        self.calls: list[float] = []

    async def send_tune_command(self, frequency_hz: float, vfo: str = "VFO A"):
        self.calls.append(frequency_hz)
        if self.raises:
            raise RuntimeError("simulated network failure")
        return self.return_value


class _FakeFleet:
    def __init__(self, clients=None):
        self.clients = dict(clients or {})

    def get_client(self, node_id):
        return self.clients.get(node_id)


def _track(emitter_id="em1", freq=462_612_500.0):
    return {"emitter_id": emitter_id, "primary_frequency": freq}


def _report(action="focus_all_nodes", nodes=("n1", "n2"),
            level="high", emitter_id="em1"):
    return {
        "emitter_id": emitter_id,
        "threat_level": level,
        "recommended_action": action,
        "recommended_nodes": list(nodes),
    }


async def _drain():
    """Yield to the loop so create_task'd coroutines actually run."""
    for _ in range(10):
        await asyncio.sleep(0)


class AutoTaskerTests(unittest.IsolatedAsyncioTestCase):
    async def test_focus_all_nodes_tunes_recommended_nodes(self):
        c1, c2 = _FakeClient(), _FakeClient()
        fleet = _FakeFleet({"n1": c1, "n2": c2})
        tasker = AutoTasker(fleet, min_interval_s=0.0)

        tasker.handle_assessment(_track(), _report(action="focus_all_nodes"))
        await _drain()

        self.assertEqual(c1.calls, [462_612_500.0])
        self.assertEqual(c2.calls, [462_612_500.0])
        self.assertEqual(tasker.tasks_issued, 2)

    async def test_increase_dwell_time_also_triggers(self):
        c = _FakeClient()
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, min_interval_s=0.0)

        tasker.handle_assessment(
            _track(), _report(action="increase_dwell_time", nodes=("n1",)))
        await _drain()
        self.assertEqual(len(c.calls), 1)

    async def test_continue_monitoring_is_noop(self):
        c = _FakeClient()
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, enabled=True)
        tasker.handle_assessment(
            _track(), _report(action="continue_monitoring", nodes=("n1",)))
        await _drain()
        self.assertEqual(c.calls, [])
        self.assertEqual(tasker.tasks_issued, 0)
        self.assertEqual(tasker.assessments_seen, 1)

    async def test_critical_action_requires_human_approval(self):
        c = _FakeClient()
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, enabled=True)
        tasker.handle_assessment(
            _track(),
            _report(action="alert_operator_immediately",
                    nodes=("n1",), level="critical"))
        await _drain()
        self.assertEqual(c.calls, [],
            "critical assessments must NOT be auto-actioned")

    async def test_disabled_tasker_skips_everything(self):
        c = _FakeClient()
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, enabled=False)
        tasker.handle_assessment(_track(), _report(nodes=("n1",)))
        await _drain()
        self.assertEqual(c.calls, [])

    async def test_rate_limit_blocks_repeat_tunes(self):
        c = _FakeClient()
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, min_interval_s=60.0)

        tasker.handle_assessment(_track(), _report(nodes=("n1",)))
        await _drain()
        # Same emitter again immediately → suppressed by rate limit
        tasker.handle_assessment(_track(), _report(nodes=("n1",)))
        await _drain()

        self.assertEqual(len(c.calls), 1)
        self.assertEqual(tasker.tasks_issued, 1)
        self.assertEqual(tasker.tasks_skipped_rate_limit, 1)

    async def test_already_tuned_node_is_skipped(self):
        # Node is already monitoring 462.6125 MHz — within tolerance
        node = _FakeNode(freqs=[462_613_000.0])
        c = _FakeClient(node=node)
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, min_interval_s=0.0,
                            freq_match_tolerance_hz=2_000.0)

        tasker.handle_assessment(_track(), _report(nodes=("n1",)))
        await _drain()
        self.assertEqual(c.calls, [])
        self.assertEqual(tasker.tasks_skipped_already_tuned, 1)

    async def test_missing_client_handled_gracefully(self):
        fleet = _FakeFleet({})  # no nodes registered
        tasker = AutoTasker(fleet, min_interval_s=0.0)
        # Should not raise — just log
        tasker.handle_assessment(_track(), _report(nodes=("ghost",)))
        await _drain()
        self.assertEqual(tasker.tasks_issued, 0)

    async def test_send_failure_does_not_raise(self):
        c = _FakeClient(raises=True)
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, min_interval_s=0.0)
        tasker.handle_assessment(_track(), _report(nodes=("n1",)))
        await _drain()
        self.assertEqual(tasker.tasks_failed, 1)
        # Failure must not advance the rate-limit clock — operator can retry
        tasker.handle_assessment(_track(), _report(nodes=("n1",)))
        await _drain()
        self.assertEqual(tasker.tasks_failed, 2)

    async def test_no_recommended_nodes_is_noop(self):
        c = _FakeClient()
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, min_interval_s=0.0)
        tasker.handle_assessment(_track(), _report(nodes=()))
        await _drain()
        self.assertEqual(c.calls, [])

    async def test_invalid_frequency_skipped(self):
        c = _FakeClient()
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, min_interval_s=0.0)
        tasker.handle_assessment(
            {"emitter_id": "x", "primary_frequency": 0.0},
            _report(nodes=("n1",)))
        await _drain()
        self.assertEqual(c.calls, [])

    async def test_send_returning_false_counts_as_failure(self):
        c = _FakeClient(return_value=False)
        fleet = _FakeFleet({"n1": c})
        tasker = AutoTasker(fleet, min_interval_s=0.0)
        tasker.handle_assessment(_track(), _report(nodes=("n1",)))
        await _drain()
        self.assertEqual(tasker.tasks_failed, 1)
        self.assertEqual(tasker.tasks_issued, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
