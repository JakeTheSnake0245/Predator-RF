"""
Operator overrides — three classes:

1) Friendly list — emitter_ids the operator has marked as own-force or
   known-benign (e.g. our own GMRS handhelds, the local police scanner).
   These suppress AutoTasker tunes, suppress CoT escalation, and get
   tagged 'friendly' in the UI. Always recoverable: an operator who
   marks the wrong thing can un-friend it.

2) Frequency blacklist — (start_hz, end_hz) ranges that the
   SweepCoordinator must skip and TrackManager must drop on ingest.
   Used to mute a noisy commercial broadcaster, a known interferer,
   or an off-limits band (e.g. emergency services freqs that we have
   regulatory obligation NOT to log).

3) Manual location override — operator supplies a manual lat/lon for
   an emitter_id (e.g. confirmed via DF gear or visual). This wins over
   any TDOA estimate until the operator clears it. Confidence is
   pinned to operator_confidence (default 0.95).

All three persist into MissionStore via the same store API used by
tracks/events. On restart the registry loads from the store so the
operator's prior overrides survive a backend bounce.
"""
from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

logger = logging.getLogger(__name__)


@dataclass
class Friendly:
    emitter_id: str
    label: str = ""
    added_ns: int = field(default_factory=time.time_ns)
    added_by: str = "operator"


@dataclass
class FreqBlacklist:
    start_hz: float
    end_hz: float
    reason: str = ""
    added_ns: int = field(default_factory=time.time_ns)

    def contains(self, freq_hz: float) -> bool:
        return self.start_hz <= freq_hz <= self.end_hz


@dataclass
class ManualLocation:
    emitter_id: str
    lat: float
    lon: float
    confidence: float = 0.95
    source: str = "operator"  # operator | df_gear | visual
    added_ns: int = field(default_factory=time.time_ns)


