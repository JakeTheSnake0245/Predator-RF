"""Mission lifecycle endpoints — start, end, list, export."""
from __future__ import annotations

import io
import json
import tarfile
import time
from typing import Any, Dict

from fastapi import APIRouter, HTTPException, Body
from fastapi.responses import StreamingResponse

router = APIRouter()

# Injected by api/server.py
registry = None  # MissionRegistry
store = None     # MissionStore


@router.post("")
async def start_mission(body: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    if registry is None:
        raise HTTPException(503, "mission registry not configured")
    name = (body.get("name") or "").strip()
    if not name:
        raise HTTPException(400, "name is required")
    m = await registry.start(name=name,
                             operator=body.get("operator", "operator"),
                             notes=body.get("notes", ""))
    return m.to_dict()


@router.post("/end")
async def end_mission(body: Dict[str, Any] = Body(default={})) -> Dict[str, Any]:
    if registry is None:
        raise HTTPException(503, "mission registry not configured")
    mid = body.get("mission_id")
    m = await registry.end(mission_id=mid)
    if m is None:
        raise HTTPException(404, "no active mission to end")
    return m.to_dict()


@router.get("")
async def list_missions():
    if registry is None:
        return []
    return registry.list_missions()


@router.get("/active")
async def active_mission():
    if registry is None or registry.active is None:
        return {"active": None}
    return {"active": registry.active.to_dict()}


@router.get("/{mission_id}/export")
async def export_mission(mission_id: str):
    """Stream a tar.gz of the mission's events/tracks/assessments as
    JSONL. Operator downloads, archives, and replays in their AAR
    tooling."""
    if store is None:
        raise HTTPException(503, "store not configured")
    bundle = store.export_mission(mission_id)
    if not bundle:
        raise HTTPException(404, "mission not found")

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        def _add(name: str, payload: bytes):
            info = tarfile.TarInfo(name=name)
            info.size = len(payload)
            info.mtime = int(time.time())
            tar.addfile(info, io.BytesIO(payload))
        _add("mission.json",
             json.dumps(bundle["mission"], default=str,
                        indent=2).encode())
        for tbl in ("events", "tracks", "assessments", "approvals"):
            payload = ("\n".join(json.dumps(r, default=str)
                                  for r in bundle[tbl])).encode()
            _add(f"{tbl}.jsonl", payload)
    buf.seek(0)
    fname = f"mission_{mission_id[:8]}.tar.gz"
    return StreamingResponse(
        buf, media_type="application/gzip",
        headers={"Content-Disposition": f"attachment; filename={fname}"})
