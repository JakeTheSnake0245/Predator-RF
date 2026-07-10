"""
CorrelationEngine — operator-defined correlation rules + intelligent fleet coordination.

Two distinct coordination modes run in parallel:

1. Custom operator rules
   Stored in correlation_rules table.  Each rule specifies:
   - which nodes to watch (empty = all nodes)
   - what frequency range to watch
   - a time window
   - how many nodes must co-detect to fire
   - what action to take (alert, geo_cue, log, all)

   Evaluated on every new track/event ingest via on_event().

2. Known-target sweep response
   When a track reaches STABLE state AND its fingerprint matches a stored
   signal from the repository (cosine similarity >= threshold), the engine:
   a. Immediately geo-cues all capable nodes via AutoTasker (dwell on freq)
   b. Saves a correlated_intercept record linking all observing nodes
   c. Pushes a high-priority operator alert

Rules are hot-reloaded from the DB; no restart required to add/change a rule.

Thread-safety: all internal state is protected by asyncio (single-threaded event
loop).  Rule storage uses MissionStore's lock.
"""
from __future__ import annotations

import asyncio
import json
import logging
import sqlite3
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Set

logger = logging.getLogger(__name__)


@dataclass
class CorrelationRule:
    rule_id:     str
    name:        str
    nodes:       List[str]       # empty = all nodes
    freq_lo_hz:  float
    freq_hi_hz:  float
    window_s:    float
    min_nodes:   int = 2
    action:      str = "alert"   # alert | geo_cue | log | all
    enabled:     bool = True
    notes:       str = ""


@dataclass
class _DetectionSlot:
    node_id:   str
    freq_hz:   float
    power_dbfs: float
    seen_ns:   int


@dataclass
class CorrelationFired:
    rule_id:       str
    rule_name:     str
    node_ids:      List[str]
    freq_hz:       float
    window_s:      float
    action:        str
    fired_ns:      int = field(default_factory=time.time_ns)


