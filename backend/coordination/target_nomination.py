"""
Operator target nomination — "everyone converges on this signal".

One operator finds the signal of interest and promotes it to THE mission
target. Nomination is a first-class manual action (unlike the automatic
anomaly/confidence candidacy engines):

* single active nominated target at a time (v1)
* persisted in the MissionStore SQLite DB so it survives a coordinator
  restart (table `op_nominated_target`, singleton row)
* side-effects on nominate:
    - registers the frequency as a known target in the SignalRepository
      (so fingerprint sweeps recognise it)
    - creates/updates a correlation rule around the frequency so
      multi-node hearings alert immediately
* side-effects reversed on clear (rule removed; the repository entry is
  kept — it is intelligence, not state)
* does NOT auto-export CoT — the manual-CoT posture is unchanged.

The manager is storage-backed but keeps an in-memory snapshot for
zero-cost reads on the hot track-serialisation path.
"""
from __future__ import annotations

import logging
import time
from typing import Any, Dict, Optional

logger = logging.getLogger(__name__)

# Correlation-rule identity for the nominated target. A fixed id means
# nominate → re-nominate naturally upserts a single rule instead of
# accumulating one rule per nomination.
NOMINATION_RULE_ID = "nominated_target"

# Half-width of the frequency window used for the correlation rule and
# for matching tracks to a frequency-based nomination. Matches the
# 25 kHz association tolerance used elsewhere (TrackManager.ingest_lob).
NOMINATION_FREQ_TOL_HZ = 25_000.0


