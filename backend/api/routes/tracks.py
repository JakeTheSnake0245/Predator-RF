from typing import List, Optional
from fastapi import APIRouter, HTTPException, Query

router = APIRouter()
track_manager = None   # Injected by server.py


@router.get("/")
async def list_tracks(
        min_confidence: float = Query(0.0, ge=0, le=1),
        state: Optional[str] = None,
        limit: int = Query(100, ge=1, le=1000),
):
    if not track_manager:
        return []
    tracks = list(track_manager.tracks.values())

    if min_confidence > 0:
        tracks = [t for t in tracks if t.confidence >= min_confidence]
    if state:
        tracks = [t for t in tracks if t.state.value == state]

    tracks.sort(key=lambda t: t.confidence, reverse=True)
    return [t.to_dict() for t in tracks[:limit]]


@router.get("/{emitter_id}")
async def get_track(emitter_id: str):
    if not track_manager or emitter_id not in track_manager.tracks:
        raise HTTPException(status_code=404, detail="Track not found")
    return track_manager.tracks[emitter_id].to_dict()


@router.get("/{emitter_id}/history")
async def track_history(emitter_id: str):
    if not track_manager or emitter_id not in track_manager.tracks:
        raise HTTPException(status_code=404, detail="Track not found")
    t = track_manager.tracks[emitter_id]
    return {
        "emitter_id": emitter_id,
        "frequency_history": t.frequency_history[-200:],
        "power_history": t.power_history[-200:],
        "confidence_history": t.confidence_history[-200:],
    }


@router.delete("/{emitter_id}")
async def delete_track(emitter_id: str):
    if not track_manager or emitter_id not in track_manager.tracks:
        raise HTTPException(status_code=404, detail="Track not found")
    t = track_manager.tracks.pop(emitter_id)
    track_manager._associator.remove_track(t)
    return {"deleted": emitter_id}
