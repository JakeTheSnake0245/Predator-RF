"""
Manual operator approval queue for CoT/TAK pushes.

When `cot_require_manual_approval=True` is set in config, the cot path
no longer fires automatically on `assessment.escalate_to_atak`. Instead,
each escalation is enqueued here and surfaces on the operator UI; the
operator clicks Approve / Reject and the approved items are then drained
to the actual CoT emitter.

Why: a single high-confidence false positive can spam TOC in the middle
of a mission. The fully-automated path is fine for a permissive lab
config, but in the field the operator needs final say on what goes out
to ATAK. The check is opt-in so existing automated flows keep working.

Design notes:
- Pure stdlib, async-safe via a single asyncio.Lock.
- Fixed-size deque so a stuck operator doesn't OOM the backend.
- Approvals carry the full track + report payloads at enqueue time so
  the eventual CoT push is byte-for-byte what the operator saw, not a
  re-fetch that may have changed.
- Persistence is intentionally out of scope here — pending approvals
  are ephemeral by design (a 2-hour-old escalation is no longer
  actionable). MissionStore records the *outcome* (approved/rejected)
  via record_approval().
"""
from __future__ import annotations

import asyncio
import logging
import time
import uuid
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple

logger = logging.getLogger(__name__)


@dataclass
class PendingApproval:
    approval_id: str
    track: Dict[str, Any]
    report: Dict[str, Any]
    fallback_location: Optional[Tuple[float, float]]
    enqueued_ns: int
    state: str = "pending"  # pending | approved | rejected | expired | dropped
    decided_by: Optional[str] = None
    decided_at_ns: Optional[int] = None
    reason: Optional[str] = None
    # Captured at enqueue time so the audit row records WHICH mission
    # was active when the operator was first asked. If the mission
    # rolls between enqueue and decide, we still attribute the
    # approval to the originating mission.
    mission_id: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "approval_id": self.approval_id,
            "state": self.state,
            "enqueued_ns": self.enqueued_ns,
            "decided_by": self.decided_by,
            "decided_at_ns": self.decided_at_ns,
            "reason": self.reason,
            "emitter_id": self.track.get("emitter_id"),
            "primary_frequency": self.track.get("primary_frequency"),
            "threat_level": self.report.get("threat_level"),
            "recommended_action": self.report.get("recommended_action"),
            "estimated_lat": self.track.get("estimated_lat"),
            "estimated_lon": self.track.get("estimated_lon"),
            "fallback_location": list(self.fallback_location)
                if self.fallback_location else None,
            "mission_id": self.mission_id,
        }


