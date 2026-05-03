from typing import Optional
from fastapi import APIRouter, HTTPException

router = APIRouter()
decision_engine = None   # Injected by server.py
track_manager = None


@router.get("/{emitter_id}")
async def assess_track(emitter_id: str):
    if not track_manager or emitter_id not in track_manager.tracks:
        raise HTTPException(status_code=404, detail="Track not found")
    if not decision_engine:
        raise HTTPException(status_code=503, detail="Decision engine not available")

    track = track_manager.tracks[emitter_id]

    from backend.intelligence.anomaly_detector import AnomalyDetector
    from backend.models.rf_event import RFEvent
    import time

    # Re-run anomaly detection on latest state
    detector = AnomalyDetector()
    # Create a synthetic "latest event" from track state
    latest = RFEvent(
        frequency=track.primary_frequency,
        power_dbfs=track.last_power_dbfs or -80.0,
        snr_db=0.0,
        timestamp_ns=track.last_seen_ns,
        node_id=track.most_trustworthy_node or "unknown",
    )
    flags = detector.analyze(track, latest)

    nodes = list(track_manager.sensor_nodes.values()) if track_manager else []
    report = decision_engine.assess(track, flags, nodes)

    return report.to_dict()


@router.get("/")
async def list_assessments(threat_level: Optional[str] = None):
    """Return assessments for all high-confidence tracks."""
    if not track_manager or not decision_engine:
        return []

    results = []
    from backend.intelligence.anomaly_detector import AnomalyDetector
    from backend.models.rf_event import RFEvent

    detector = AnomalyDetector()
    for track in track_manager.high_confidence_tracks(min_confidence=0.4):
        latest = RFEvent(
            frequency=track.primary_frequency,
            power_dbfs=track.last_power_dbfs or -80.0,
            snr_db=0.0,
            timestamp_ns=track.last_seen_ns,
            node_id=track.most_trustworthy_node or "unknown",
        )
        flags = detector.analyze(track, latest)
        report = decision_engine.assess(track, flags)
        if not threat_level or report.threat_level == threat_level:
            results.append(report.to_dict())

    return results
