import logging
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from backend.models.emitter_track import EmitterTrack
from backend.models.sensor_node import SensorNodeTrust
from backend.intelligence.anomaly_detector import AnomalyDetector, AnomalyFlag

logger = logging.getLogger(__name__)


@dataclass
class AssessmentReport:
    """Intelligence assessment for a single emitter track."""

    emitter_id: str
    assessment_ns: int = field(default_factory=time.time_ns)

    # Classification
    threat_level: str = "unknown"       # unknown / low / medium / high / critical
    confidence: float = 0.0

    # Findings
    anomaly_flags: List[AnomalyFlag] = field(default_factory=list)
    summary: str = ""

    # Recommendations
    recommended_action: str = "continue_monitoring"
    recommended_nodes: List[str] = field(default_factory=list)
    escalate_to_atak: bool = False

    def to_dict(self) -> dict:
        return {
            "emitter_id": self.emitter_id,
            "assessment_ns": self.assessment_ns,
            "threat_level": self.threat_level,
            "confidence": self.confidence,
            "anomaly_count": len(self.anomaly_flags),
            "anomaly_flags": [f.description for f in self.anomaly_flags],
            "summary": self.summary,
            "recommended_action": self.recommended_action,
            "recommended_nodes": list(self.recommended_nodes),
            "escalate_to_atak": self.escalate_to_atak,
        }


class DecisionEngine:
    """
    Analyst intelligence layer — converts tracks + anomalies into
    threat assessments and operational recommendations.
    """

    # Frequency band labels (regulatory context)
    _BAND_LABELS: Dict[str, str] = {
        'public_safety_vhf': "Public Safety VHF",
        'public_safety_uhf': "Public Safety UHF",
        'ism_433': "ISM 433 MHz",
        'ism_915': "ISM 915 MHz",
        'ism_2400': "ISM 2.4 GHz",
        'aviation': "Aviation",
        'marine_vhf': "Marine VHF",
    }

    def __init__(self, anomaly_detector: Optional[AnomalyDetector] = None):
        self._anomaly_detector = anomaly_detector or AnomalyDetector()

    def assess(self, track: EmitterTrack,
               anomaly_flags: Optional[List[AnomalyFlag]] = None,
               available_nodes: Optional[List[SensorNodeTrust]] = None) -> AssessmentReport:
        """
        Produce a full assessment for a track.

        Args:
            track: The emitter track to assess.
            anomaly_flags: Pre-computed anomaly flags (or None to skip).
            available_nodes: Nodes available for tasking.
        """
        report = AssessmentReport(
            emitter_id=track.emitter_id,
            confidence=track.confidence,
            anomaly_flags=anomaly_flags or [],
        )

        # Determine threat level
        report.threat_level = self._compute_threat_level(track, report.anomaly_flags)
        track.threat_level = report.threat_level

        # Build summary
        report.summary = self._build_summary(track, report)

        # Recommendations
        report.recommended_action = self._recommend_action(report.threat_level)
        report.escalate_to_atak = report.threat_level in ('high', 'critical')

        if available_nodes:
            report.recommended_nodes = self._select_nodes_for_tasking(
                track, report.threat_level, available_nodes)

        return report

    # ── Threat level computation ──────────────────────────────────────────────

    def _compute_threat_level(self, track: EmitterTrack,
                               flags: List[AnomalyFlag]) -> str:
        if not flags and track.confidence < 0.3:
            return "unknown"

        # Count flag severities
        severities = [f.severity for f in flags]
        critical_count = severities.count('critical')
        high_count = severities.count('high')
        medium_count = severities.count('medium')

        if critical_count > 0:
            return "critical"
        if high_count >= 2:
            return "high"
        if high_count == 1 and track.confidence >= 0.5:
            return "high"
        if high_count == 1 or medium_count >= 2:
            return "medium"
        if medium_count >= 1 or len(flags) > 0:
            return "low"
        return "unknown"

    def _build_summary(self, track: EmitterTrack,
                        report: AssessmentReport) -> str:
        freq_mhz = track.primary_frequency / 1e6
        band = self._identify_band(track.primary_frequency)
        age_s = (time.time_ns() - track.first_seen_ns) / 1e9

        parts = [
            f"Emitter at {freq_mhz:.4f} MHz ({band}).",
            f"First seen {age_s:.0f}s ago, {track.observation_count} observations.",
            f"Confidence: {track.confidence:.0%}.",
        ]

        if report.anomaly_flags:
            parts.append(
                f"{len(report.anomaly_flags)} anomaly/s: "
                + "; ".join(f.description for f in report.anomaly_flags[:3])
                + ("..." if len(report.anomaly_flags) > 3 else ".")
            )

        if track.modulation:
            parts.append(f"Modulation: {track.modulation}.")
        if track.protocol:
            parts.append(f"Protocol: {track.protocol}.")

        return " ".join(parts)

    def _recommend_action(self, threat_level: str) -> str:
        return {
            'unknown':  'continue_monitoring',
            'low':      'continue_monitoring',
            'medium':   'increase_dwell_time',
            'high':     'focus_all_nodes',
            'critical': 'alert_operator_immediately',
        }.get(threat_level, 'continue_monitoring')

    def _select_nodes_for_tasking(self, track: EmitterTrack,
                                   threat_level: str,
                                   nodes: List[SensorNodeTrust]) -> List[str]:
        if threat_level in ('high', 'critical'):
            # Task all TDOA-capable nodes for geolocation
            tdoa = [n.node_id for n in nodes if n.can_do_tdoa]
            if tdoa:
                return tdoa
        # Else: nodes already monitoring this frequency
        return [n.node_id for n in nodes
                if any(abs(f - track.primary_frequency) < 1e6
                       for f in n.center_frequencies_monitored)]

    def _identify_band(self, freq_hz: float) -> str:
        if 108e6 <= freq_hz <= 137e6:
            return "Aviation"
        if 136e6 <= freq_hz <= 174e6:
            return "VHF Public Safety / Land Mobile"
        if 156e6 <= freq_hz <= 162e6:
            return "Marine VHF"
        if 380e6 <= freq_hz <= 512e6:
            return "UHF Public Safety"
        if 430e6 <= freq_hz <= 440e6:
            return "ISM 433 MHz / Amateur"
        if 862e6 <= freq_hz <= 960e6:
            return "ISM 915 MHz / Cellular"
        if 1559e6 <= freq_hz <= 1610e6:
            return "GNSS (GPS/GLONASS)"
        if 2400e6 <= freq_hz <= 2500e6:
            return "ISM 2.4 GHz (WiFi/BT)"
        return f"{freq_hz/1e6:.0f} MHz"
