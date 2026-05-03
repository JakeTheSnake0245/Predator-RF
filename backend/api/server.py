"""
Predator-SDR Python backend REST API.
Exposes tracks, nodes, events, assessments, missions, approvals,
overrides, and observability over HTTP/JSON.
"""

import logging
from typing import Optional

logger = logging.getLogger(__name__)


def create_app(track_manager=None, fleet_manager=None,
               decision_engine=None, backend=None, rns_daemon=None):
    """
    Create the FastAPI application.

    Args:
        track_manager: TrackManager instance (injected)
        fleet_manager: KujhadFleetManager instance (injected)
        decision_engine: DecisionEngine instance (injected)
        backend: full PredatorBackend reference for routes that need
                 access to multiple subsystems (health, missions,
                 approvals, overrides).
    """
    try:
        from fastapi import FastAPI
        from fastapi.middleware.cors import CORSMiddleware
        from starlette.middleware.base import BaseHTTPMiddleware
    except ImportError:
        raise RuntimeError("FastAPI not installed. Run: pip install fastapi uvicorn")

    from backend.config import config
    from backend.api.routes import (
        tracks, nodes, events, assessments,
        health, missions, approvals, overrides,
        preflight, android_pull, cot_export, rns)
    from backend.api.middleware.auth import make_bearer_middleware

    app = FastAPI(
        title="Predator-SDR Backend API",
        description="RF intelligence fusion backend for Predator-SDR sensor network",
        version="2.0.0",
    )

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # Bearer-token auth — no-op when API_BEARER_TOKEN is empty.
    # Logged loudly at startup so the operator can't accidentally
    # ship without it.
    if config.api_bearer_token:
        app.add_middleware(BaseHTTPMiddleware,
                            dispatch=make_bearer_middleware(
                                config.api_bearer_token))
        logger.info("API auth: bearer-token middleware ARMED")
    else:
        logger.warning("API auth: DISABLED (API_BEARER_TOKEN unset). "
                       "Do NOT expose this backend beyond a trusted LAN.")

    # Inject shared state into route modules
    tracks.track_manager = track_manager
    nodes.fleet_manager = fleet_manager
    assessments.decision_engine = decision_engine
    assessments.track_manager = track_manager
    health.backend_ref = backend
    if backend is not None:
        if hasattr(backend, "missions"):
            missions.registry = backend.missions
        if hasattr(backend, "store"):
            missions.store = backend.store
        if hasattr(backend, "approvals"):
            approvals.queue = backend.approvals
        if hasattr(backend, "overrides"):
            overrides.registry = backend.overrides

    # Tier 4 — Android/Windows client integration routes.
    android_pull.track_manager = track_manager
    android_pull.fleet_manager = fleet_manager
    android_pull.backend_ref = backend
    cot_export.track_manager = track_manager
    cot_export.backend_ref = backend

    app.include_router(tracks.router, prefix="/api/v1/tracks", tags=["tracks"])
    app.include_router(nodes.router, prefix="/api/v1/nodes", tags=["nodes"])
    app.include_router(events.router, prefix="/api/v1/events", tags=["events"])
    app.include_router(assessments.router,
                        prefix="/api/v1/assessments", tags=["assessments"])
    app.include_router(missions.router,
                        prefix="/api/v1/missions", tags=["missions"])
    app.include_router(approvals.router,
                        prefix="/api/v1/approvals", tags=["approvals"])
    app.include_router(overrides.router,
                        prefix="/api/v1/overrides", tags=["overrides"])

    # Tier 4 — Android/Windows client integration routes. Each module
    # gracefully degrades to `router = None` if FastAPI isn't installed,
    # so guard the include.
    if getattr(preflight, "router", None) is not None:
        app.include_router(preflight.router, tags=["preflight"])
    if getattr(android_pull, "router", None) is not None:
        app.include_router(android_pull.router,
                            prefix="/api/v1/android-pull",
                            tags=["android"])
    if getattr(cot_export, "router", None) is not None:
        app.include_router(cot_export.router,
                            prefix="/api/v1/cot/export",
                            tags=["cot"])
    # Health/metrics live at root (not under /api/v1) so a Prometheus
    # scraper or a load balancer can hit them with a single, stable
    # path that doesn't change with the API version.
    # RNS daemon control (task #27). Injected daemon may be None when
    # RNS_ENABLED=0; the route module guards every call with HTTP 503.
    # The router itself degrades to None when fastapi/pydantic aren't
    # installed (matches the preflight/android_pull pattern).
    # Spec section F + threat model: the RNS daemon control plane is
    # local-only — Linux operators talk to it via the uid-checked Unix
    # socket in `backend/rns/daemon.py::ControlServer`, Android via the
    # same Unix socket through `LocalSocket` (see RnsBridge.kt). We
    # deliberately do NOT mount the FastAPI router here so the daemon
    # never gets a network-exposed control surface, even when the API
    # binds to 0.0.0.0. The `rns` module remains importable for tests
    # and a hypothetical future opt-in mode.
    rns.daemon = rns_daemon
    app.include_router(health.router, tags=["health"])

    @app.get("/health")
    async def health_compat():
        # Back-compat alias for the original /health endpoint.
        return {"status": "ok", "service": "predator-sdr-backend"}

    @app.get("/api/v1/status")
    async def status():
        track_count = len(track_manager.tracks) if track_manager else 0
        node_count = fleet_manager.node_count() if fleet_manager else 0
        return {
            "active_tracks": track_count,
            "connected_nodes": node_count,
            "mission":
                backend.missions.active_id
                if backend is not None and hasattr(backend, "missions")
                else None,
        }

    return app
