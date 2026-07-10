"""
LOB (Line of Bearing) measurement from a KrakenSDR DOA node.

One measurement represents a single bearing observation from one antenna
array at one point in time.  Multiple measurements from different nodes
(or the same node at different times for a moving emitter) are consumed
by LOBTriangulator to produce a crosscut fix.
"""
from __future__ import annotations

import uuid
import time
from dataclasses import dataclass, field


@dataclass
class LOBMeasurement:
    """Single direction-finding observation from one KrakenSDR node."""

    # ── Identity ─────────────────────────────────────────────────────────
    measurement_id: str = field(default_factory=lambda: str(uuid.uuid4()))

    # ── Sensor geometry ───────────────────────────────────────────────────
    node_id: str = ""           # KrakenSDR device identifier
    node_lat: float = 0.0       # WGS-84 latitude (decimal degrees)
    node_lon: float = 0.0       # WGS-84 longitude (decimal degrees)

    # ── Bearing ───────────────────────────────────────────────────────────
    bearing_deg: float = 0.0         # True bearing from node, 0-360 (0 = north)
    bearing_uncert_deg: float = 10.0  # 1-sigma uncertainty (degrees)

    # ── Quality ───────────────────────────────────────────────────────────
    confidence: float = 0.5     # 0..1; fed into triangulator as weight²
    power_dbfs: float = 0.0     # Received power at array (dBFS)
    snr_db: float = 0.0         # Signal-to-noise ratio (dB)

    # ── RF parameters ────────────────────────────────────────────────────
    frequency_hz: float = 0.0   # Centre frequency of detected signal

    # ── Timing ───────────────────────────────────────────────────────────
    timestamp_ns: int = field(default_factory=time.time_ns)
    # Heading of the node platform (0 = north) — used when the array is
    # on a moving vehicle and bearing_deg is platform-relative.  When
    # the array is stationary set heading_deg = 0.
    heading_deg: float = 0.0    # Platform heading — 0 = static array

    def to_dict(self) -> dict:
        return {
            "measurement_id": self.measurement_id,
            "node_id": self.node_id,
            "node_lat": self.node_lat,
            "node_lon": self.node_lon,
            "bearing_deg": self.bearing_deg,
            "bearing_uncert_deg": self.bearing_uncert_deg,
            "confidence": self.confidence,
            "power_dbfs": self.power_dbfs,
            "snr_db": self.snr_db,
            "frequency_hz": self.frequency_hz,
            "timestamp_ns": self.timestamp_ns,
            "heading_deg": self.heading_deg,
        }
