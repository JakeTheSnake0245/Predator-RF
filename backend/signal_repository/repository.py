"""
SignalRepository — three-tier signal store.

Tier 1  Metadata: every detected signal, always.
Tier 2  Fingerprint: compact spectral feature vector when track reaches STABLE.
Tier 3  IQ capture: raw samples on explicit operator demand (writes a .cs8 file
        and stores the path + duration in the DB).

All writes are async-safe via asyncio.to_thread (same pattern as MissionStore).
Schema lives in the same SQLite DB as the mission store (separate tables, same
file) so the AAR export bundle covers everything in one tarball.

Public API:
    repo.save_signal(event, fingerprint_vec=None) → signal_id
    repo.save_fingerprint(signal_id, vec)
    repo.record_iq_capture(signal_id, path, duration_s)
    repo.search(freq_hz=None, freq_tol_hz=None, after_ns=None, before_ns=None,
                modulation=None, threat_level=None, lat=None, lon=None,
                radius_m=None, limit=100) → List[dict]
    repo.find_similar(fingerprint_vec, threshold=None, limit=10) → List[dict]
    repo.get_fingerprint(signal_id) → List[float] | None
"""
from __future__ import annotations

import asyncio
import json
import logging
import math
import os
import sqlite3
import threading
import time
import uuid
from typing import Any, Dict, List, Optional, Sequence

from .fingerprinter import SignalFingerprinter, MATCH_THRESHOLD

logger = logging.getLogger(__name__)

REPO_SCHEMA_VERSION = 1


