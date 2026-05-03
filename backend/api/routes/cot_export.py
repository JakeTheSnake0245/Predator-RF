"""GET /api/v1/cot/export — pull-style CoT XML export.

The Android (ATAK plugin) client uses this when it can't receive
multicast on the local Wi-Fi (carrier NAT, hotspot, school networks).
Instead of waiting for a UDP datagram on 239.2.3.1:6969, the phone
polls this endpoint and feeds the returned XML into ATAK's local
"file import" CoT pipeline.

Two modes:

    GET /api/v1/cot/export                 → all tracks that currently
                                             pass the escalation gate
                                             (assessment.escalate_to_atak)
                                             concatenated into one
                                             multi-event document
    GET /api/v1/cot/export?emitter_id=...  → just that one track,
                                             gated only by the operator
                                             requesting it (manual
                                             override from the phone)

XML schema is *exactly* what backend/output/cot_emitter.py emits over
UDP, so an Android-side parser only has to learn one format. See
docs/ATAK_COT_FORMAT.md for the contract.
"""
from __future__ import annotations

import logging
from typing import Any, Optional

logger = logging.getLogger(__name__)

track_manager = None
backend_ref = None


def _multi_event_envelope(events_xml: list[bytes]) -> bytes:
    """Wrap N <event> documents in a single <events> root so ATAK's
    file-import accepts a one-shot pull."""
    parts = [b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
             b'<events>']
    for e in events_xml:
        # Strip the per-event xml prolog so the outer envelope is valid.
        if e.startswith(b'<?xml'):
            idx = e.find(b'?>')
            if idx != -1:
                e = e[idx + 2:]
        parts.append(e.lstrip())
    parts.append(b'</events>')
    return b"\n".join(parts)


def _build_track_event(track, *, uid_prefix: str, stale_s: float
                        ) -> Optional[bytes]:
    from backend.output.cot_emitter import build_cot_xml
    lat = getattr(track, "estimated_lat", None)
    lon = getattr(track, "estimated_lon", None)
    if lat is None or lon is None:
        return None
    emitter_id = getattr(track, "emitter_id", "unknown")
    freq_mhz = float(getattr(track, "primary_frequency", 0.0) or 0.0) / 1e6
    threat = (getattr(track, "threat_level", "unknown") or "unknown").upper()
    callsign = f"{uid_prefix}-{emitter_id[:8]}"
    remarks = (
        f"PREDATOR-RF {threat} | "
        f"{freq_mhz:.4f} MHz | "
        f"obs={getattr(track, 'observation_count', 0)} | "
        f"conf={getattr(track, 'confidence', 0):.2f}"
    )
    return build_cot_xml(
        uid=f"{uid_prefix}.{emitter_id}",
        lat=lat, lon=lon,
        cot_type="a-u-G",
        callsign=callsign,
        ce_meters=50.0 + (1.0 - max(0.0, min(1.0,
            float(getattr(track, "location_confidence", 0.0))))) * 4_950.0,
        stale_seconds=stale_s,
        remarks=remarks,
    )


try:
    from fastapi import APIRouter, HTTPException, Query
    from fastapi.responses import Response
    router = APIRouter()

    @router.get("")
    async def export_cot(
        emitter_id: Optional[str] = Query(None,
            description="If set, export only this track (operator override)."),
        stale_seconds: float = Query(300.0, ge=1.0, le=86_400.0),
    ) -> Any:
        from backend.config import config
        if track_manager is None:
            raise HTTPException(status_code=503,
                                 detail="track manager not wired")

        uid_prefix = config.cot_uid_prefix or "PREDATOR"

        if emitter_id is not None:
            t = track_manager.tracks.get(emitter_id)
            if t is None:
                raise HTTPException(status_code=404, detail="track not found")
            xml = _build_track_event(t, uid_prefix=uid_prefix,
                                      stale_s=stale_seconds)
            if xml is None:
                raise HTTPException(status_code=409,
                    detail="track has no TDOA fix — cannot emit CoT")
            return Response(content=xml, media_type="application/xml")

        # Bulk export — every track whose latest assessment escalates.
        # Matches the UDP emitter's gate so the operator gets the same
        # set whether they receive over multicast OR pull from the phone.
        approved_tracks = []
        latest_asmts = {}
        if backend_ref is not None and hasattr(backend_ref, "store"):
            try:
                latest_asmts = await backend_ref.store.latest_assessments() \
                    if hasattr(backend_ref.store,
                                "latest_assessments") else {}
            except Exception as exc:
                logger.warning("cot/export assessment lookup failed: %s", exc)

        for t in track_manager.tracks.values():
            asmt = latest_asmts.get(getattr(t, "emitter_id", None))
            if asmt and asmt.get("escalate_to_atak"):
                approved_tracks.append(t)

        events = []
        for t in approved_tracks:
            xml = _build_track_event(t, uid_prefix=uid_prefix,
                                      stale_s=stale_seconds)
            if xml is not None:
                events.append(xml)

        body = _multi_event_envelope(events)
        return Response(content=body, media_type="application/xml")

except ImportError:
    router = None  # type: ignore
