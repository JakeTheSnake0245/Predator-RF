"""GET /api/v1/android-pull — single-shot snapshot tuned for the Android
client on a flaky link.

The Android app polls this every N seconds (default 5) instead of
juggling SSE on a phone. Returns ONLY what changed since the client's
`since_ns` cursor, so a phone on EDGE can keep up with a few KB/poll.

Schema is versioned (`schema=2`) so an older Android build can still
parse newer payloads — clients MUST ignore unknown fields.

Payload (see docs/ANDROID_INTEGRATION.md for the full contract):

    {
      "schema": 2,
      "server_ts_ns": <int>,
      "cursor": <int>,                # echo back as ?since_ns=
      "mission": {...} | null,
      "nodes": [...],                 # always full — small + critical
      "tracks": [...],                # delta: only updated since cursor
      "events": [...],                # delta: capped at ?max_events
      "approvals_pending": [...],     # always full — operator-critical
      "preflight_go": true|false      # cached; refreshed every 30s
    }
"""
from __future__ import annotations

import logging
import time
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)

# Injected by server.py (same pattern as the other routes).
track_manager = None
fleet_manager = None
backend_ref = None

# Cached preflight result so the polling endpoint stays cheap.
_preflight_cache: Dict[str, Any] = {"ts": 0.0, "go": True}
_PREFLIGHT_TTL_S = 30.0


try:
    from fastapi import APIRouter, Query
    router = APIRouter()

    @router.get("")
    async def android_pull(
        since_ns: int = Query(0, ge=0,
            description="Server-ts cursor returned by previous poll. 0 = full snapshot."),
        max_events: int = Query(200, ge=1, le=2000,
            description="Cap on RF events returned in this poll (cheap + bounded)."),
        include_history: bool = Query(False,
            description="If true, include per-track frequency_history (large)."),
    ) -> Dict[str, Any]:
        return await _build_snapshot(
            since_ns=since_ns,
            max_events=max_events,
            include_history=include_history,
        )

except ImportError:
    router = None  # type: ignore


async def _build_snapshot(*, since_ns: int, max_events: int,
                           include_history: bool) -> Dict[str, Any]:
    """Pure function so unit tests can call it without a running app."""
    now_ns = time.time_ns()

    # ── Mission (light) ────────────────────────────────────────────
    mission: Optional[Dict[str, Any]] = None
    if backend_ref is not None and hasattr(backend_ref, "missions"):
        try:
            active_id = backend_ref.missions.active_id
            if active_id:
                m = backend_ref.missions.get(active_id)
                if m is not None:
                    mission = {
                        "mission_id": active_id,
                        "name": getattr(m, "name", None),
                        "operator": getattr(m, "operator", None),
                        "started_ts_ns": getattr(m, "started_ts_ns", None),
                    }
        except Exception as exc:
            logger.warning("android-pull mission read failed: %s", exc)

    # ── Nodes (always full — they're tiny and the operator NEEDS
    #    every node's GPS / trust state every poll) ─────────────────
    nodes: List[Dict[str, Any]] = []
    if fleet_manager is not None:
        try:
            for node in fleet_manager.all_nodes():
                nodes.append({
                    "node_id": getattr(node, "node_id", None),
                    "hardware_code": getattr(node, "hardware_code", None),
                    "trust_score": getattr(node, "trust_score", None),
                    "gps_lock": getattr(node, "gps_lock", None),
                    "gps_age_s": getattr(node, "gps_age_s", None),
                    "lat": getattr(node, "lat", None),
                    "lon": getattr(node, "lon", None),
                    "last_seen_ns": getattr(node, "last_seen_ns", None),
                })
        except Exception as exc:
            logger.warning("android-pull nodes read failed: %s", exc)

    # ── Tracks (delta only) ────────────────────────────────────────
    tracks: List[Dict[str, Any]] = []
    if track_manager is not None:
        try:
            for t in track_manager.tracks.values():
                if int(getattr(t, "last_seen_ns", 0)) <= since_ns:
                    continue
                d = t.to_dict() if hasattr(t, "to_dict") else {}
                if not include_history:
                    # Histories can be 100s of floats per track; the
                    # phone almost never wants them. Caller can re-pull
                    # via /api/v1/tracks/<id>/history when zooming in.
                    d.pop("frequency_history", None)
                    d.pop("power_history", None)
                    d.pop("confidence_history", None)
                tracks.append(d)
        except Exception as exc:
            logger.warning("android-pull tracks read failed: %s", exc)

    # ── Events (delta + cap) ───────────────────────────────────────
    events: List[Dict[str, Any]] = []
    if backend_ref is not None and hasattr(backend_ref, "store"):
        try:
            store = backend_ref.store
            if store is not None and hasattr(store, "fetch_events_since"):
                events = await store.fetch_events_since(
                    since_ns=since_ns, limit=max_events)
        except Exception as exc:
            logger.warning("android-pull events read failed: %s", exc)

    # ── Pending approvals (always full — operator-critical) ────────
    approvals: List[Dict[str, Any]] = []
    if backend_ref is not None and hasattr(backend_ref, "approvals"):
        try:
            q = backend_ref.approvals
            if q is not None and hasattr(q, "list_pending"):
                approvals = q.list_pending()
        except Exception as exc:
            logger.warning("android-pull approvals read failed: %s", exc)

    # ── Cached preflight GO/NO-GO ──────────────────────────────────
    pf_go = _preflight_cache.get("go", True)
    if (time.time() - _preflight_cache["ts"]) > _PREFLIGHT_TTL_S:
        try:
            from deploy.preflight import run_all
            report = await run_all(allow_lab=True)
            pf_go = bool(report.get("go", True))
            _preflight_cache["go"] = pf_go
            _preflight_cache["ts"] = time.time()
        except Exception as exc:
            logger.debug("android-pull preflight refresh failed: %s", exc)

    return {
        "schema": 2,
        "server_ts_ns": now_ns,
        "cursor": now_ns,
        "mission": mission,
        "nodes": nodes,
        "tracks": tracks,
        "events": events,
        "approvals_pending": approvals,
        "preflight_go": pf_go,
    }
