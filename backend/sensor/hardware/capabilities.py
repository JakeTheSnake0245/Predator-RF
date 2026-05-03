from dataclasses import dataclass, field
from enum import Enum
from typing import List, Tuple, Optional


class RXMode(Enum):
    WIDEBAND_SCAN = "wideband"
    NARROWBAND_FOCUS = "narrowband"
    DEEP_ANALYSIS = "deep_analysis"


class GainMode(Enum):
    MANUAL = "manual"
    AGC = "agc"
    ADAPTIVE = "adaptive"


@dataclass
class SDRCapabilities:
    """Hardware capabilities for an SDR device type."""

    hardware_name: str
    hardware_code: str

    freq_range_hz: Tuple[float, float]
    tuning_step_hz: float
    freq_accuracy_ppm: float

    max_sample_rate_hz: int
    typical_sample_rate_hz: int
    fft_sizes_supported: List[int] = field(default_factory=lambda: [4096, 8192, 16384])

    noise_figure_db: float = 6.0
    min_signal_detectable_dbm: float = -110.0
    dynamic_range_db: float = 60.0

    gain_modes: List[GainMode] = field(default_factory=lambda: [GainMode.MANUAL])
    gain_range_db: Tuple[float, float] = (0.0, 50.0)
    gain_step_db: float = 1.0

    antenna_ports: int = 1
    typical_antenna_gain_dbi: float = 3.0

    has_gps: bool = False
    timing_uncertainty_ns: int = 100
    pps_output: bool = False

    supports_full_duplex: bool = False
    supports_arbitrary_waveform: bool = False
    supports_fast_frequency_switching: bool = False
    supports_direct_sampling: bool = False
    supports_tdoa: bool = False

    max_parallel_detectors: int = 1
    max_iq_buffer_seconds: float = 10.0

    typical_mtbf_hours: int = 500
    thermal_throttle_temp_c: float = 85.0

    price_usd: float = 0.0
    power_consumption_watts: float = 2.0


RTL_SDR_CAPABILITIES = SDRCapabilities(
    hardware_name="RTL-SDR",
    hardware_code="rtlsdr",
    freq_range_hz=(25e6, 1700e6),
    tuning_step_hz=1000,
    freq_accuracy_ppm=50,
    max_sample_rate_hz=3_200_000,
    typical_sample_rate_hz=2_400_000,
    noise_figure_db=6.0,
    min_signal_detectable_dbm=-110,
    dynamic_range_db=60,
    gain_modes=[GainMode.MANUAL, GainMode.AGC],
    gain_range_db=(0, 50),
    has_gps=False,
    timing_uncertainty_ns=1000,
    supports_tdoa=False,
    max_parallel_detectors=1,
    typical_mtbf_hours=500,
    price_usd=30,
    power_consumption_watts=0.5,
)

HACKRF_CAPABILITIES = SDRCapabilities(
    hardware_name="HackRF One",
    hardware_code="hackrf",
    freq_range_hz=(1e6, 6e9),
    tuning_step_hz=1000,
    freq_accuracy_ppm=20,
    max_sample_rate_hz=20_000_000,
    typical_sample_rate_hz=10_000_000,
    noise_figure_db=10.0,
    min_signal_detectable_dbm=-100,
    dynamic_range_db=55,
    gain_modes=[GainMode.MANUAL, GainMode.ADAPTIVE],
    gain_range_db=(0, 40),
    has_gps=False,
    timing_uncertainty_ns=500,
    supports_full_duplex=True,
    supports_fast_frequency_switching=True,
    supports_tdoa=True,
    max_parallel_detectors=2,
    typical_mtbf_hours=1000,
    price_usd=300,
    power_consumption_watts=1.2,
)

LIMESDR_CAPABILITIES = SDRCapabilities(
    hardware_name="LimeSDR-USB",
    hardware_code="limesdr",
    freq_range_hz=(100e3, 3.8e9),
    tuning_step_hz=1.0,
    freq_accuracy_ppm=5,
    max_sample_rate_hz=61_440_000,
    typical_sample_rate_hz=30_720_000,
    fft_sizes_supported=[4096, 8192, 16384, 32768, 65536],
    noise_figure_db=3.0,
    min_signal_detectable_dbm=-120,
    dynamic_range_db=70,
    gain_modes=[GainMode.MANUAL, GainMode.AGC, GainMode.ADAPTIVE],
    gain_range_db=(0, 70),
    has_gps=False,
    timing_uncertainty_ns=100,
    supports_full_duplex=True,
    supports_arbitrary_waveform=True,
    supports_fast_frequency_switching=True,
    supports_tdoa=True,
    pps_output=True,
    antenna_ports=2,
    max_parallel_detectors=4,
    typical_mtbf_hours=2000,
    price_usd=600,
    power_consumption_watts=4.0,
)

