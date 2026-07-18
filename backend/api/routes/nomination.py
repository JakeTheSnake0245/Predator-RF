"""Operator target nomination endpoints.

POST   /api/v1/target/nominate   — nominate by emitter_id or frequency_hz
DELETE /api/v1/target/nomination — clear the active nomination
GET    /api/v1/target/nomination — read the active nomination (or null)
"""
from __future__ import annotations

from typing import Any, Dict

from fastapi import APIRouter, HTTPException, Body

router = APIRouter()

manager = None   # NominationManager — injected by api/server.py


@router.get("/nomination")
async def get_nomination() -> Dict[str, Any]:
    if manager is None:
        return {"nominated": None, "supported": False}
    return {"nominated": manager.current(), "supported": True}


@router.post("/nominate")
async def nominate(body: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    if manager is None:
        raise HTTPException(503, "nomination manager not configured")
    emitter_id = body.get("emitter_id")
    frequency_hz = body.get("frequency_hz")
    if frequency_hz is not None:
        try:
            frequency_hz = float(frequency_hz)
        except (TypeError, ValueError):
            raise HTTPException(400, "frequency_hz must be a number")
    try:
        nom = manager.nominate(
            emitter_id=emitter_id,
            frequency_hz=frequency_hz,
            label=body.get("label"),
            operator=body.get("operator", "operator"),
        )
    except ValueError as exc:
        raise HTTPException(400, str(exc))
    return {"nominated": nom}


@router.delete("/nomination")
async def clear_nomination() -> Dict[str, Any]:
    if manager is None:
        raise HTTPException(503, "nomination manager not configured")
    cleared = manager.clear()
    return {"cleared": cleared, "nominated": None}
