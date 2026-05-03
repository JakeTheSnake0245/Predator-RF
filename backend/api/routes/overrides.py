"""Operator override endpoints — friendly list, blacklist, manual locations."""
from __future__ import annotations

from typing import Any, Dict

from fastapi import APIRouter, HTTPException, Body

router = APIRouter()

# Injected by api/server.py
registry = None  # OverrideRegistry


# ── Friendly list ────────────────────────────────────────────────────
@router.get("/friendly")
async def list_friendly():
    if registry is None:
        return []
    return registry.list_friendly()


@router.post("/friendly")
async def add_friendly(body: Dict[str, Any] = Body(...)):
    if registry is None:
        raise HTTPException(503, "override registry not configured")
    em = body.get("emitter_id")
    if not em:
        raise HTTPException(400, "emitter_id required")
    f = await registry.add_friendly(em, label=body.get("label", ""))
    return {"emitter_id": f.emitter_id, "label": f.label,
            "added_ns": f.added_ns}


@router.delete("/friendly/{emitter_id}")
async def remove_friendly(emitter_id: str):
    if registry is None:
        raise HTTPException(503, "override registry not configured")
    return {"removed": await registry.remove_friendly(emitter_id)}


# ── Frequency blacklist ──────────────────────────────────────────────
@router.get("/blacklist")
async def list_blacklist():
    if registry is None:
        return []
    return registry.list_blacklist()


@router.post("/blacklist")
async def add_blacklist(body: Dict[str, Any] = Body(...)):
    if registry is None:
        raise HTTPException(503, "override registry not configured")
    try:
        start_hz = float(body["start_hz"])
        end_hz = float(body["end_hz"])
    except (KeyError, TypeError, ValueError):
        raise HTTPException(400, "start_hz and end_hz required (numeric)")
    b = await registry.add_blacklist(
        start_hz, end_hz, reason=body.get("reason", ""))
    return {"start_hz": b.start_hz, "end_hz": b.end_hz,
            "reason": b.reason, "added_ns": b.added_ns}


@router.delete("/blacklist")
async def clear_blacklist():
    if registry is None:
        raise HTTPException(503, "override registry not configured")
    await registry.clear_blacklist()
    return {"cleared": True}


# ── Manual location override ─────────────────────────────────────────
@router.get("/manual_location")
async def list_manual_locations():
    if registry is None:
        return []
    return registry.list_manual_locations()


@router.post("/manual_location")
async def set_manual_location(body: Dict[str, Any] = Body(...)):
    if registry is None:
        raise HTTPException(503, "override registry not configured")
    try:
        em = body["emitter_id"]
        lat = float(body["lat"])
        lon = float(body["lon"])
    except (KeyError, TypeError, ValueError):
        raise HTTPException(400,
            "emitter_id, lat, lon required (lat/lon numeric)")
    ml = await registry.set_manual_location(
        em, lat, lon,
        confidence=float(body.get("confidence", 0.95)),
        source=body.get("source", "operator"))
    return {"emitter_id": ml.emitter_id, "lat": ml.lat, "lon": ml.lon,
            "confidence": ml.confidence, "source": ml.source,
            "added_ns": ml.added_ns}


@router.delete("/manual_location/{emitter_id}")
async def clear_manual_location(emitter_id: str):
    if registry is None:
        raise HTTPException(503, "override registry not configured")
    return {"removed": await registry.clear_manual_location(emitter_id)}
