"""Server-Sent Events (SSE) stream for real-time track updates."""

import asyncio
import json
import time
from fastapi import APIRouter
from fastapi.responses import StreamingResponse

router = APIRouter()

# In-memory event ring buffer shared with the ingest pipeline
_event_ring: list = []
_MAX_EVENTS = 1000


def push_event(event_dict: dict):
    """Called by the ingest pipeline to publish events to SSE subscribers."""
    _event_ring.append(event_dict)
    if len(_event_ring) > _MAX_EVENTS:
        _event_ring.pop(0)


@router.get("/stream")
async def event_stream():
    """Server-Sent Events stream of all RF events (live)."""

    async def generator():
        last_index = len(_event_ring)
        while True:
            await asyncio.sleep(0.1)
            current = len(_event_ring)
            if current > last_index:
                for ev in _event_ring[last_index:current]:
                    yield f"data: {json.dumps(ev)}\n\n"
                last_index = current

    return StreamingResponse(generator(), media_type="text/event-stream")


@router.get("/recent")
async def recent_events(count: int = 50):
    """Get last N events."""
    return _event_ring[-count:]
