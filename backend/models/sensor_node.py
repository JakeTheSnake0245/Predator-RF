from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional, Tuple
import time

from backend.models.sdr_backend import SDRBackend


def _load_hardware_capabilities(hardware_code: str):
    """Look up hardware capabilities WITHOUT going through
    `backend.sensor.__init__`. The package's __init__ imports numpy
    (via dsp_engine) — on hosts that don't have it (CoC-only
    workstations, this Repl), the import would fail and we'd silently
    lose freq-range, max-sample-rate, and TDOA-capability data.
    Loading the leaf module file directly via importlib sidesteps the
    package init entirely."""
    import importlib.util
    import os
    try:
        path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "sensor", "hardware", "capabilities.py")
        if not os.path.isfile(path):
            return None
        spec = importlib.util.spec_from_file_location(
            "_predator_capabilities", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        getter = getattr(mod, "get_hardware_capabilities", None)
        if getter is None:
            return None
        return getter(hardware_code)
    except Exception:
        return None


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
    # When was location_gps last refreshed? UNIX ns. 0 means "never" —
    # used by TDOACoordinator to drop stale-GPS nodes from solves so
    # we don't triangulate against a position the operator drove
    # away from 20 minutes ago.
    location_gps_updated_ns: int = 0

    # Clock — both reported by the C++ /v1/timing endpoint when
    # available. Without this, timing_stability_trust is a guess
    # derived from the hardware code; with it, we use the device's
    # actual GPSDO/NTP-disciplined offset and the freshness of its
    # last PPS.
    gps_synchronized: bool = False
    clock_drift_ppm: float = 0.0
    timing_offset_ns: int = 0
    timing_source: str = ""             # "gpsdo" | "ntp" | "system" | ""
    timing_last_sync_ns: int = 0        # UNIX ns of last sync
    timing_pps_lock: bool = False       # GPS-PPS visible to the SDR clock
    timing_offset_ms: float = 0.0       # NTP/GPSDO offset reported by node

    # Operational config
    bandwidth_allocated_mhz: float = 100.0
    center_frequencies_monitored: List[float] = field(default_factory=list)

    # Optional multi-SDR profile. Empty list → "single-SDR node", keep
    # using `hardware_code` + `max_sample_rate_hz`. Populated → this node
    # has multiple physical radios and the orchestrator can task them
    # independently (e.g. one watching a control channel while another
    # sweeps). The C++ Kujhad daemon side reports its radios in
    # /v1/identify; we mirror that list here.
    sdr_backends: List[SDRBackend] = field(default_factory=list)

    # Mirror of C++ /v1/state — populated by KujhadClient capability probe.
    # These fields reflect what the *device* reports it is doing right now;
    # do not write them from the orchestrator side, they will be overwritten
    # on the next /v1/state poll.
    mission_mode_active: int = 0                              # C++ enum int
    scan_running: bool = False
    scan_status: str = ""
    threshold_db: float = 0.0                                  # detection floor
    active_search_bands_hz: List[Tuple[float, float]] = field(default_factory=list)
    record_audio: bool = False

    # Inferred from hardware identity (capability_inference module). Lists
    # of decoder/detector *names* (matching the registries) this node's
    # hardware can plausibly run. Empty until first /v1/identify succeeds.
    available_decoders: List[str] = field(default_factory=list)
    available_detectors: List[str] = field(default_factory=list)

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
        self.refresh_hardware_capabilities()

    def refresh_hardware_capabilities(self):
        """Re-look-up `hardware_capabilities` and recompute hardware-derived
        trust factors based on the current `hardware_code`. Idempotent;
        safe to call any time the hardware identity changes (e.g. after
        /v1/identify reports a different value than what was configured).

        Implementation note: we load `backend.sensor.hardware.capabilities`
        directly via `importlib.util` instead of `from … import …` because
        the `backend.sensor` package __init__ pulls in `dsp_engine` which
        imports numpy. On a CoC-only host (or any host that doesn't have
        the DSP stack installed) the package import would fail and we'd
        silently lose hardware capability data — including the per-radio
        frequency range used by SweepCoordinator. Loading the leaf module
        directly avoids the numpy chain."""
        if not self.hardware_code:
            return
        caps = _load_hardware_capabilities(self.hardware_code)
        if caps is None:
            return
        self.hardware_capabilities = caps
        self.max_sample_rate_hz = caps.max_sample_rate_hz
        self.can_do_tdoa = caps.supports_tdoa
        self._init_hardware_trust_factors(caps)

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

    # ── Multi-SDR helpers ────────────────────────────────────────────────
    def all_sdr_backends(self) -> List[SDRBackend]:
        """Return every SDR attached to this node. Synthesises a single
        backend from the legacy `hardware_code` fields if `sdr_backends`
        is empty, so callers don't have to special-case single-SDR
        nodes.

        Critical: when synthesising the legacy default we MUST pull
        the per-hardware frequency range from `hardware_capabilities`
        — otherwise a HackRF (1 MHz–6 GHz) gets clamped to
        SDRBackend's class defaults (24 MHz–1.7 GHz, RTL-SDR's range)
        and the SweepCoordinator would refuse to task it above
        1.7 GHz. Same applies to the LimeSDR, BladeRF, etc."""
        if self.sdr_backends:
            return list(self.sdr_backends)
        # Legacy path — derive freq range from capability lookup so
        # high-frequency-capable radios aren't wrongly clamped. Try
        # the cached `hardware_capabilities` first, then a fresh lookup
        # in case __post_init__'s lookup failed (e.g. import order
        # races on first construction).
        min_freq_hz = 24e6
        max_freq_hz = 1.7e9
        caps = self.hardware_capabilities or _load_hardware_capabilities(
            self.hardware_code)
        freq_range = getattr(caps, "freq_range_hz", None)
        if freq_range and len(freq_range) == 2:
            min_freq_hz = float(freq_range[0])
            max_freq_hz = float(freq_range[1])
        return [SDRBackend(
            backend_id=self.node_id + ":default",
            hardware_code=self.hardware_code,
            hardware_serial=self.hardware_serial,
            max_sample_rate_hz=self.max_sample_rate_hz,
            instantaneous_bandwidth_hz=self.max_sample_rate_hz,
            min_freq_hz=min_freq_hz,
            max_freq_hz=max_freq_hz,
            timing_stability_trust=self.timing_stability_trust,
            sensitivity_trust=self.sensitivity_trust,
            frequency_stability_trust=self.frequency_stability_trust,
        )]

    def total_instantaneous_bandwidth_hz(self) -> int:
        """Sum of the instantaneous bandwidths across all SDRs on this
        node. Used by the SweepCoordinator to allocate spectrum
        segments — a multi-SDR node can be assigned wider/multiple
        segments per phase."""
        return sum(s.instantaneous_bandwidth_hz for s in self.all_sdr_backends())

    def free_sdr_backends(self) -> List[SDRBackend]:
        """Backends not currently claimed by an active scan/decode."""
        return [s for s in self.all_sdr_backends() if not s.in_use]

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
