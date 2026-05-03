"""
Mission lifecycle — operator marks the start and end of a SIGINT
mission so events / tracks / assessments / approvals can be grouped,
exported, and replayed as a single after-action package.

Without this, persistence is one ever-growing append-only blob and
operators can't say "show me everything from yesterday's drill" or
"export Mission Bravo as a tarball for the AAR." Both are first-week
operational asks.

The MissionRegistry holds the *current* (in-flight) mission_id; every
write path in the backend tags its row with that id (or NULL for
"not part of any mission"). MissionStore.export_mission() bundles the
DB rows into a JSONL tarball.
"""
from __future__ import annotations

import asyncio
import logging
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)


@dataclass
class Mission:
    mission_id: str
    name: str
    started_ns: int
    ended_ns: Optional[int] = None
    operator: str = "operator"
    notes: str = ""

    @property
    def is_active(self) -> bool:
        return self.ended_ns is None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "mission_id": self.mission_id,
            "name": self.name,
            "started_ns": self.started_ns,
            "ended_ns": self.ended_ns,
            "operator": self.operator,
            "notes": self.notes,
            "is_active": self.is_active,
        }


class MissionRegistry:
    """In-memory current-mission tracker, with a write-through to
    MissionStore so the active mission survives a restart."""

    def __init__(self, store=None):
        self._store = store
        self._lock = asyncio.Lock()
        self._active: Optional[Mission] = None
        self._all: Dict[str, Mission] = {}
        # Rehydrate any mission that was in-flight when we last shut down
        if store is not None and hasattr(store, "load_missions"):
            try:
                for row in store.load_missions():
                    m = Mission(
                        mission_id=row["mission_id"], name=row["name"],
                        started_ns=int(row["started_ns"]),
                        ended_ns=(int(row["ended_ns"])
                                  if row.get("ended_ns") else None),
                        operator=row.get("operator") or "operator",
                        notes=row.get("notes") or "")
                    self._all[m.mission_id] = m
                    if m.is_active:
                        self._active = m
                if self._active:
                    logger.info("Resumed active mission '%s' (%s)",
                                self._active.name,
                                self._active.mission_id[:8])
            except Exception as exc:
                logger.warning("Mission rehydrate failed: %s", exc)

    @property
    def active(self) -> Optional[Mission]:
        return self._active

    @property
    def active_id(self) -> Optional[str]:
        return self._active.mission_id if self._active else None

    async def start(self, name: str, operator: str = "operator",
                    notes: str = "") -> Mission:
        async with self._lock:
            # End any in-flight mission first — operator shouldn't have
            # to remember to close one before opening another.
            if self._active is not None:
                logger.info("Auto-ending previous mission '%s' on new start",
                            self._active.name)
                self._active.ended_ns = time.time_ns()
                if self._store is not None:
                    await self._store.upsert_mission(self._active.to_dict())
            m = Mission(mission_id=str(uuid.uuid4()), name=name,
                        started_ns=time.time_ns(), operator=operator,
                        notes=notes)
            self._all[m.mission_id] = m
            self._active = m
        if self._store is not None:
            await self._store.upsert_mission(m.to_dict())
        logger.info("Mission START '%s' id=%s by %s", name,
                    m.mission_id[:8], operator)
        return m

    async def end(self, mission_id: Optional[str] = None) -> Optional[Mission]:
        async with self._lock:
            target = (self._all.get(mission_id) if mission_id
                      else self._active)
            if target is None or not target.is_active:
                return None
            target.ended_ns = time.time_ns()
            if self._active is target:
                self._active = None
        if self._store is not None:
            await self._store.upsert_mission(target.to_dict())
        logger.info("Mission END '%s' id=%s", target.name,
                    target.mission_id[:8])
        return target

    def list_missions(self) -> List[Dict[str, Any]]:
        return [m.to_dict() for m in self._all.values()]
