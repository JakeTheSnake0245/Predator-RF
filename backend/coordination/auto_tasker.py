"""
AutoTasker — closes the intelligence-to-action loop.

Consumes AssessmentReports as they're produced by DecisionEngine and, when
the recommended action calls for it, tasks the recommended sensor nodes
via the KujhadFleetManager (HTTP /v1/command → tune the SDR to the track's
primary frequency).

Action vocabulary (from DecisionEngine._recommend_action):
  * `continue_monitoring`         → no-op
  * `increase_dwell_time`         → tune recommended nodes to the freq
  * `focus_all_nodes`             → tune all TDOA-capable nodes to the freq
  * `alert_operator_immediately`  → no auto-tune (critical assessments
                                    require an operator-in-the-loop hold)

Safety
------
* **Per-node rate limit** (default 30s) so a chatty emitter can't thrash
  a node into a constant retune storm.
* **Per-node "already tuned" check** — skip when the node is already
  within ±2 kHz of the requested centre frequency.
* **Critical assessments are NOT auto-actioned** — the operator must
  push the button. We emit a log line and stop. This preserves
  human-in-the-loop for the highest-stakes decisions.
* **Failures are logged, never raised** — the tasking loop is best-effort
  and must not take down the orchestrator if a node is offline.
"""
from __future__ import annotations

import asyncio
import logging
import time
from typing import Awaitable, Callable, Dict, Iterable, Optional, Protocol

logger = logging.getLogger(__name__)

# Actions we will auto-action. `alert_operator_immediately` deliberately
# omitted — it requires human approval.
_AUTO_ACTIONS = frozenset({"increase_dwell_time", "focus_all_nodes"})


class _ClientLike(Protocol):
    async def send_tune_command(self, frequency_hz: float,
                                 vfo: str = ...) -> bool: ...


class _FleetLike(Protocol):
    def get_client(self, node_id: str) -> Optional[_ClientLike]: ...


class AutoTasker:
    def __init__(self,
                 fleet_manager: _FleetLike,
                 *,
                 min_interval_s: float = 30.0,
                 freq_match_tolerance_hz: float = 2_000.0,
                 enabled: bool = True,
                 spawn: Optional[Callable[[Awaitable], asyncio.Task]] = None):
        """`spawn`: hook for the orchestrator to register the per-tune
        coroutine in its shutdown-drain set. Defaults to asyncio.create_task
        so tests and standalone use keep working, but in production
        PredatorBackend wires its own _spawn so SIGTERM cleanly waits for
        in-flight tunes (or cancels them within SHUTDOWN_DRAIN_TIMEOUT_S)
        instead of orphaning them past shutdown."""
        self.fleet = fleet_manager
        self.min_interval_s = float(min_interval_s)
        self.freq_match_tolerance_hz = float(freq_match_tolerance_hz)
        self.enabled = bool(enabled)
        self._spawn = spawn or asyncio.create_task
        # node_id → last tune unix-seconds
        self._last_tune: Dict[str, float] = {}
        # Counters for /metrics + tests
        self.tasks_issued = 0
        self.tasks_skipped_rate_limit = 0
        self.tasks_skipped_already_tuned = 0
        self.tasks_failed = 0
        self.assessments_seen = 0

    def handle_assessment(self, track_dict: dict, report_dict: dict) -> None:
        """Synchronous entry point — schedules async tune tasks. Safe to
        call from inside `_on_rf_event` (which is sync but runs on the
        asyncio loop)."""
        self.assessments_seen += 1
        if not self.enabled:
            return

        action = report_dict.get("recommended_action", "continue_monitoring")
        if action not in _AUTO_ACTIONS:
            if action == "alert_operator_immediately":
                logger.warning(
                    "Critical assessment for %s — operator approval required "
                    "before auto-tasking. Action='%s' threat=%s",
                    report_dict.get("emitter_id"), action,
                    report_dict.get("threat_level"))
            return

        nodes = list(report_dict.get("recommended_nodes") or [])
        if not nodes:
            logger.debug("AutoTasker: assessment for %s has no recommended "
                         "nodes (action=%s) — nothing to do",
                         report_dict.get("emitter_id"), action)
            return

        freq = track_dict.get("primary_frequency")
        if not freq or freq <= 0:
            logger.debug("AutoTasker: track %s has no usable frequency",
                         track_dict.get("emitter_id"))
            return

        for node_id in nodes:
            self._spawn(
                self._tune_one(node_id, float(freq),
                               track_dict.get("emitter_id", "?"), action))

    async def _tune_one(self, node_id: str, freq_hz: float,
                         emitter_id: str, action: str) -> bool:
        # Rate limit gate
        now = time.time()
        last = self._last_tune.get(node_id, 0.0)
        if now - last < self.min_interval_s:
            self.tasks_skipped_rate_limit += 1
            logger.debug("AutoTasker: %s rate-limited (last tune %.1fs ago)",
                         node_id, now - last)
            return False

        # "Already tuned" check (best-effort — get_client may return a
        # client whose underlying node has center_frequencies_monitored)
        client = self.fleet.get_client(node_id)
        if client is None:
            logger.debug("AutoTasker: no client for node %s", node_id)
            return False

        node = getattr(client, "node", None)
        if node is not None:
            already = getattr(node, "center_frequencies_monitored", None) or []
            for f in already:
                if abs(float(f) - freq_hz) <= self.freq_match_tolerance_hz:
                    self.tasks_skipped_already_tuned += 1
                    logger.debug(
                        "AutoTasker: %s already monitoring %.4f MHz — skip",
                        node_id, freq_hz / 1e6)
                    return False

        try:
            ok = await client.send_tune_command(freq_hz)
        except Exception as exc:
            self.tasks_failed += 1
            logger.warning("AutoTasker: tune of %s → %.4f MHz failed: %s",
                           node_id, freq_hz / 1e6, exc)
            return False

        if not ok:
            self.tasks_failed += 1
            logger.warning("AutoTasker: tune of %s → %.4f MHz returned False",
                           node_id, freq_hz / 1e6)
            return False

        self._last_tune[node_id] = now
        self.tasks_issued += 1
        logger.info("AutoTasker: tuned %s → %.4f MHz (emitter=%s, action=%s)",
                    node_id, freq_hz / 1e6, emitter_id[:8], action)
        return True

    def stats(self) -> dict:
        return {
            "enabled": self.enabled,
            "assessments_seen": self.assessments_seen,
            "tasks_issued": self.tasks_issued,
            "tasks_skipped_rate_limit": self.tasks_skipped_rate_limit,
            "tasks_skipped_already_tuned": self.tasks_skipped_already_tuned,
            "tasks_failed": self.tasks_failed,
        }