class CorrelationEngine:
    def __init__(self,
                 store,
                 signal_repo=None,
                 auto_tasker=None,
                 fingerprint_threshold: float = 0.82):
        """
        store            — MissionStore (for rule persistence)
        signal_repo      — SignalRepository (for known-target recognition)
        auto_tasker      — AutoTasker (for geo-cue)
        """
        self._store    = store
        self._repo     = signal_repo
        self._tasker   = auto_tasker
        self._fp_thresh = fingerprint_threshold

        self._rules: Dict[str, CorrelationRule] = {}
        self._window: Dict[str, List[_DetectionSlot]] = {}

        self._alert_callbacks: List[Callable[[Dict], None]] = []
        self._ensure_schema()
        self._load_rules()

    def _ensure_schema(self):
        try:
            self._store._conn.executescript("""
                CREATE TABLE IF NOT EXISTS correlation_rules (
                    rule_id      TEXT PRIMARY KEY,
                    name         TEXT NOT NULL,
                    nodes_json   TEXT NOT NULL DEFAULT '[]',
                    freq_lo_hz   REAL NOT NULL,
                    freq_hi_hz   REAL NOT NULL,
                    window_s     REAL NOT NULL DEFAULT 60.0,
                    min_nodes    INTEGER NOT NULL DEFAULT 2,
                    action       TEXT NOT NULL DEFAULT 'alert',
                    enabled      INTEGER NOT NULL DEFAULT 1,
                    notes        TEXT DEFAULT '',
                    created_ns   INTEGER NOT NULL
                );
            """)
        except Exception as exc:
            logger.warning("CorrelationEngine schema: %s", exc)

    def _load_rules(self):
        try:
            rows = self._store._conn.execute(
                "SELECT * FROM correlation_rules WHERE enabled=1").fetchall()
            self._rules = {}
            for row in rows:
                r = CorrelationRule(
                    rule_id   = row["rule_id"],
                    name      = row["name"],
                    nodes     = json.loads(row["nodes_json"] or "[]"),
                    freq_lo_hz= row["freq_lo_hz"],
                    freq_hi_hz= row["freq_hi_hz"],
                    window_s  = row["window_s"],
                    min_nodes = row["min_nodes"],
                    action    = row["action"],
                    enabled   = bool(row["enabled"]),
                    notes     = row["notes"] or "",
                )
                self._rules[r.rule_id] = r
            logger.info("CorrelationEngine: loaded %d rules", len(self._rules))
        except Exception as exc:
            logger.warning("CorrelationEngine: rule load failed: %s", exc)

    def add_rule(self, rule: CorrelationRule) -> str:
        now = time.time_ns()
        with self._store._lock:
            self._store._conn.execute("""
                INSERT OR REPLACE INTO correlation_rules
                (rule_id, name, nodes_json, freq_lo_hz, freq_hi_hz,
                 window_s, min_nodes, action, enabled, notes, created_ns)
                VALUES (?,?,?,?,?,?,?,?,?,?,?)
            """, (rule.rule_id, rule.name, json.dumps(rule.nodes),
                  rule.freq_lo_hz, rule.freq_hi_hz, rule.window_s,
                  rule.min_nodes, rule.action, int(rule.enabled),
                  rule.notes, now))
        self._rules[rule.rule_id] = rule
        logger.info("CorrelationEngine: rule %s (%s) added", rule.rule_id, rule.name)
        return rule.rule_id

    def remove_rule(self, rule_id: str) -> bool:
        with self._store._lock:
            self._store._conn.execute(
                "DELETE FROM correlation_rules WHERE rule_id=?", (rule_id,))
        removed = rule_id in self._rules
        self._rules.pop(rule_id, None)
        self._window.pop(rule_id, None)
        return removed

    def list_rules(self) -> List[Dict]:
        return [
            {"rule_id": r.rule_id, "name": r.name, "nodes": r.nodes,
             "freq_lo_hz": r.freq_lo_hz, "freq_hi_hz": r.freq_hi_hz,
             "window_s": r.window_s, "min_nodes": r.min_nodes,
             "action": r.action, "enabled": r.enabled, "notes": r.notes}
            for r in self._rules.values()
        ]

    def on_alert(self, callback: Callable[[Dict], None]):
        self._alert_callbacks.append(callback)

    def on_event(self, node_id: str, freq_hz: float, power_dbfs: float = 0.0):
        """Call this on every new detection event from any node."""
        now = time.time_ns()
        slot = _DetectionSlot(node_id=node_id, freq_hz=freq_hz,
                              power_dbfs=power_dbfs, seen_ns=now)
        for rule_id, rule in list(self._rules.items()):
            if not rule.enabled:
                continue
            if not (rule.freq_lo_hz <= freq_hz <= rule.freq_hi_hz):
                continue
            if rule.nodes and node_id not in rule.nodes:
                continue

            window_ns = int(rule.window_s * 1e9)
            cutoff_ns = now - window_ns
            if rule_id not in self._window:
                self._window[rule_id] = []
            self._window[rule_id].append(slot)
            self._window[rule_id] = [
                s for s in self._window[rule_id] if s.seen_ns >= cutoff_ns
            ]

            distinct_nodes: Set[str] = {s.node_id for s in self._window[rule_id]}
            if len(distinct_nodes) >= rule.min_nodes:
                self._window[rule_id] = []
                fired = CorrelationFired(
                    rule_id=rule.rule_id, rule_name=rule.name,
                    node_ids=list(distinct_nodes), freq_hz=freq_hz,
                    window_s=rule.window_s, action=rule.action
                )
                asyncio.get_event_loop().call_soon_threadsafe(
                    lambda f=fired: asyncio.ensure_future(self._fire(f))
                )

    async def on_known_target_detected(self,
                                        track,
                                        signal_id: str,
                                        similarity: float,
                                        node_ids: List[str]):
        """
        Called by PredatorBackend when a STABLE track fingerprint matches a
        stored repository signal above threshold.
        """
        freq_hz = track.primary_frequency
        logger.info(
            "CorrelationEngine: known-target match signal=%s freq=%.3f MHz "
            "sim=%.2f nodes=%s",
            signal_id, freq_hz / 1e6, similarity, node_ids)

        if self._repo:
            try:
                fix_lat = getattr(track, "estimated_lat", None)
                fix_lon = getattr(track, "estimated_lon", None)
                fix_r   = getattr(track, "location_error_radius_m", None)
                fix_m   = getattr(track, "location_method", None)
                self._repo.save_correlated_intercept(
                    signal_id=signal_id,
                    node_ids=node_ids,
                    center_hz=freq_hz,
                    rule_id="known_target_sweep",
                    fix_lat=fix_lat, fix_lon=fix_lon,
                    fix_radius_m=fix_r, fix_method=fix_m,
                    notes=f"auto-detected sim={similarity:.2f}"
                )
            except Exception as exc:
                logger.warning("CorrelationEngine: intercept save failed: %s", exc)

        if self._tasker:
            class _MockAssessment:
                recommended_action = "focus_all_nodes"
                recommended_nodes  = node_ids
                confidence         = 0.9
                emitter_id         = getattr(track, "emitter_id", "unknown")
                primary_frequency  = freq_hz
            try:
                await self._tasker.on_assessment(_MockAssessment(), list(node_ids))
            except Exception as exc:
                logger.warning("CorrelationEngine: geo-cue failed: %s", exc)

        alert = {
            "type":       "known_target_detected",
            "signal_id":  signal_id,
            "freq_hz":    freq_hz,
            "similarity": similarity,
            "node_ids":   node_ids,
            "emitter_id": getattr(track, "emitter_id", None),
            "fired_ns":   time.time_ns(),
        }
        self._dispatch_alert(alert)

    async def _fire(self, fired: CorrelationFired):
        logger.info(
            "CorrelationEngine: rule %s (%s) fired — nodes=%s freq=%.3f MHz action=%s",
            fired.rule_id, fired.rule_name, fired.node_ids,
            fired.freq_hz / 1e6, fired.action)

        if fired.action in ("geo_cue", "all") and self._tasker:
            class _MockAssessment:
                recommended_action = "focus_all_nodes"
                recommended_nodes  = fired.node_ids
                confidence         = 0.75
                emitter_id         = f"corr_{fired.rule_id}"
                primary_frequency  = fired.freq_hz
            try:
                await self._tasker.on_assessment(_MockAssessment(), fired.node_ids)
            except Exception as exc:
                logger.warning("CorrelationEngine: geo-cue failed: %s", exc)

        if fired.action in ("log", "all") and self._repo:
            try:
                sid = self._repo.save_signal(
                    node_id="correlation_engine",
                    center_hz=fired.freq_hz,
                    notes=f"rule={fired.rule_name} nodes={','.join(fired.node_ids)}"
                )
                self._repo.save_correlated_intercept(
                    signal_id=sid,
                    node_ids=fired.node_ids,
                    center_hz=fired.freq_hz,
                    rule_id=fired.rule_id,
                )
            except Exception as exc:
                logger.warning("CorrelationEngine: intercept log failed: %s", exc)

        if fired.action in ("alert", "all"):
            alert = {
                "type":      "correlation_rule_fired",
                "rule_id":   fired.rule_id,
                "rule_name": fired.rule_name,
                "node_ids":  fired.node_ids,
                "freq_hz":   fired.freq_hz,
                "window_s":  fired.window_s,
                "action":    fired.action,
                "fired_ns":  fired.fired_ns,
            }
            self._dispatch_alert(alert)

    def _dispatch_alert(self, alert: Dict):
        for cb in self._alert_callbacks:
            try:
                cb(alert)
            except Exception as exc:
                logger.warning("CorrelationEngine: alert callback error: %s", exc)
