from dataclasses import dataclass, field
from typing import Optional
import time


@dataclass
class RFEvent:
    """Atomic RF detection event from a sensor node."""

    frequency: float                # Hz
    power_dbfs: float               # dBFS (relative to full-scale)
    snr_db: float                   # Signal-to-noise ratio
    timestamp_ns: int               # UNIX nanoseconds

    node_id: str                    # Source sensor node
    node_trust_score: float = 0.5   # 0..1

    bandwidth_hz: float = 12500.0   # Estimated signal bandwidth
    duration_ms: float = 0.0        # Signal duration (0 = unknown)

    hardware_id: str = ""           # Device serial
    detector: str = "fft_peak"      # Algorithm that found this

    modulation: Optional[str] = None    # Decoded modulation type
    protocol: Optional[str] = None      # Decoded protocol
    decoded_payload: Optional[str] = None

    # Location of detecting node (if GPS available)
    node_lat: Optional[float] = None
    node_lon: Optional[float] = None
    node_alt_m: Optional[float] = None

    event_id: str = field(default_factory=lambda: _gen_id())

    def to_dict(self) -> dict:
        return {
            "event_id": self.event_id,
            "frequency": self.frequency,
            "power_dbfs": self.power_dbfs,
            "snr_db": self.snr_db,
            "timestamp_ns": self.timestamp_ns,
            "node_id": self.node_id,
            "node_trust_score": self.node_trust_score,
            "bandwidth_hz": self.bandwidth_hz,
            "hardware_id": self.hardware_id,
            "detector": self.detector,
            "modulation": self.modulation,
            "protocol": self.protocol,
            "node_lat": self.node_lat,
            "node_lon": self.node_lon,
        }


def _gen_id() -> str:
    import uuid
    return str(uuid.uuid4())
