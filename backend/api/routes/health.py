"""
Health, readiness, and metrics endpoints.

Three distinct concepts so a watchdog (systemd, k8s, Prom scraper)
can tell them apart:
  /healthz   — process is alive (always 200 unless we're crashing)
  /readyz    — process is ready to serve traffic (200 only when the
               fleet has ≥1 node OR CoC is up AND DB is writable)
  /metrics   — Prometheus text-format scrape

These are intentionally NOT gated by the bearer-token middleware
so a Prom scraper / load balancer can hit them without shipping the
secret. The /metrics body itself is non-sensitive (counters and gauges,
no payload data).
"""
from __future__ import annotations

import os
import time
from typing import Any, Dict

from fastapi import APIRouter, Response

from backend.observability.metrics import metrics

router = APIRouter()

# Injected by api/server.py
backend_ref = None  # PredatorBackend


@router.get("/healthz")
async def healthz() -> Dict[str, Any]:
    """Liveness probe — does NOT verify dependencies. Used to decide
    whether to restart the process."""
    return {"status": "ok", "ts": int(time.time())}


@router.get("/readyz")
async def readyz(response: Response) -> Dict[str, Any]:
    """Readiness probe — verifies the backend can actually serve work.
    Returns 503 + a `not_ready` reason list when any check fails."""
    reasons = []
    backend = backend_ref
    if backend is None:
        response.status_code = 503
        return {"status": "not_ready",
                "reasons": ["backend not initialised"]}

    has_fleet = backend.fleet_manager.node_count() > 0
    has_coc = backend.coc is not None
    if not (has_fleet or has_coc):
        reasons.append("no fleet nodes and CoC mode is off")

    # DB writable? Try a no-op write.
    if backend.store is not None:
        try:
            backend.store._conn.execute(
                "CREATE TABLE IF NOT EXISTS _readyz_probe (ts INTEGER)")
        except Exception as exc:
            reasons.append(f"db not writable: {exc}")

    if reasons:
        response.status_code = 503
        return {"status": "not_ready", "reasons": reasons}
    return {
        "status": "ready",
        "fleet_nodes": backend.fleet_manager.node_count(),
        "coc_mode": has_coc,
        "active_tracks": len(backend.track_manager.tracks),
        "mission":
            backend.missions.active_id if hasattr(backend, "missions")
            else None,
    }


@router.get("/metrics")
async def metrics_endpoint() -> Response:
    """Prometheus text exposition. Refreshes the gauge values from the
    backend before rendering — counters live in the registry already."""
    backend = backend_ref
    if backend is not None:
        metrics.gauge("predator_active_tracks",
                      len(backend.track_manager.tracks),
                      help_text="Currently-active EmitterTracks")
        metrics.gauge("predator_fleet_nodes",
                      backend.fleet_manager.node_count(),
                      help_text="Connected sensor nodes")
        metrics.gauge("predator_pending_tasks",
                      len(backend._pending_tasks),
                      help_text="In-flight async tasks awaiting drain")
        if backend.cot is not None:
            stats = backend.cot.stats()
            metrics.gauge("predator_cot_sent_total", stats.get("sent", 0),
                          help_text="CoT datagrams sent since startup")
            metrics.gauge("predator_cot_errors_total",
                          stats.get("errors", 0),
                          help_text="CoT send errors since startup")
        if backend.auto_tasker is not None:
            s = backend.auto_tasker.stats()
            for k, v in s.items():
                if isinstance(v, (int, float)) and not isinstance(v, bool):
                    metrics.gauge(f"predator_auto_tasker_{k}", v)
        if hasattr(backend, "approvals") and backend.approvals is not None:
            for k, v in backend.approvals.stats().items():
                metrics.gauge(f"predator_approvals_{k}", v)
        if hasattr(backend, "dedup") and backend.dedup is not None:
            metrics.gauge("predator_dedup_merges_total",
                          backend.dedup.merges_total)
    return Response(content=metrics.render(),
                    media_type="text/plain; version=0.0.4")
