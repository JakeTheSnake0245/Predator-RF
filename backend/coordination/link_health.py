"""
LinkHealthMonitor — detects node online↔offline transitions.

Pure, synchronous, stateful-per-instance. The caller (PredatorBackend's
link-health loop) feeds it one observation per node per tick; the monitor
returns a transition event dict when — and only when — the node's online
state flipped since the previous observation.

The very first observation of a node establishes its baseline state
WITHOUT emitting an event, so a backend restart doesn't fire a storm of
"node_online" alarms for a healthy fleet.

Event shape (mirrored by the C++ web backend for parity):
    {
        "type":            "node_online" | "node_offline",
        "node_id":         str,
        "ts_ns":           int,      # transition detection time
        "last_contact_ns": int|None, # last successful poll, if any
        "offline_after_s": float,    # threshold used for the decision
    }
"""
from __future__ import annotations

import time
from typing import Dict, List, Optional

from backend.models.sensor_node import NODE_OFFLINE_AFTER_S


class LinkHealthMonitor:
    def __init__(self, offline_after_s: float = NODE_OFFLINE_AFTER_S):
        self.offline_after_s = float(offline_after_s)
        self._prior: Dict[str, bool] = {}

    def is_online(self, last_contact_ns: Optional[int],
                  now_ns: Optional[int] = None) -> bool:
        if not last_contact_ns:
            return False
        now = now_ns if now_ns is not None else time.time_ns()
        return (now - last_contact_ns) / 1e9 < self.offline_after_s

    def observe(self, node_id: str, last_contact_ns: Optional[int],
                now_ns: Optional[int] = None) -> Optional[dict]:
        """Record one observation. Returns a transition event dict when the
        node's online state changed since the last observation, else None."""
        now = now_ns if now_ns is not None else time.time_ns()
        online = self.is_online(last_contact_ns, now)
        prev = self._prior.get(node_id)
        self._prior[node_id] = online
        if prev is None or prev == online:
            return None
        return {
            "type": "node_online" if online else "node_offline",
            "node_id": node_id,
            "ts_ns": now,
            "last_contact_ns": last_contact_ns or None,
            "offline_after_s": self.offline_after_s,
        }

    def forget(self, node_id: str) -> None:
        """Drop cached state for a deregistered node."""
        self._prior.pop(node_id, None)

    def observe_fleet(self, contacts: Dict[str, Optional[int]],
                      now_ns: Optional[int] = None) -> List[dict]:
        """Observe many nodes at once; prune state for nodes not present."""
        events = []
        for node_id, last_ns in contacts.items():
            ev = self.observe(node_id, last_ns, now_ns)
            if ev:
                events.append(ev)
        for gone in [n for n in self._prior if n not in contacts]:
            self._prior.pop(gone, None)
        return events