PLUTOSDR_CAPABILITIES = SDRCapabilities(
    hardware_name="ADALM-PLUTO",
    hardware_code="plutosdr",
    freq_range_hz=(325e6, 3.8e9),
    tuning_step_hz=1.0,
    freq_accuracy_ppm=10,
    max_sample_rate_hz=61_440_000,
    typical_sample_rate_hz=30_720_000,
    noise_figure_db=5.0,
    min_signal_detectable_dbm=-115,
    dynamic_range_db=65,
    gain_modes=[GainMode.MANUAL, GainMode.AGC],
    gain_range_db=(0, 76),
    has_gps=False,
    timing_uncertainty_ns=200,
    supports_full_duplex=True,
    supports_fast_frequency_switching=True,
    supports_tdoa=True,
    max_parallel_detectors=2,
    typical_mtbf_hours=3000,
    price_usd=200,
    power_consumption_watts=2.5,
)

AIRSPY_CAPABILITIES = SDRCapabilities(
    hardware_name="Airspy R2",
    hardware_code="airspy",
    freq_range_hz=(24e6, 1700e6),
    tuning_step_hz=1.0,
    freq_accuracy_ppm=30,
    max_sample_rate_hz=20_000_000,
    typical_sample_rate_hz=10_000_000,
    noise_figure_db=2.5,
    min_signal_detectable_dbm=-125,
    dynamic_range_db=75,
    gain_modes=[GainMode.MANUAL, GainMode.AGC],
    gain_range_db=(0, 21),
    has_gps=False,
    timing_uncertainty_ns=50,
    supports_fast_frequency_switching=True,
    supports_tdoa=True,
    max_parallel_detectors=2,
    typical_mtbf_hours=2000,
    price_usd=170,
    power_consumption_watts=1.5,
)

BLADERF_CAPABILITIES = SDRCapabilities(
    hardware_name="bladeRF 2.0",
    hardware_code="bladerf",
    freq_range_hz=(47e6, 6e9),
    tuning_step_hz=1.0,
    freq_accuracy_ppm=2,
    max_sample_rate_hz=61_440_000,
    typical_sample_rate_hz=30_720_000,
    noise_figure_db=4.0,
    min_signal_detectable_dbm=-118,
    dynamic_range_db=68,
    gain_modes=[GainMode.MANUAL, GainMode.AGC],
    gain_range_db=(0, 60),
    timing_uncertainty_ns=80,
    supports_full_duplex=True,
    supports_fast_frequency_switching=True,
    supports_tdoa=True,
    max_parallel_detectors=3,
    typical_mtbf_hours=2500,
    price_usd=480,
    power_consumption_watts=3.0,
)

# SoapySDR generic fallback (unknown hardware)
SOAPY_GENERIC_CAPABILITIES = SDRCapabilities(
    hardware_name="SoapySDR Generic",
    hardware_code="soapy",
    freq_range_hz=(1e6, 6e9),
    tuning_step_hz=1000,
    freq_accuracy_ppm=30,
    max_sample_rate_hz=10_000_000,
    typical_sample_rate_hz=2_000_000,
    noise_figure_db=8.0,
    min_signal_detectable_dbm=-105,
    dynamic_range_db=55,
    gain_modes=[GainMode.MANUAL],
    timing_uncertainty_ns=500,
    supports_tdoa=False,
    max_parallel_detectors=1,
    price_usd=0,
    power_consumption_watts=2.0,
)

HARDWARE_REGISTRY: dict = {
    'rtlsdr': RTL_SDR_CAPABILITIES,
    'hackrf': HACKRF_CAPABILITIES,
    'limesdr': LIMESDR_CAPABILITIES,
    'plutosdr': PLUTOSDR_CAPABILITIES,
    'airspy': AIRSPY_CAPABILITIES,
    'bladerf': BLADERF_CAPABILITIES,
    'soapy': SOAPY_GENERIC_CAPABILITIES,
}


def get_hardware_capabilities(hardware_code: str) -> Optional[SDRCapabilities]:
    return HARDWARE_REGISTRY.get(hardware_code.lower())
