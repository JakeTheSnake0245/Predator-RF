"""
Predator-SDR Python backend REST API.
Exposes tracks, nodes, events, and assessments over HTTP/JSON.
"""

import logging
from typing import Optional

logger = logging.getLogger(__name__)


def create_app(track_manager=None, fleet_manager=None, decision_engine=None):
    """
    Create the FastAPI application.

    Args:
        track_manager: TrackManager instance (injected)
        fleet_manager: KujhadFleetManager instance (injected)
        decision_engine: DecisionEngine instance (injected)
    """
    try:
        from fastapi import FastAPI
        from fastapi.middleware.cors import CORSMiddleware
    except ImportError:
        raise RuntimeError("FastAPI not installed. Run: pip install fastapi uvicorn")

    from backend.api.routes import tracks, nodes, events, assessments

    app = FastAPI(
        title="Predator-SDR Backend API",
        description="RF intelligence fusion backend for Predator-SDR sensor network",
        version="1.0.0",
    )

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # Inject shared state into route modules
    tracks.track_manager = track_manager
    nodes.fleet_manager = fleet_manager
    assessments.decision_engine = decision_engine
    assessments.track_manager = track_manager

    app.include_router(tracks.router, prefix="/api/v1/tracks", tags=["tracks"])
    app.include_router(nodes.router, prefix="/api/v1/nodes", tags=["nodes"])
    app.include_router(events.router, prefix="/api/v1/events", tags=["events"])
    app.include_router(assessments.router, prefix="/api/v1/assessments", tags=["assessments"])

    @app.get("/health")
    async def health():
        return {"status": "ok", "service": "predator-sdr-backend"}

    @app.get("/api/v1/status")
    async def status():
        track_count = len(track_manager.tracks) if track_manager else 0
        node_count = fleet_manager.node_count() if fleet_manager else 0
        return {
            "active_tracks": track_count,
            "connected_nodes": node_count,
        }

    return app