class OverrideRegistry:
    """In-memory + write-through-to-store registry of all operator
    overrides. Pure stdlib; tests don't need a store."""

    def __init__(self, store=None):
        self._store = store
        self._lock = asyncio.Lock()
        self._friendly: Dict[str, Friendly] = {}
        self._blacklist: List[FreqBlacklist] = []
        self._manual_loc: Dict[str, ManualLocation] = {}
        # Rehydrate
        if store is not None and hasattr(store, "load_overrides"):
            try:
                rows = store.load_overrides()
                for r in rows.get("friendly", []):
                    f = Friendly(emitter_id=r["emitter_id"],
                                 label=r.get("label") or "",
                                 added_ns=int(r.get("added_ns", 0)),
                                 added_by=r.get("added_by") or "operator")
                    self._friendly[f.emitter_id] = f
                for r in rows.get("blacklist", []):
                    self._blacklist.append(FreqBlacklist(
                        start_hz=float(r["start_hz"]),
                        end_hz=float(r["end_hz"]),
                        reason=r.get("reason") or "",
                        added_ns=int(r.get("added_ns", 0))))
                for r in rows.get("manual_location", []):
                    ml = ManualLocation(
                        emitter_id=r["emitter_id"],
                        lat=float(r["lat"]), lon=float(r["lon"]),
                        confidence=float(r.get("confidence", 0.95)),
                        source=r.get("source") or "operator",
                        added_ns=int(r.get("added_ns", 0)))
                    self._manual_loc[ml.emitter_id] = ml
                logger.info("Loaded overrides: %d friendly, %d blacklist, "
                            "%d manual locations",
                            len(self._friendly), len(self._blacklist),
                            len(self._manual_loc))
            except Exception as exc:
                logger.warning("Override rehydrate failed: %s", exc)

    # ── Friendly list ─────────────────────────────────────────────
    def is_friendly(self, emitter_id: str) -> bool:
        return emitter_id in self._friendly

    async def add_friendly(self, emitter_id: str, label: str = "") -> Friendly:
        async with self._lock:
            f = Friendly(emitter_id=emitter_id, label=label)
            self._friendly[emitter_id] = f
        if self._store is not None:
            await self._store.upsert_override("friendly", {
                "emitter_id": f.emitter_id, "label": f.label,
                "added_ns": f.added_ns, "added_by": f.added_by})
        logger.info("Friendly added: %s (%s)", emitter_id[:8], label)
        return f

    async def remove_friendly(self, emitter_id: str) -> bool:
        async with self._lock:
            removed = self._friendly.pop(emitter_id, None) is not None
        if removed and self._store is not None:
            await self._store.delete_override("friendly", emitter_id)
        return removed

    def list_friendly(self) -> List[Dict[str, Any]]:
        return [{"emitter_id": f.emitter_id, "label": f.label,
                 "added_ns": f.added_ns, "added_by": f.added_by}
                for f in self._friendly.values()]

    # ── Frequency blacklist ───────────────────────────────────────
    def is_blacklisted(self, freq_hz: float) -> bool:
        return any(b.contains(freq_hz) for b in self._blacklist)

    async def add_blacklist(self, start_hz: float, end_hz: float,
                            reason: str = "") -> FreqBlacklist:
        if start_hz > end_hz:
            start_hz, end_hz = end_hz, start_hz
        b = FreqBlacklist(start_hz=start_hz, end_hz=end_hz, reason=reason)
        async with self._lock:
            self._blacklist.append(b)
        if self._store is not None:
            await self._store.upsert_override("blacklist", {
                "start_hz": b.start_hz, "end_hz": b.end_hz,
                "reason": b.reason, "added_ns": b.added_ns})
        logger.info("Blacklist added: %.3f-%.3f MHz (%s)",
                    start_hz / 1e6, end_hz / 1e6, reason or "no reason")
        return b

    def list_blacklist(self) -> List[Dict[str, Any]]:
        return [{"start_hz": b.start_hz, "end_hz": b.end_hz,
                 "reason": b.reason, "added_ns": b.added_ns}
                for b in self._blacklist]

    async def clear_blacklist(self):
        async with self._lock:
            self._blacklist.clear()
        if self._store is not None:
            await self._store.clear_overrides("blacklist")

    # ── Manual location override ──────────────────────────────────
    def get_manual_location(
            self, emitter_id: str) -> Optional[ManualLocation]:
        return self._manual_loc.get(emitter_id)

    async def set_manual_location(self, emitter_id: str, lat: float,
                                  lon: float, confidence: float = 0.95,
                                  source: str = "operator"
                                  ) -> ManualLocation:
        ml = ManualLocation(emitter_id=emitter_id, lat=lat, lon=lon,
                            confidence=confidence, source=source)
        async with self._lock:
            self._manual_loc[emitter_id] = ml
        if self._store is not None:
            await self._store.upsert_override("manual_location", {
                "emitter_id": ml.emitter_id, "lat": ml.lat, "lon": ml.lon,
                "confidence": ml.confidence, "source": ml.source,
                "added_ns": ml.added_ns})
        logger.info("Manual location set: %s -> (%.5f, %.5f) conf=%.2f",
                    emitter_id[:8], lat, lon, confidence)
        return ml

    async def clear_manual_location(self, emitter_id: str) -> bool:
        async with self._lock:
            removed = self._manual_loc.pop(emitter_id, None) is not None
        if removed and self._store is not None:
            await self._store.delete_override("manual_location", emitter_id)
        return removed

    def list_manual_locations(self) -> List[Dict[str, Any]]:
        return [{"emitter_id": ml.emitter_id, "lat": ml.lat, "lon": ml.lon,
                 "confidence": ml.confidence, "source": ml.source,
                 "added_ns": ml.added_ns}
                for ml in self._manual_loc.values()]

    def apply_to_track(self, track_dict: Dict[str, Any]) -> Dict[str, Any]:
        """Mutate a track dict with operator overrides applied. Used by
        the API layer so the UI sees the operator's view of truth, not
        the raw fusion view."""
        em = track_dict.get("emitter_id")
        if em and em in self._friendly:
            track_dict["threat_level"] = "friendly"
            track_dict["operator_label"] = self._friendly[em].label
        if em and em in self._manual_loc:
            ml = self._manual_loc[em]
            track_dict["estimated_lat"] = ml.lat
            track_dict["estimated_lon"] = ml.lon
            track_dict["location_confidence"] = ml.confidence
            track_dict["location_source"] = ml.source
        return track_dict