class NominationManager:
    """Owns the single active nominated mission target."""

    def __init__(self, store=None, correlation_engine=None,
                 signal_repo=None, track_manager=None):
        self._store = store
        self._correlation = correlation_engine
        self._repo = signal_repo
        self._tracks = track_manager
        self._current: Optional[Dict[str, Any]] = None
        if store is not None:
            self._ensure_schema()
            self._rehydrate()

    # ── Persistence ──────────────────────────────────────────────────────

    def _ensure_schema(self):
        try:
            with self._store._lock:
                self._store._conn.executescript("""
                    CREATE TABLE IF NOT EXISTS op_nominated_target (
                        slot          INTEGER PRIMARY KEY CHECK (slot = 1),
                        emitter_id    TEXT,
                        frequency_hz  REAL NOT NULL,
                        label         TEXT,
                        nominated_by  TEXT,
                        nominated_ns  INTEGER NOT NULL,
                        signal_id     TEXT,
                        rule_id       TEXT
                    );
                """)
        except Exception as exc:
            logger.warning("NominationManager schema: %s", exc)

    def _rehydrate(self):
        try:
            with self._store._lock:
                row = self._store._conn.execute(
                    "SELECT * FROM op_nominated_target WHERE slot=1"
                ).fetchone()
            if row is None:
                return
            self._current = dict(row)
            self._current.pop("slot", None)
            logger.info(
                "Resumed nominated target: %.4f MHz (emitter=%s)",
                self._current["frequency_hz"] / 1e6,
                self._current.get("emitter_id"))
            # Re-assert the correlation rule in case the rules table was
            # wiped or the rule was removed while we were down. add_rule
            # is an upsert so this is idempotent.
            self._apply_correlation_rule(self._current["frequency_hz"],
                                         self._current.get("label"))
        except Exception as exc:
            logger.warning("Nomination rehydrate failed: %s", exc)

    def _persist(self):
        if self._store is None:
            return
        try:
            with self._store._lock:
                if self._current is None:
                    self._store._conn.execute(
                        "DELETE FROM op_nominated_target WHERE slot=1")
                else:
                    c = self._current
                    self._store._conn.execute(
                        """INSERT INTO op_nominated_target
                             (slot, emitter_id, frequency_hz, label,
                              nominated_by, nominated_ns, signal_id, rule_id)
                           VALUES (1,?,?,?,?,?,?,?)
                           ON CONFLICT(slot) DO UPDATE SET
                             emitter_id=excluded.emitter_id,
                             frequency_hz=excluded.frequency_hz,
                             label=excluded.label,
                             nominated_by=excluded.nominated_by,
                             nominated_ns=excluded.nominated_ns,
                             signal_id=excluded.signal_id,
                             rule_id=excluded.rule_id""",
                        (c.get("emitter_id"), c["frequency_hz"],
                         c.get("label"), c.get("nominated_by"),
                         c["nominated_ns"], c.get("signal_id"),
                         c.get("rule_id")))
        except Exception as exc:
            logger.warning("Nomination persist failed: %s", exc)

    # ── Side-effects ─────────────────────────────────────────────────────

    def _apply_correlation_rule(self, freq_hz: float,
                                label: Optional[str]) -> Optional[str]:
        if self._correlation is None:
            return None
        try:
            from backend.coordination.correlation_engine import CorrelationRule
            rule = CorrelationRule(
                rule_id=NOMINATION_RULE_ID,
                name=f"Nominated target {freq_hz / 1e6:.4f} MHz"
                     + (f" ({label})" if label else ""),
                nodes=[],                       # all nodes
                freq_lo_hz=freq_hz - NOMINATION_FREQ_TOL_HZ,
                freq_hi_hz=freq_hz + NOMINATION_FREQ_TOL_HZ,
                window_s=30.0,
                min_nodes=2,
                action="alert",
                notes="auto-created by operator target nomination",
            )
            self._correlation.add_rule(rule)
            return NOMINATION_RULE_ID
        except Exception as exc:
            logger.warning("Nomination correlation rule failed: %s", exc)
            return None

    def _remove_correlation_rule(self):
        if self._correlation is None:
            return
        try:
            self._correlation.remove_rule(NOMINATION_RULE_ID)
        except Exception as exc:
            logger.warning("Nomination rule removal failed: %s", exc)

    def _register_known_target(self, freq_hz: float,
                               emitter_id: Optional[str],
                               label: Optional[str],
                               operator: str) -> Optional[str]:
        """Add the nominated frequency to the known-target allow-list
        (SignalRepository). The entry stays after clear — intelligence
        about a signal doesn't evaporate when the operator moves on."""
        if self._repo is None:
            return None
        try:
            track = None
            if emitter_id and self._tracks is not None:
                track = self._tracks.tracks.get(emitter_id)
            return self._repo.save_signal(
                node_id="operator_nomination",
                center_hz=freq_hz,
                emitter_id=emitter_id,
                modulation=getattr(track, "modulation", None),
                protocol=getattr(track, "protocol", None),
                threat_level=getattr(track, "threat_level", None),
                estimated_lat=getattr(track, "estimated_lat", None),
                estimated_lon=getattr(track, "estimated_lon", None),
                location_method=getattr(track, "location_method", None),
                decoded_text=None,
            )
        except Exception as exc:
            logger.warning("Nomination known-target save failed: %s", exc)
            return None

    # ── Public API ───────────────────────────────────────────────────────

    def nominate(self, emitter_id: Optional[str] = None,
                 frequency_hz: Optional[float] = None,
                 label: Optional[str] = None,
                 operator: str = "operator") -> Dict[str, Any]:
        """Set the nominated mission target by track id or raw frequency.

        Raises ValueError when neither a resolvable emitter_id nor a
        positive frequency is provided. Re-nominating replaces the
        previous target (single active target, v1).
        """
        if emitter_id:
            track = (self._tracks.tracks.get(emitter_id)
                     if self._tracks is not None else None)
            if track is None:
                raise ValueError(f"track not found: {emitter_id}")
            frequency_hz = track.primary_frequency
        if not frequency_hz or frequency_hz <= 0:
            raise ValueError("emitter_id or a positive frequency_hz required")

        # v1: single active target — replacing implicitly clears the old
        # one (the rule is upserted under the fixed rule id).
        signal_id = self._register_known_target(
            frequency_hz, emitter_id, label, operator)
        rule_id = self._apply_correlation_rule(frequency_hz, label)

        self._current = {
            "emitter_id": emitter_id,
            "frequency_hz": float(frequency_hz),
            "label": label,
            "nominated_by": operator,
            "nominated_ns": time.time_ns(),
            "signal_id": signal_id,
            "rule_id": rule_id,
        }
        self._persist()
        logger.info("Target NOMINATED: %.4f MHz (emitter=%s by %s)",
                    frequency_hz / 1e6, emitter_id, operator)
        return dict(self._current)

    def clear(self) -> bool:
        """Clear the active nomination. Returns False if none was set."""
        if self._current is None:
            return False
        cleared = self._current
        self._remove_correlation_rule()
        self._current = None
        self._persist()
        logger.info("Target nomination CLEARED (was %.4f MHz)",
                    cleared["frequency_hz"] / 1e6)
        return True

    def current(self) -> Optional[Dict[str, Any]]:
        return dict(self._current) if self._current is not None else None

    def is_nominated(self, emitter_id: Optional[str],
                     frequency_hz: Optional[float] = None) -> bool:
        """True when the given track is the nominated target — by exact
        emitter id, or by frequency proximity for frequency-nominations
        (and for re-associated tracks after a restart)."""
        if self._current is None:
            return False
        if emitter_id and self._current.get("emitter_id") == emitter_id:
            return True
        if frequency_hz is not None:
            return (abs(frequency_hz - self._current["frequency_hz"])
                    <= NOMINATION_FREQ_TOL_HZ)
        return False
