"""
SDRBackend — describes one physical SDR attached to a node.

A `SensorNodeTrust` historically modelled exactly one SDR per node.
Nodes can now optionally declare a list of `SDRBackend` instances —
useful when a single Raspberry Pi has, say, a HackRF AND an RTL-SDR
plugged in, or when a workstation node bundles three different radios
behind one Kujhad endpoint.

Backwards compatibility: existing call sites that read
`SensorNodeTrust.hardware_code` and `max_sample_rate_hz` keep working
unchanged. New code that wants to take advantage of multiple SDRs uses
`node.sdr_backends` (empty list = "single-SDR mode, look at the legacy
fields"). Helper methods on `SensorNodeTrust` (`all_sdr_backends()`,
`total_instantaneous_bandwidth_hz()`) hide the seam.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional


@dataclass
class SDRBackend:
    """One physical SDR radio attached to a node."""

    # Stable identity
    backend_id: str = ""              # operator-given handle, e.g. "hackrf-A"
    hardware_code: str = ""           # 'rtlsdr', 'hackrf', 'limesdr', etc.
    hardware_serial: str = ""

    # Capability — what this radio can physically do
    max_sample_rate_hz: int = 2_400_000
    instantaneous_bandwidth_hz: int = 2_400_000  # how wide it can listen at once
    min_freq_hz: float = 24e6
    max_freq_hz: float = 1.7e9

    # Current state — what this radio is doing right now
    current_center_freq_hz: Optional[float] = None
    current_sample_rate_hz: Optional[int] = None
    in_use: bool = False              # claimed by an active scan/decode

    # Quality factors (0..1) inherited from hardware capability lookup
    timing_stability_trust: float = 1.0
    sensitivity_trust: float = 1.0
    frequency_stability_trust: float = 1.0

    def covers(self, freq_hz: float) -> bool:
        """Is this radio physically capable of tuning this frequency?"""
        return self.min_freq_hz <= freq_hz <= self.max_freq_hz

    def to_dict(self) -> dict:
        return {
            "backend_id": self.backend_id,
            "hardware_code": self.hardware_code,
            "hardware_serial": self.hardware_serial,
            "max_sample_rate_hz": self.max_sample_rate_hz,
            "instantaneous_bandwidth_hz": self.instantaneous_bandwidth_hz,
            "min_freq_hz": self.min_freq_hz,
            "max_freq_hz": self.max_freq_hz,
            "current_center_freq_hz": self.current_center_freq_hz,
            "in_use": self.in_use,
        }
