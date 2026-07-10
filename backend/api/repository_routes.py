"""
Repository API routes — signal repository search, fingerprint, IQ capture.

Mounted at /api/v1/repository by main.py.

Endpoints:
    GET  /api/v1/repository/signals            search signals
    GET  /api/v1/repository/signals/<signal_id> single record
    POST /api/v1/repository/signals             save a signal manually
    GET  /api/v1/repository/similar            find similar by fingerprint vec
    POST /api/v1/repository/iq_capture         trigger IQ capture on a node
    GET  /api/v1/repository/intercepts         list correlated intercepts
    GET  /api/v1/repository/rules              list correlation rules
    POST /api/v1/repository/rules              add a correlation rule
    DELETE /api/v1/repository/rules/<rule_id>  remove a rule
    GET  /api/v1/fleet/state                   full fleet snapshot
    GET  /api/v1/fleet/events                  fleet event stream (since=N)
"""
from __future__ import annotations

import logging
import uuid
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel, Field

logger = logging.getLogger(__name__)

router = APIRouter()


def _get_repo(request: Request):
    repo = getattr(request.app.state, "signal_repo", None)
    if repo is None:
        raise HTTPException(503, "Signal repository not initialised")
    return repo


def _get_corr(request: Request):
    engine = getattr(request.app.state, "correlation_engine", None)
    if engine is None:
        raise HTTPException(503, "Correlation engine not initialised")
    return engine


def _get_fleet(request: Request):
    mgr = getattr(request.app.state, "fleet_state_manager", None)
    if mgr is None:
        raise HTTPException(503, "Fleet state manager not initialised")
    return mgr


def _get_iq(request: Request):
    svc = getattr(request.app.state, "iq_capture_service", None)
    if svc is None:
        raise HTTPException(503, "IQ capture service not initialised")
    return svc


class SignalSaveBody(BaseModel):
    node_id:      str
    center_hz:    float
    bandwidth_hz: Optional[float] = None
    power_dbfs:   Optional[float] = None
    snr_db:       Optional[float] = None
    modulation:   Optional[str]   = None
    protocol:     Optional[str]   = None
    decoded_text: Optional[str]   = None
    threat_level: Optional[str]   = None
    node_lat:     Optional[float] = None
    node_lon:     Optional[float] = None
    emitter_id:   Optional[str]   = None
    fingerprint_vec: Optional[list] = None


class SimilarBody(BaseModel):
    fingerprint_vec: list
    threshold: Optional[float] = None
    limit: int = 10


class IQCaptureBody(BaseModel):
    node_id:          str
    freq_hz:          float
    signal_id:        Optional[str]   = None
    duration_s:       Optional[float] = None
    sample_rate_hz:   Optional[float] = None


class CorrelationRuleBody(BaseModel):
    name:        str
    nodes:       list = Field(default_factory=list)
    freq_lo_hz:  float
    freq_hi_hz:  float
    window_s:    float = 60.0
    min_nodes:   int   = 2
    action:      str   = "all"
    notes:       str   = ""


@router.get("/signals")
async def search_signals(
    request: Request,
    freq_hz:       Optional[float] = None,
    freq_tol_hz:   float = 25000.0,
    after_ns:      Optional[int]   = None,
    before_ns:     Optional[int]   = None,
    modulation:    Optional[str]   = None,
    threat_level:  Optional[str]   = None,
    lat:           Optional[float] = None,
    lon:           Optional[float] = None,
    radius_m:      Optional[float] = None,
    limit:         int = 100,
    repo=Depends(_get_repo)
):
    results = repo.search(
        freq_hz=freq_hz, freq_tol_hz=freq_tol_hz,
        after_ns=after_ns, before_ns=before_ns,
        modulation=modulation, threat_level=threat_level,
        lat=lat, lon=lon, radius_m=radius_m,
        limit=min(limit, 1000)
    )
    return {"signals": results, "count": len(results)}


@router.post("/signals")
async def save_signal(body: SignalSaveBody, repo=Depends(_get_repo)):
    sid = await repo.async_save_signal(
        node_id=body.node_id, center_hz=body.center_hz,
        bandwidth_hz=body.bandwidth_hz, power_dbfs=body.power_dbfs,
        snr_db=body.snr_db, modulation=body.modulation,
        protocol=body.protocol, decoded_text=body.decoded_text,
        threat_level=body.threat_level,
        node_lat=body.node_lat, node_lon=body.node_lon,
        emitter_id=body.emitter_id,
        fingerprint_vec=body.fingerprint_vec,
    )
    return {"signal_id": sid}


@router.post("/similar")
async def find_similar(body: SimilarBody, repo=Depends(_get_repo)):
    results = repo.find_similar(
        fingerprint_vec=body.fingerprint_vec,
        threshold=body.threshold,
        limit=body.limit
    )
    return {"matches": results, "count": len(results)}


@router.post("/iq_capture")
async def trigger_iq_capture(
    body: IQCaptureBody,
    request: Request,
    svc=Depends(_get_iq)
):
    capture_id = await svc.capture(
        node_id=body.node_id,
        freq_hz=body.freq_hz,
        signal_id=body.signal_id,
        duration_s=body.duration_s,
        sample_rate_hz=body.sample_rate_hz,
        captured_by="operator",
    )
    if capture_id is None:
        raise HTTPException(502, "IQ capture command failed — check node is connected")
    return {"capture_id": capture_id, "status": "queued"}


@router.get("/intercepts")
async def list_intercepts(
    request: Request,
    limit: int = 50,
    repo=Depends(_get_repo)
):
    with repo._lock:
        rows = repo._conn.execute("""
            SELECT i.*, s.center_hz as s_freq, s.modulation
            FROM correlated_intercepts i
            LEFT JOIN signal_repository s USING (signal_id)
            ORDER BY i.first_detected_ns DESC
            LIMIT ?
        """, (min(limit, 500),)).fetchall()
    return {"intercepts": [dict(r) for r in rows], "count": len(rows)}


@router.get("/rules")
async def list_rules(engine=Depends(_get_corr)):
    return {"rules": engine.list_rules()}


@router.post("/rules")
async def add_rule(body: CorrelationRuleBody, engine=Depends(_get_corr)):
    from backend.coordination.correlation_engine import CorrelationRule
    rule = CorrelationRule(
        rule_id   = str(uuid.uuid4()),
        name      = body.name,
        nodes     = body.nodes,
        freq_lo_hz= body.freq_lo_hz,
        freq_hi_hz= body.freq_hi_hz,
        window_s  = body.window_s,
        min_nodes = body.min_nodes,
        action    = body.action,
        notes     = body.notes,
    )
    rule_id = engine.add_rule(rule)
    return {"rule_id": rule_id, "status": "added"}


@router.delete("/rules/{rule_id}")
async def remove_rule(rule_id: str, engine=Depends(_get_corr)):
    removed = engine.remove_rule(rule_id)
    if not removed:
        raise HTTPException(404, f"Rule {rule_id} not found")
    return {"status": "removed"}


@router.get("/fleet/state")
async def fleet_state(fleet=Depends(_get_fleet)):
    return fleet.serialize()


@router.get("/fleet/events")
async def fleet_events(since: int = 0, fleet=Depends(_get_fleet)):
    events = fleet.get_events_since(since)
    return {
        "events": events,
        "count":  len(events),
        "fleet_serial": fleet._global_serial,
    }
