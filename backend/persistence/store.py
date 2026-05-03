"""
Mission persistence — SQLite-backed log of RF events, emitter tracks, and
assessment reports.

Design constraints
------------------
* **Stdlib-only** — `sqlite3` is in the Python standard library, so this
  works on a bare Raspberry Pi without `pip install`. No aiosqlite, no
  SQLAlchemy. Sync calls are pushed onto the default executor via
  `asyncio.to_thread` so they don't stall the asyncio loop.
* **WAL mode** — write-ahead logging gives readers + a writer without
  blocking, and survives a SIGKILL mid-write without corrupting the DB.
* **Schema versioned** via `PRAGMA user_version`. `migrate()` is idempotent
  and bumps the version atomically.
* **Append-only** for events and assessments (one row per ingest);
  **upsert** for tracks (one row per emitter_id, latest snapshot wins).
* **Replay-friendly** — `load_active_tracks(window_s)` returns the tracks
  whose `last_seen_ns` is within `window_s` so the orchestrator can rehydrate
  in-flight tracks across a restart.

Threading
---------
A single `sqlite3.Connection` is shared but `check_same_thread=False`
because asyncio.to_thread may run callbacks on different worker threads.
All writes go through `_lock` to serialise multi-threaded access.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import sqlite3
import threading
import time
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)

SCHEMA_VERSION = 1


class MissionStore:
    def __init__(self, db_path: str):
        self.db_path = db_path
        os.makedirs(os.path.dirname(os.path.abspath(db_path)) or ".",
                    exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(
            db_path, check_same_thread=False, isolation_level=None)
        self._conn.row_factory = sqlite3.Row
        # WAL gives crash-safe writes + concurrent reads. NORMAL synchronous
        # is the right tradeoff for a mission log: we accept the OS-level
        # write may lag the SQL ack by a few ms in exchange for ~10x
        # throughput. The mission is on-the-wire, not in-the-DB.
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=NORMAL")
        self._conn.execute("PRAGMA foreign_keys=ON")
        self._migrate()

    # ── Schema ────────────────────────────────────────────────────────────

    def _migrate(self):
        with self._lock:
            cur = self._conn.execute("PRAGMA user_version")
            current = cur.fetchone()[0]
            if current >= SCHEMA_VERSION:
                return
            logger.info("Migrating MissionStore schema %d → %d",
                        current, SCHEMA_VERSION)
            # v0 → v1
            self._conn.executescript("""
                CREATE TABLE IF NOT EXISTS rf_events (
                    event_id          TEXT PRIMARY KEY,
                    timestamp_ns      INTEGER NOT NULL,
                    node_id           TEXT NOT NULL,
                    frequency         REAL NOT NULL,
                    power_dbfs        REAL NOT NULL,
                    snr_db            REAL NOT NULL,
                    bandwidth_hz      REAL,
                    detector          TEXT,
                    modulation        TEXT,
                    protocol          TEXT,
                    decoded_payload   TEXT,
                    node_lat          REAL,
                    node_lon          REAL,
                    node_alt_m        REAL,
                    node_trust_score  REAL,
                    raw_json          TEXT
                );
                CREATE INDEX IF NOT EXISTS idx_rf_events_ts ON rf_events(timestamp_ns);
                CREATE INDEX IF NOT EXISTS idx_rf_events_node ON rf_events(node_id);
                CREATE INDEX IF NOT EXISTS idx_rf_events_freq ON rf_events(frequency);

                CREATE TABLE IF NOT EXISTS emitter_tracks (
                    emitter_id            TEXT PRIMARY KEY,
                    state                 TEXT NOT NULL,
                    primary_frequency     REAL NOT NULL,
                    last_power_dbfs       REAL,
                    first_seen_ns         INTEGER NOT NULL,
                    last_seen_ns          INTEGER NOT NULL,
                    observation_count     INTEGER NOT NULL,
                    confidence            REAL,
                    threat_level          TEXT,
                    modulation            TEXT,
                    protocol              TEXT,
                    estimated_lat         REAL,
                    estimated_lon         REAL,
                    location_confidence   REAL,
                    detecting_nodes_json  TEXT,
                    anomaly_flags_json    TEXT,
                    updated_ns            INTEGER NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_tracks_last_seen ON emitter_tracks(last_seen_ns);
                CREATE INDEX IF NOT EXISTS idx_tracks_state     ON emitter_tracks(state);

                CREATE TABLE IF NOT EXISTS assessment_reports (
                    report_id            INTEGER PRIMARY KEY AUTOINCREMENT,
                    emitter_id           TEXT NOT NULL,
                    assessment_ns        INTEGER NOT NULL,
                    threat_level         TEXT NOT NULL,
                    confidence           REAL,
                    summary              TEXT,
                    recommended_action   TEXT,
                    recommended_nodes_json TEXT,
                    escalate_to_atak     INTEGER NOT NULL DEFAULT 0,
                    anomaly_flags_json   TEXT
                );
                CREATE INDEX IF NOT EXISTS idx_assess_emitter ON assessment_reports(emitter_id);
                CREATE INDEX IF NOT EXISTS idx_assess_ts      ON assessment_reports(assessment_ns);
            """)
            self._conn.execute(f"PRAGMA user_version = {SCHEMA_VERSION}")

    # ── Sync write primitives (called via asyncio.to_thread) ──────────────

    def _insert_event_sync(self, ev: Dict[str, Any]):
        with self._lock:
            self._conn.execute(
                """INSERT OR IGNORE INTO rf_events
                   (event_id, timestamp_ns, node_id, frequency, power_dbfs,
                    snr_db, bandwidth_hz, detector, modulation, protocol,
                    decoded_payload, node_lat, node_lon, node_alt_m,
                    node_trust_score, raw_json)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (ev.get("event_id"), int(ev.get("timestamp_ns", 0)),
                 ev.get("node_id", ""), float(ev.get("frequency", 0.0)),
                 float(ev.get("power_dbfs", 0.0)), float(ev.get("snr_db", 0.0)),
                 ev.get("bandwidth_hz"), ev.get("detector"),
                 ev.get("modulation"), ev.get("protocol"),
                 ev.get("decoded_payload"),
                 ev.get("node_lat"), ev.get("node_lon"), ev.get("node_alt_m"),
                 ev.get("node_trust_score"),
                 json.dumps(ev, default=str)))

    def _upsert_track_sync(self, tr: Dict[str, Any]):
        with self._lock:
            self._conn.execute(
                """INSERT INTO emitter_tracks
                   (emitter_id, state, primary_frequency, last_power_dbfs,
                    first_seen_ns, last_seen_ns, observation_count,
                    confidence, threat_level, modulation, protocol,
                    estimated_lat, estimated_lon, location_confidence,
                    detecting_nodes_json, anomaly_flags_json, updated_ns)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
                   ON CONFLICT(emitter_id) DO UPDATE SET
                     state=excluded.state,
                     primary_frequency=excluded.primary_frequency,
                     last_power_dbfs=excluded.last_power_dbfs,
                     last_seen_ns=excluded.last_seen_ns,
                     observation_count=excluded.observation_count,
                     confidence=excluded.confidence,
                     threat_level=excluded.threat_level,
                     modulation=excluded.modulation,
                     protocol=excluded.protocol,
                     estimated_lat=excluded.estimated_lat,
                     estimated_lon=excluded.estimated_lon,
                     location_confidence=excluded.location_confidence,
                     detecting_nodes_json=excluded.detecting_nodes_json,
                     anomaly_flags_json=excluded.anomaly_flags_json,
                     updated_ns=excluded.updated_ns""",
                (tr["emitter_id"], tr.get("state", "new"),
                 float(tr.get("primary_frequency", 0.0)),
                 tr.get("last_power_dbfs"),
                 int(tr.get("first_seen_ns", 0)),
                 int(tr.get("last_seen_ns", 0)),
                 int(tr.get("observation_count", 0)),
                 tr.get("confidence"), tr.get("threat_level"),
                 tr.get("modulation"), tr.get("protocol"),
                 tr.get("estimated_lat"), tr.get("estimated_lon"),
                 tr.get("location_confidence"),
                 json.dumps(tr.get("detecting_nodes", []) or []),
                 json.dumps(tr.get("anomaly_flags", []) or []),
                 time.time_ns()))

    def _insert_assessment_sync(self, rep: Dict[str, Any]):
        with self._lock:
            self._conn.execute(
                """INSERT INTO assessment_reports
                   (emitter_id, assessment_ns, threat_level, confidence,
                    summary, recommended_action, recommended_nodes_json,
                    escalate_to_atak, anomaly_flags_json)
                   VALUES (?,?,?,?,?,?,?,?,?)""",
                (rep["emitter_id"], int(rep.get("assessment_ns", time.time_ns())),
                 rep.get("threat_level", "unknown"),
                 rep.get("confidence", 0.0),
                 rep.get("summary"),
                 rep.get("recommended_action"),
                 json.dumps(rep.get("recommended_nodes", []) or []),
                 1 if rep.get("escalate_to_atak") else 0,
                 json.dumps(rep.get("anomaly_flags", []) or [])))

    # ── Async write API ───────────────────────────────────────────────────

    async def record_event(self, event_dict: Dict[str, Any]):
        """Append an RFEvent (idempotent on event_id collision)."""
        try:
            await asyncio.to_thread(self._insert_event_sync, event_dict)
        except Exception as exc:
            logger.warning("MissionStore: event persist failed: %s", exc)

    async def record_track(self, track_dict: Dict[str, Any]):
        """Upsert an EmitterTrack snapshot (latest wins)."""
        try:
            await asyncio.to_thread(self._upsert_track_sync, track_dict)
        except Exception as exc:
            logger.warning("MissionStore: track persist failed: %s", exc)

    async def record_assessment(self, report_dict: Dict[str, Any]):
        """Append an AssessmentReport row."""
        try:
            await asyncio.to_thread(self._insert_assessment_sync, report_dict)
        except Exception as exc:
            logger.warning("MissionStore: assessment persist failed: %s", exc)

    # ── Sync read API (used at startup, no need to be async) ──────────────

    def load_active_tracks(self, window_s: float = 86_400.0) -> List[Dict[str, Any]]:
        """Return tracks whose `last_seen_ns` is within `window_s`. Used by
        the orchestrator to rehydrate the in-memory track set after a
        restart so we don't double-create tracks for emitters we already
        knew about."""
        cutoff_ns = time.time_ns() - int(window_s * 1e9)
        with self._lock:
            cur = self._conn.execute(
                """SELECT * FROM emitter_tracks
                   WHERE last_seen_ns >= ? AND state != 'lost'
                   ORDER BY last_seen_ns DESC""",
                (cutoff_ns,))
            rows = cur.fetchall()
        out: List[Dict[str, Any]] = []
        for r in rows:
            d = dict(r)
            try:
                d["detecting_nodes"] = json.loads(d.pop("detecting_nodes_json") or "[]")
            except (TypeError, ValueError):
                d["detecting_nodes"] = []
            try:
                d["anomaly_flags"] = json.loads(d.pop("anomaly_flags_json") or "[]")
            except (TypeError, ValueError):
                d["anomaly_flags"] = []
            out.append(d)
        return out

    def event_count(self) -> int:
        with self._lock:
            cur = self._conn.execute("SELECT COUNT(*) FROM rf_events")
            return cur.fetchone()[0]

    def track_count(self) -> int:
        with self._lock:
            cur = self._conn.execute("SELECT COUNT(*) FROM emitter_tracks")
            return cur.fetchone()[0]

    def assessment_count(self) -> int:
        with self._lock:
            cur = self._conn.execute("SELECT COUNT(*) FROM assessment_reports")
            return cur.fetchone()[0]

    def close(self):
        with self._lock:
            try:
                self._conn.close()
            except Exception:
                pass