class ApprovalQueue:
    def __init__(self, max_pending: int = 200, expiry_s: float = 7200.0):
        self._pending: Dict[str, PendingApproval] = {}
        # FIFO order for the UI; deque holds approval_ids
        self._order: deque[str] = deque()
        self._max = max_pending
        self._expiry_s = expiry_s
        self._lock = asyncio.Lock()
        self._approved_cb: Optional[Callable[[PendingApproval], Any]] = None
        # Hooks injected by orchestrator: snapshot of current mission_id
        # at enqueue time + persistence of every terminal state so the
        # AAR ledger has a complete record of operator decisions.
        self._mission_provider: Optional[Callable[[], Optional[str]]] = None
        self._terminal_cb: Optional[Callable[[PendingApproval], Any]] = None

    def on_approved(self, fn: Callable[[PendingApproval], Any]):
        self._approved_cb = fn

    def set_mission_provider(self, fn: Callable[[], Optional[str]]) -> None:
        self._mission_provider = fn

    def on_terminal(self, fn: Callable[[PendingApproval], Any]) -> None:
        """Called for every terminal transition (approved, rejected,
        expired, dropped). Used by the orchestrator to write the audit
        row regardless of decision."""
        self._terminal_cb = fn

    async def _emit_terminal(self, item: PendingApproval) -> None:
        if self._terminal_cb is None:
            return
        try:
            res = self._terminal_cb(item)
            if asyncio.iscoroutine(res):
                await res
        except Exception as exc:
            logger.exception("Approval terminal hook raised: %s", exc)

    async def enqueue(self, track: Dict[str, Any], report: Dict[str, Any],
                      fallback_location: Optional[Tuple[float, float]]
                      ) -> str:
        approval_id = str(uuid.uuid4())
        mission_id = None
        if self._mission_provider is not None:
            try:
                mission_id = self._mission_provider()
            except Exception:
                mission_id = None
        item = PendingApproval(
            approval_id=approval_id, track=track, report=report,
            fallback_location=fallback_location, enqueued_ns=time.time_ns(),
            mission_id=mission_id)
        dropped: List[PendingApproval] = []
        async with self._lock:
            # Compact: drop already-decided entries from the head FIRST
            # (they're history, not back-pressure), then evict actually-
            # pending entries only if still over budget. Without this,
            # an approve()'d entry sitting at the head gets re-flagged
            # as "dropped" the next time a new enqueue overruns.
            while self._order:
                head_id = self._order[0]
                head = self._pending.get(head_id)
                if head is None or head.state != "pending":
                    self._order.popleft()
                    continue
                break
            while len(self._order) >= self._max:
                drop_id = self._order.popleft()
                drop = self._pending.get(drop_id)
                if drop and drop.state == "pending":
                    drop.state = "dropped"
                    drop.decided_at_ns = time.time_ns()
                    drop.reason = "queue_full"
                    dropped.append(drop)
                    logger.warning("Approval queue full — dropped %s",
                                   drop_id[:8])
            self._pending[approval_id] = item
            self._order.append(approval_id)
        for d in dropped:
            await self._emit_terminal(d)
        logger.info("Enqueued CoT approval %s for emitter %s (%s)",
                    approval_id[:8],
                    track.get("emitter_id", "?")[:8],
                    report.get("recommended_action", "?"))
        return approval_id

    async def list_pending(self) -> List[Dict[str, Any]]:
        async with self._lock:
            now_ns = time.time_ns()
            return [p.to_dict() for pid in list(self._order)
                    if (p := self._pending.get(pid)) and p.state == "pending"
                    and (now_ns - p.enqueued_ns) / 1e9 < self._expiry_s]

    async def approve(self, approval_id: str,
                      operator: str = "operator"
                      ) -> Optional[PendingApproval]:
        async with self._lock:
            p = self._pending.get(approval_id)
            if p is None or p.state != "pending":
                return None
            p.state = "approved"
            p.decided_by = operator
            p.decided_at_ns = time.time_ns()
        if self._approved_cb is not None:
            try:
                res = self._approved_cb(p)
                if asyncio.iscoroutine(res):
                    await res
            except Exception as exc:
                logger.exception("Approval callback raised: %s", exc)
        await self._emit_terminal(p)
        return p

    async def reject(self, approval_id: str, reason: str = "",
                     operator: str = "operator") -> bool:
        async with self._lock:
            p = self._pending.get(approval_id)
            if p is None or p.state != "pending":
                return False
            p.state = "rejected"
            p.decided_by = operator
            p.decided_at_ns = time.time_ns()
            p.reason = reason
        logger.info("Rejected CoT approval %s by %s: %s",
                    approval_id[:8], operator, reason or "(no reason)")
        await self._emit_terminal(p)
        return True

    async def expire_stale(self) -> int:
        """Mark items older than expiry_s as expired. Returns count.
        Called periodically by the maintenance loop in main.py."""
        now_ns = time.time_ns()
        expired: List[PendingApproval] = []
        async with self._lock:
            for pid in list(self._order):
                p = self._pending.get(pid)
                if p and p.state == "pending":
                    age_s = (now_ns - p.enqueued_ns) / 1e9
                    if age_s > self._expiry_s:
                        p.state = "expired"
                        p.decided_at_ns = now_ns
                        expired.append(p)
        for p in expired:
            await self._emit_terminal(p)
        if expired:
            logger.info("Expired %d stale CoT approvals", len(expired))
        return len(expired)

    def stats(self) -> Dict[str, int]:
        from collections import Counter
        c = Counter(p.state for p in self._pending.values())
        return {"pending": c.get("pending", 0),
                "approved": c.get("approved", 0),
                "rejected": c.get("rejected", 0),
                "expired": c.get("expired", 0),
                "total": sum(c.values())}