class SignalRepository:
    def __init__(self, db_path: str, iq_dir: str = "/var/lib/predator-rf/iq"):
        self._db_path = db_path
        self._iq_dir  = iq_dir
        os.makedirs(iq_dir, exist_ok=True)
        self._lock   = threading.Lock()
        self._conn   = sqlite3.connect(db_path, check_same_thread=False,
                                        isolation_level=None)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=NORMAL")
        self._fp = SignalFingerprinter()
        self._migrate()

    def _migrate(self):
        with self._lock:
            cur = self._conn.execute("PRAGMA user_version")
            if cur.fetchone()[0] >= REPO_SCHEMA_VERSION:
                return
            self._conn.executescript("""
                CREATE TABLE IF NOT EXISTS signal_repository (
                    signal_id         TEXT PRIMARY KEY,
                    mission_id        TEXT,
                    emitter_id        TEXT,
                    node_id           TEXT NOT NULL,
                    first_seen_ns     INTEGER NOT NULL,
                    last_seen_ns      INTEGER NOT NULL,
                    center_hz         REAL NOT NULL,
                    bandwidth_hz      REAL,
                    power_dbfs        REAL,
                    snr_db            REAL,
                    modulation        TEXT,
                    protocol          TEXT,
                    decoded_text      TEXT,
                    threat_level      TEXT,
                    node_lat          REAL,
                    node_lon          REAL,
                    estimated_lat     REAL,
                    estimated_lon     REAL,
                    location_method   TEXT,
                    observation_count INTEGER DEFAULT 1,
                    notes             TEXT
                );
                CREATE INDEX IF NOT EXISTS idx_repo_freq
                    ON signal_repository(center_hz);
                CREATE INDEX IF NOT EXISTS idx_repo_time
                    ON signal_repository(first_seen_ns);
                CREATE INDEX IF NOT EXISTS idx_repo_emitter
                    ON signal_repository(emitter_id);

                CREATE TABLE IF NOT EXISTS signal_fingerprints (
                    signal_id         TEXT PRIMARY KEY REFERENCES signal_repository(signal_id),
                    fingerprint_blob  BLOB NOT NULL,
                    fp_dim            INTEGER NOT NULL,
                    computed_ns       INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS iq_captures (
                    capture_id        TEXT PRIMARY KEY,
                    signal_id         TEXT REFERENCES signal_repository(signal_id),
                    file_path         TEXT NOT NULL,
                    duration_s        REAL NOT NULL,
                    sample_rate_hz    REAL,
                    center_hz         REAL,
                    format            TEXT DEFAULT 'cs8',
                    captured_ns       INTEGER NOT NULL,
                    captured_by       TEXT
                );
                CREATE INDEX IF NOT EXISTS idx_iq_signal
                    ON iq_captures(signal_id);

                CREATE TABLE IF NOT EXISTS correlated_intercepts (
                    intercept_id      TEXT PRIMARY KEY,
                    signal_id         TEXT REFERENCES signal_repository(signal_id),
                    rule_id           TEXT,
                    node_ids_json     TEXT NOT NULL,
                    center_hz         REAL NOT NULL,
                    first_detected_ns INTEGER NOT NULL,
                    fix_lat           REAL,
                    fix_lon           REAL,
                    fix_radius_m      REAL,
                    fix_method        TEXT,
                    notes             TEXT
                );
                CREATE INDEX IF NOT EXISTS idx_intercept_signal
                    ON correlated_intercepts(signal_id);
                CREATE INDEX IF NOT EXISTS idx_intercept_time
                    ON correlated_intercepts(first_detected_ns);
            """)
            self._conn.execute(f"PRAGMA user_version = {REPO_SCHEMA_VERSION}")
            logger.info("SignalRepository schema v%d initialised", REPO_SCHEMA_VERSION)

    def _fp_to_blob(self, vec: List[float]) -> bytes:
        import struct
        return struct.pack(f"{len(vec)}f", *vec)

    def _blob_to_fp(self, blob: bytes) -> List[float]:
        import struct
        n = len(blob) // 4
        return list(struct.unpack(f"{n}f", blob))

    def save_signal(self,
                    node_id: str,
                    center_hz: float,
                    *,
                    signal_id: Optional[str] = None,
                    mission_id: Optional[str] = None,
                    emitter_id: Optional[str] = None,
                    bandwidth_hz: Optional[float] = None,
                    power_dbfs: Optional[float] = None,
                    snr_db: Optional[float] = None,
                    modulation: Optional[str] = None,
                    protocol: Optional[str] = None,
                    decoded_text: Optional[str] = None,
                    threat_level: Optional[str] = None,
                    node_lat: Optional[float] = None,
                    node_lon: Optional[float] = None,
                    estimated_lat: Optional[float] = None,
                    estimated_lon: Optional[float] = None,
                    location_method: Optional[str] = None,
                    fingerprint_vec: Optional[List[float]] = None) -> str:
        sid  = signal_id or str(uuid.uuid4())
        now  = time.time_ns()
        with self._lock:
            self._conn.execute("""
                INSERT OR REPLACE INTO signal_repository
                (signal_id, mission_id, emitter_id, node_id, first_seen_ns,
                 last_seen_ns, center_hz, bandwidth_hz, power_dbfs, snr_db,
                 modulation, protocol, decoded_text, threat_level,
                 node_lat, node_lon, estimated_lat, estimated_lon,
                 location_method, observation_count)
                VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,
                  COALESCE((SELECT observation_count+1 FROM signal_repository
                             WHERE signal_id=?), 1))
            """, (sid, mission_id, emitter_id, node_id, now, now,
                  center_hz, bandwidth_hz, power_dbfs, snr_db,
                  modulation, protocol, decoded_text, threat_level,
                  node_lat, node_lon, estimated_lat, estimated_lon,
                  location_method, sid))
        if fingerprint_vec:
            self.save_fingerprint(sid, fingerprint_vec)
        return sid

    async def async_save_signal(self, *args, **kwargs) -> str:
        return await asyncio.to_thread(self.save_signal, *args, **kwargs)

    def save_fingerprint(self, signal_id: str, vec: List[float]) -> None:
        blob = self._fp_to_blob(vec)
        now  = time.time_ns()
        with self._lock:
            self._conn.execute("""
                INSERT OR REPLACE INTO signal_fingerprints
                (signal_id, fingerprint_blob, fp_dim, computed_ns)
                VALUES (?,?,?,?)
            """, (signal_id, blob, len(vec), now))

    async def async_save_fingerprint(self, signal_id: str, vec: List[float]) -> None:
        await asyncio.to_thread(self.save_fingerprint, signal_id, vec)

    def get_fingerprint(self, signal_id: str) -> Optional[List[float]]:
        with self._lock:
            row = self._conn.execute(
                "SELECT fingerprint_blob FROM signal_fingerprints WHERE signal_id=?",
                (signal_id,)).fetchone()
        return self._blob_to_fp(row[0]) if row else None

    def record_iq_capture(self, signal_id: str, file_path: str, duration_s: float,
                          sample_rate_hz: Optional[float] = None,
                          center_hz: Optional[float] = None,
                          captured_by: Optional[str] = None) -> str:
        cid = str(uuid.uuid4())
        now = time.time_ns()
        with self._lock:
            self._conn.execute("""
                INSERT INTO iq_captures
                (capture_id, signal_id, file_path, duration_s, sample_rate_hz,
                 center_hz, captured_ns, captured_by)
                VALUES (?,?,?,?,?,?,?,?)
            """, (cid, signal_id, file_path, duration_s, sample_rate_hz,
                  center_hz, now, captured_by))
        return cid

    def save_correlated_intercept(self,
                                   signal_id: str,
                                   node_ids: List[str],
                                   center_hz: float,
                                   rule_id: Optional[str] = None,
                                   fix_lat: Optional[float] = None,
                                   fix_lon: Optional[float] = None,
                                   fix_radius_m: Optional[float] = None,
                                   fix_method: Optional[str] = None,
                                   notes: Optional[str] = None) -> str:
        iid = str(uuid.uuid4())
        now = time.time_ns()
        with self._lock:
            self._conn.execute("""
                INSERT INTO correlated_intercepts
                (intercept_id, signal_id, rule_id, node_ids_json, center_hz,
                 first_detected_ns, fix_lat, fix_lon, fix_radius_m, fix_method, notes)
                VALUES (?,?,?,?,?,?,?,?,?,?,?)
            """, (iid, signal_id, rule_id, json.dumps(node_ids), center_hz,
                  now, fix_lat, fix_lon, fix_radius_m, fix_method, notes))
        return iid

    def search(self,
               freq_hz: Optional[float] = None,
               freq_tol_hz: float = 25_000.0,
               after_ns: Optional[int] = None,
               before_ns: Optional[int] = None,
               modulation: Optional[str] = None,
               threat_level: Optional[str] = None,
               lat: Optional[float] = None,
               lon: Optional[float] = None,
               radius_m: Optional[float] = None,
               limit: int = 100) -> List[Dict]:
        clauses, params = [], []
        if freq_hz is not None:
            clauses.append("center_hz BETWEEN ? AND ?")
            params += [freq_hz - freq_tol_hz, freq_hz + freq_tol_hz]
        if after_ns is not None:
            clauses.append("last_seen_ns >= ?")
            params.append(after_ns)
        if before_ns is not None:
            clauses.append("first_seen_ns <= ?")
            params.append(before_ns)
        if modulation:
            clauses.append("modulation = ?")
            params.append(modulation)
        if threat_level:
            clauses.append("threat_level = ?")
            params.append(threat_level)

        where = ("WHERE " + " AND ".join(clauses)) if clauses else ""
        sql = f"""
            SELECT s.*, f.fingerprint_blob IS NOT NULL AS has_fingerprint
            FROM signal_repository s
            LEFT JOIN signal_fingerprints f USING (signal_id)
            {where}
            ORDER BY last_seen_ns DESC
            LIMIT ?
        """
        params.append(limit * 3 if (lat and lon and radius_m) else limit)

        with self._lock:
            rows = self._conn.execute(sql, params).fetchall()

        results = [dict(r) for r in rows]

        if lat is not None and lon is not None and radius_m is not None:
            results = [r for r in results
                       if r.get("node_lat") and r.get("node_lon") and
                          _haversine_m(lat, lon, r["node_lat"], r["node_lon"]) <= radius_m]
            results = results[:limit]

        for r in results:
            r.pop("fingerprint_blob", None)

        return results

    def find_similar(self,
                     fingerprint_vec: List[float],
                     threshold: Optional[float] = None,
                     limit: int = 10) -> List[Dict]:
        if threshold is None:
            threshold = MATCH_THRESHOLD
        with self._lock:
            rows = self._conn.execute("""
                SELECT s.signal_id, s.center_hz, s.modulation, s.threat_level,
                       s.first_seen_ns, s.last_seen_ns, s.node_id,
                       f.fingerprint_blob
                FROM signal_fingerprints f
                JOIN signal_repository s USING (signal_id)
                ORDER BY s.last_seen_ns DESC
                LIMIT 2000
            """).fetchall()

        scored = []
        for row in rows:
            blob = row["fingerprint_blob"]
            if not blob:
                continue
            stored_vec = self._blob_to_fp(blob)
            sim = self._fp.similarity(fingerprint_vec, stored_vec)
            if sim >= threshold:
                scored.append({
                    "signal_id":   row["signal_id"],
                    "center_hz":   row["center_hz"],
                    "modulation":  row["modulation"],
                    "threat_level":row["threat_level"],
                    "first_seen_ns": row["first_seen_ns"],
                    "last_seen_ns":  row["last_seen_ns"],
                    "node_id":     row["node_id"],
                    "similarity":  round(sim, 4),
                })

        scored.sort(key=lambda x: x["similarity"], reverse=True)
        return scored[:limit]


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    R = 6_371_000.0
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2) ** 2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
