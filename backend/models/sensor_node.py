from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional, Tuple
import time


class NodeRole(Enum):
    WIDEBAND_SCANNER = "wideband"
    NARROWBAND_MONITOR = "narrowband"
    DEEP_ANALYZER = "analyzer"
    MULTI_ROLE = "multi"


@dataclass
class SensorNodeTrust:
    """Hardware-aware sensor node trust model."""

    node_id: str
    node_role: NodeRole = NodeRole.MULTI_ROLE

    # Hardware identity
    hardware_code: str = ""         # 'rtlsdr', 'hackrf', 'limesdr', etc.
    hardware_serial: str = ""
    hardware_age_days: int = 0

    # Kujhad fleet endpoint (from C++ app HTTP API)
    kujhad_host: str = ""           # e.g. "192.168.1.10"
    kujhad_port: int = 5259
    kujhad_api_key: str = ""
    kujhad_tls: bool = False
    # TLS cert verification: default-secure. Set True only on internal
    # self-signed-cert fleets where you've assessed the trust posture and
    # cannot install the fleet CA on the backend host. No effect when
    # kujhad_tls is False (plain HTTP).
    kujhad_tls_insecure_skip_verify: bool = False

    # Location
    location_gps: Optional[Tuple[float, float]] = None   # (lat, lon)
    location_accuracy_m: float = 10.0

    # Clock
    gps_synchronized: bool = False
    clock_drift_ppm: float = 0.0
    timing_offset_ns: int = 0

    # Operational config
    bandwidth_allocated_mhz: float = 100.0
    center_frequencies_monitored: List[float] = field(default_factory=list)

    # Trust components
    base_trust: float = 0.6
    uptime_fraction: float = 1.0
    false_positive_rate: float = 0.0
    multi_node_agreement: float = 1.0

    # Hardware-specific trust factors (set in __post_init__)
    frequency_stability_trust: float = 1.0
    sensitivity_trust: float = 1.0
    timing_stability_trust: float = 1.0

    # Calibration
    frequency_calibration_offset_hz: float = 0.0
    gain_calibration_factor: float = 1.0
    last_calibration_ns: int = 0

    # Capability flags (derived from hardware)
    can_do_wideband_scan: bool = True
    can_do_narrowband_focus: bool = True
    can_do_iq_recording: bool = True
    can_do_tdoa: bool = False
    max_concurrent_decoders: int = 1
    max_sample_rate_hz: int = 2_400_000
    max_fft_size: int = 8192
    thermal_throttling_active: bool = False

    # Observations
    total_observations: int = 0
    observations_corroborated: int = 0
    observations_flagged_anomalous: int = 0

    # Hardware capabilities object (populated in __post_init__)
    hardware_capabilities: object = field(default=None, repr=False)

    def __post_init__(self):
        if self.hardware_code:
            try:
                from backend.sensor.hardware.capabilities import get_hardware_capabilities
                caps = get_hardware_capabilities(self.hardware_code)
                self.hardware_capabilities = caps
                if caps:
                    self.max_sample_rate_hz = caps.max_sample_rate_hz
                    self.can_do_tdoa = caps.supports_tdoa
                    self._init_hardware_trust_factors(caps)
            except ImportError:
                pass

    def _init_hardware_trust_factors(self, caps):
        max_ppm, min_ppm = 100.0, 1.0
        self.frequency_stability_trust = max(0.5, min(1.0,
            1.0 - (caps.freq_accuracy_ppm - min_ppm) / (max_ppm - min_ppm)))

        self.sensitivity_trust = max(0.5, min(1.0,
            1.0 - (caps.noise_figure_db - 1.0) / 10.0))

        self.timing_stability_trust = max(0.3, min(0.99,
            1.0 - (caps.timing_uncertainty_ns - 10) / 1000.0))

    def compute_trust_score(self) -> float:
        base = self.base_trust * self.uptime_fraction
        operational = base * (1.0 - self.false_positive_rate)
        multi_node_boost = self.multi_node_agreement * 0.2
        hw_factor = (
            self.frequency_stability_trust * 0.3 +
            self.sensitivity_trust * 0.3 +
            self.timing_stability_trust * 0.2
        ) + 0.2
        score = (operational + multi_node_boost) * hw_factor
        if self.thermal_throttling_active:
            score *= 0.7
        return max(0.05, min(0.98, score))

    def get_effective_sensitivity_dbm(self) -> float:
        if not self.hardware_capabilities:
            return -100.0
        mds = self.hardware_capabilities.min_signal_detectable_dbm
        if self.thermal_throttling_active:
            mds -= 3.0
        return mds

    def kujhad_base_url(self) -> str:
        scheme = "https" if self.kujhad_tls else "http"
        return f"{scheme}://{self.kujhad_host}:{self.kujhad_port}"

    def to_dict(self) -> dict:
        return {
            "node_id": self.node_id,
            "hardware_code": self.hardware_code,
            "hardware_serial": self.hardware_serial,
            "kujhad_host": self.kujhad_host,
            "kujhad_port": self.kujhad_port,
            "location_gps": self.location_gps,
            "gps_synchronized": self.gps_synchronized,
            "trust_score": self.compute_trust_score(),
            "can_do_tdoa": self.can_do_tdoa,
            "thermal_throttling_active": self.thermal_throttling_active,
            "total_observations": self.total_observations,
        }
