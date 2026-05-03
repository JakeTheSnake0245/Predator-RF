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

SCHEMA_VERSION = 2

# Schema version history:
#   v0 → no DB
#   v1 → rf_events, emitter_tracks, assessment_reports
#   v2 → adds: missions table, mission_id FK on rf_events / emitter_tracks /
#        assessment_reports (nullable), op_friendly / op_blacklist /
#        op_manual_location override tables, op_approvals_log outcome
#        ledger, gps_age_s column on rf_events for TDOA freshness audit.
#
# Each migration step is forward-only and idempotent so an in-flight
# mission DB upgrades cleanly on the next backend restart. Operators
# never have to manually run ALTER TABLE.


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
            if current < 1:
                self._migrate_v0_to_v1()
            if current < 2:
                self._migrate_v1_to_v2()
            self._conn.execute(f"PRAGMA user_version = {SCHEMA_VERSION}")
            logger.info("MissionStore schema now at v%d", SCHEMA_VERSION)

    def _migrate_v0_to_v1(self):
        """Initial schema: events, tracks, assessments."""
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

    def _migrate_v1_to_v2(self):
        """Add mission lifecycle, operator overrides, approvals ledger,
        and gps_age_s on rf_events. ALTER TABLE ADD COLUMN is supported
        in SQLite ≥3.2; the new columns are nullable so existing rows
        are valid."""
        self._conn.executescript("""
            -- Mission lifecycle
            CREATE TABLE IF NOT EXISTS missions (
                mission_id   TEXT PRIMARY KEY,
                name         TEXT NOT NULL,
                started_ns   INTEGER NOT NULL,
                ended_ns     INTEGER,
                operator     TEXT,
                notes        TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_missions_active
                ON missions(ended_ns);

            -- Operator overrides — friendly emitters
            CREATE TABLE IF NOT EXISTS op_friendly (
                emitter_id   TEXT PRIMARY KEY,
                label        TEXT,
                added_ns     INTEGER NOT NULL,
                added_by     TEXT
            );

            -- Operator overrides — frequency blacklist
            CREATE TABLE IF NOT EXISTS op_blacklist (
                rowid        INTEGER PRIMARY KEY AUTOINCREMENT,
                start_hz     REAL NOT NULL,
                end_hz       REAL NOT NULL,
                reason       TEXT,
                added_ns     INTEGER NOT NULL
            );

            -- Operator overrides — manual location overrides
            CREATE TABLE IF NOT EXISTS op_manual_location (
                emitter_id   TEXT PRIMARY KEY,
                lat          REAL NOT NULL,
                lon          REAL NOT NULL,
                confidence   REAL NOT NULL,
                source       TEXT,
                added_ns     INTEGER NOT NULL
            );

            -- Approvals outcome ledger (not the queue itself — that's
            -- ephemeral in ApprovalQueue. Recorded here so an AAR can
            -- see who approved/rejected what and when.)
            CREATE TABLE IF NOT EXISTS op_approvals_log (
                approval_id  TEXT PRIMARY KEY,
                mission_id   TEXT,
                emitter_id   TEXT,
                state        TEXT NOT NULL,
                decided_by   TEXT,
                decided_at_ns INTEGER,
                reason       TEXT,
                payload_json TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_approvals_mission
                ON op_approvals_log(mission_id);
        """)
        # Add mission_id columns. ALTER TABLE has no IF NOT EXISTS in
        # SQLite — the migration is gated by user_version so each ALTER
        # only runs once per DB. Wrap in a try in case an aborted prior
        # migration left it half-applied.
        for stmt in (
            "ALTER TABLE rf_events           ADD COLUMN mission_id TEXT",
            "ALTER TABLE emitter_tracks      ADD COLUMN mission_id TEXT",
            "ALTER TABLE assessment_reports  ADD COLUMN mission_id TEXT",
            "ALTER TABLE rf_events           ADD COLUMN gps_age_s REAL",
            "ALTER TABLE rf_events           ADD COLUMN upstream_source TEXT",
            "ALTER TABLE emitter_tracks      ADD COLUMN upstream_source TEXT",
            # Single-node RSSI proximity fallback adds these — tells
            # the UI which renderer to use ("tdoa" tight ellipse vs
            # "rssi_proximity" wide circle) and the radius in metres.
            "ALTER TABLE emitter_tracks      ADD COLUMN location_method TEXT",
            "ALTER TABLE emitter_tracks      ADD COLUMN location_error_radius_m REAL",
        ):
            try:
                self._conn.execute(stmt)
            except sqlite3.OperationalError as exc:
                # "duplicate column name" — column already added in a
                # previous run; tolerate and continue.
                if "duplicate" not in str(exc).lower():
                    raise
        self._conn.executescript("""
            CREATE INDEX IF NOT EXISTS idx_rf_events_mission
                ON rf_events(mission_id);
            CREATE INDEX IF NOT EXISTS idx_tracks_mission
                ON emitter_tracks(mission_id);
            CREATE INDEX IF NOT EXISTS idx_assess_mission
                ON assessment_reports(mission_id);
        """)

    # ── Sync write primitives (called via asyncio.to_thread) ──────────────

    def _insert_event_sync(self, ev: Dict[str, Any]):
        with self._lock:
            self._conn.execute(
                """INSERT OR IGNORE INTO rf_events
                   (event_id, timestamp_ns, node_id, frequency, power_dbfs,
                    snr_db, bandwidth_hz, detector, modulation, protocol,
                    decoded_payload, node_lat, node_lon, node_alt_m,
                    node_trust_score, raw_json,
                    mission_id, gps_age_s, upstream_source)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (ev.get("event_id"), int(ev.get("timestamp_ns", 0)),
                 ev.get("node_id", ""), float(ev.get("frequency", 0.0)),
                 float(ev.get("power_dbfs", 0.0)), float(ev.get("snr_db", 0.0)),
                 ev.get("bandwidth_hz"), ev.get("detector"),
                 ev.get("modulation"), ev.get("protocol"),
                 ev.get("decoded_payload"),
                 ev.get("node_lat"), ev.get("node_lon"), ev.get("node_alt_m"),
                 ev.get("node_trust_score"),
                 json.dumps(ev, default=str),
                 ev.get("mission_id") or self._current_mission_id(),
                 ev.get("gps_age_s"),
                 ev.get("upstream_source")))

    def _upsert_track_sync(self, tr: Dict[str, Any]):
        with self._lock:
            self._conn.execute(
                """INSERT INTO emitter_tracks
                   (emitter_id, state, primary_frequency, last_power_dbfs,
                    first_seen_ns, last_seen_ns, observation_count,
                    confidence, threat_level, modulation, protocol,
                    estimated_lat, estimated_lon, location_confidence,
                    detecting_nodes_json, anomaly_flags_json, updated_ns,
                    mission_id, upstream_source,
                    location_method, location_error_radius_m)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
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
                     updated_ns=excluded.updated_ns,
                     mission_id=COALESCE(excluded.mission_id, mission_id),
                     upstream_source=COALESCE(excluded.upstream_source,
                                              upstream_source),
                     location_method=excluded.location_method,
                     location_error_radius_m=excluded.location_error_radius_m""",
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
                 time.time_ns(),
                 tr.get("mission_id") or self._current_mission_id(),
                 tr.get("upstream_source"),
                 tr.get("location_method"),
                 tr.get("location_error_radius_m")))

    def _insert_assessment_sync(self, rep: Dict[str, Any]):
        with self._lock:
            self._conn.execute(
                """INSERT INTO assessment_reports
                   (emitter_id, assessment_ns, threat_level, confidence,
                    summary, recommended_action, recommended_nodes_json,
                    escalate_to_atak, anomaly_flags_json, mission_id)
                   VALUES (?,?,?,?,?,?,?,?,?,?)""",
                (rep["emitter_id"], int(rep.get("assessment_ns", time.time_ns())),
                 rep.get("threat_level", "unknown"),
                 rep.get("confidence", 0.0),
                 rep.get("summary"),
                 rep.get("recommended_action"),
                 json.dumps(rep.get("recommended_nodes", []) or []),
                 1 if rep.get("escalate_to_atak") else 0,
                 json.dumps(rep.get("anomaly_flags", []) or []),
                 rep.get("mission_id") or self._current_mission_id()))

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

    # ── Tier 4 read-side helpers (Android pull / CoT bulk export) ────
    # These are READ-ONLY queries the polling endpoints depend on.
    # Kept here next to the table definitions so the column lists
    # don't drift if a future migration renames things.

    def _fetch_events_since_sync(self, since_ns: int, limit: int
                                  ) -> List[Dict[str, Any]]:
        with self._lock:
            cur = self._conn.execute(
                "SELECT event_id, timestamp_ns, node_id, frequency, "
                "       power_dbfs, snr_db, bandwidth_hz, detector, "
                "       modulation, protocol, node_lat, node_lon, "
                "       node_trust_score, mission_id, upstream_source "
                "FROM rf_events "
                "WHERE timestamp_ns > ? "
                "ORDER BY timestamp_ns ASC "
                "LIMIT ?",
                (int(since_ns), int(limit)))
            cols = [c[0] for c in cur.description]
            return [dict(zip(cols, row)) for row in cur.fetchall()]

    async def fetch_events_since(self, *, since_ns: int, limit: int = 200
                                  ) -> List[Dict[str, Any]]:
        """Delta read for the Android polling endpoint. Strictly
        `timestamp_ns > since_ns` so the cursor returned to the client
        (server-now) is never re-included on the next poll."""
        return await asyncio.to_thread(
            self._fetch_events_since_sync, since_ns, limit)

    def _latest_assessments_sync(self) -> Dict[str, Dict[str, Any]]:
        with self._lock:
            # MAX(assessment_ns) per emitter — the most recent verdict
            # wins. JOIN back to the row to get the full assessment.
            cur = self._conn.execute("""
                SELECT a.emitter_id, a.assessment_ns, a.threat_level,
                       a.confidence, a.summary, a.recommended_action,
                       a.escalate_to_atak
                FROM assessment_reports a
                JOIN (SELECT emitter_id, MAX(assessment_ns) AS ts
                      FROM assessment_reports
                      GROUP BY emitter_id) latest
                  ON a.emitter_id = latest.emitter_id
                 AND a.assessment_ns = latest.ts
            """)
            cols = [c[0] for c in cur.description]
            out: Dict[str, Dict[str, Any]] = {}
            for row in cur.fetchall():
                d = dict(zip(cols, row))
                d["escalate_to_atak"] = bool(d.get("escalate_to_atak"))
                out[d["emitter_id"]] = d
            return out

    async def latest_assessments(self) -> Dict[str, Dict[str, Any]]:
        """Map of emitter_id → most recent AssessmentReport. Used by
        the bulk CoT export endpoint to filter to escalating tracks."""
        return await asyncio.to_thread(self._latest_assessments_sync)

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

    # ── v2: mission_id provider ────────────────────────────────────────
    # The orchestrator owns the current-mission concept; it injects a
    # callable here so each event/track/assessment write picks up the
    # active mission_id automatically. Tests / standalone use leave
    # this unset → mission_id stays NULL on those rows, which is fine.
    _mission_provider = None  # type: ignore

    def set_mission_provider(self, fn):
        """fn() -> Optional[str] returning the active mission_id."""
        self._mission_provider = fn

    def _current_mission_id(self):
        if self._mission_provider is None:
            return None
        try:
            return self._mission_provider()
        except Exception:
            return None

    # ── v2: missions ───────────────────────────────────────────────────
    async def upsert_mission(self, m: Dict[str, Any]):
        try:
            await asyncio.to_thread(self._upsert_mission_sync, m)
        except Exception as exc:
            logger.warning("MissionStore: mission upsert failed: %s", exc)

    def _upsert_mission_sync(self, m: Dict[str, Any]):
        with self._lock:
            self._conn.execute(
                """INSERT INTO missions
                     (mission_id, name, started_ns, ended_ns, operator, notes)
                   VALUES (?,?,?,?,?,?)
                   ON CONFLICT(mission_id) DO UPDATE SET
                     name=excluded.name,
                     ended_ns=excluded.ended_ns,
                     operator=excluded.operator,
                     notes=excluded.notes""",
                (m["mission_id"], m["name"], int(m["started_ns"]),
                 m.get("ended_ns"), m.get("operator"), m.get("notes")))

    def load_missions(self) -> List[Dict[str, Any]]:
        with self._lock:
            cur = self._conn.execute(
                "SELECT * FROM missions ORDER BY started_ns DESC")
            return [dict(r) for r in cur.fetchall()]

    def export_mission(self, mission_id: str) -> Dict[str, Any]:
        """Bundle a single mission's events/tracks/assessments into a
        plain dict. Caller serializes (json/jsonl/tar). Used by the
        /api/v1/missions/{id}/export route for after-action review."""
        with self._lock:
            mission = self._conn.execute(
                "SELECT * FROM missions WHERE mission_id=?",
                (mission_id,)).fetchone()
            if mission is None:
                return {}
            ev = self._conn.execute(
                "SELECT * FROM rf_events WHERE mission_id=? "
                "ORDER BY timestamp_ns", (mission_id,)).fetchall()
            tr = self._conn.execute(
                "SELECT * FROM emitter_tracks WHERE mission_id=?",
                (mission_id,)).fetchall()
            ar = self._conn.execute(
                "SELECT * FROM assessment_reports WHERE mission_id=? "
                "ORDER BY assessment_ns", (mission_id,)).fetchall()
            apl = self._conn.execute(
                "SELECT * FROM op_approvals_log WHERE mission_id=?",
                (mission_id,)).fetchall()
        return {
            "mission":     dict(mission),
            "events":      [dict(r) for r in ev],
            "tracks":      [dict(r) for r in tr],
            "assessments": [dict(r) for r in ar],
            "approvals":   [dict(r) for r in apl],
        }

    # ── v2: overrides ──────────────────────────────────────────────────
    async def upsert_override(self, category: str, row: Dict[str, Any]):
        try:
            await asyncio.to_thread(
                self._upsert_override_sync, category, row)
        except Exception as exc:
            logger.warning("MissionStore: override upsert failed: %s", exc)

    def _upsert_override_sync(self, category: str, row: Dict[str, Any]):
        with self._lock:
            if category == "friendly":
                self._conn.execute(
                    """INSERT INTO op_friendly
                         (emitter_id, label, added_ns, added_by)
                       VALUES (?,?,?,?)
                       ON CONFLICT(emitter_id) DO UPDATE SET
                         label=excluded.label,
                         added_ns=excluded.added_ns,
                         added_by=excluded.added_by""",
                    (row["emitter_id"], row.get("label"),
                     int(row.get("added_ns", time.time_ns())),
                     row.get("added_by") or "operator"))
            elif category == "blacklist":
                self._conn.execute(
                    """INSERT INTO op_blacklist
                         (start_hz, end_hz, reason, added_ns)
                       VALUES (?,?,?,?)""",
                    (float(row["start_hz"]), float(row["end_hz"]),
                     row.get("reason"),
                     int(row.get("added_ns", time.time_ns()))))
            elif category == "manual_location":
                self._conn.execute(
                    """INSERT INTO op_manual_location
                         (emitter_id, lat, lon, confidence, source, added_ns)
                       VALUES (?,?,?,?,?,?)
                       ON CONFLICT(emitter_id) DO UPDATE SET
                         lat=excluded.lat, lon=excluded.lon,
                         confidence=excluded.confidence,
                         source=excluded.source,
                         added_ns=excluded.added_ns""",
                    (row["emitter_id"], float(row["lat"]),
                     float(row["lon"]), float(row.get("confidence", 0.95)),
                     row.get("source") or "operator",
                     int(row.get("added_ns", time.time_ns()))))

    async def delete_override(self, category: str, key: str):
        try:
            await asyncio.to_thread(self._delete_override_sync, category, key)
        except Exception as exc:
            logger.warning("MissionStore: override delete failed: %s", exc)

    def _delete_override_sync(self, category: str, key: str):
        table = {"friendly": "op_friendly",
                 "manual_location": "op_manual_location"}.get(category)
        if not table:
            return
        with self._lock:
            self._conn.execute(
                f"DELETE FROM {table} WHERE emitter_id=?", (key,))

    async def clear_overrides(self, category: str):
        try:
            await asyncio.to_thread(self._clear_overrides_sync, category)
        except Exception as exc:
            logger.warning("MissionStore: override clear failed: %s", exc)

    def _clear_overrides_sync(self, category: str):
        table = {"friendly": "op_friendly",
                 "blacklist": "op_blacklist",
                 "manual_location": "op_manual_location"}.get(category)
        if not table:
            return
        with self._lock:
            self._conn.execute(f"DELETE FROM {table}")

    def load_overrides(self) -> Dict[str, List[Dict[str, Any]]]:
        with self._lock:
            f = [dict(r) for r in self._conn.execute(
                "SELECT * FROM op_friendly").fetchall()]
            b = [dict(r) for r in self._conn.execute(
                "SELECT * FROM op_blacklist").fetchall()]
            m = [dict(r) for r in self._conn.execute(
                "SELECT * FROM op_manual_location").fetchall()]
        return {"friendly": f, "blacklist": b, "manual_location": m}

    # ── v2: approvals outcome log ──────────────────────────────────────
    async def record_approval(self, approval_dict: Dict[str, Any]):
        try:
            await asyncio.to_thread(
                self._record_approval_sync, approval_dict)
        except Exception as exc:
            logger.warning("MissionStore: approval log failed: %s", exc)

    def _record_approval_sync(self, a: Dict[str, Any]):
        import json as _json
        with self._lock:
            self._conn.execute(
                """INSERT INTO op_approvals_log
                     (approval_id, mission_id, emitter_id, state,
                      decided_by, decided_at_ns, reason, payload_json)
                   VALUES (?,?,?,?,?,?,?,?)
                   ON CONFLICT(approval_id) DO UPDATE SET
                     state=excluded.state,
                     decided_by=excluded.decided_by,
                     decided_at_ns=excluded.decided_at_ns,
                     reason=excluded.reason""",
                (a["approval_id"],
                 a.get("mission_id") or self._current_mission_id(),
                 a.get("emitter_id"), a.get("state", "pending"),
                 a.get("decided_by"), a.get("decided_at_ns"),
                 a.get("reason"), _json.dumps(a, default=str)))
