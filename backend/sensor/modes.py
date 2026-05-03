from dataclasses import dataclass, field
from enum import Enum
from typing import List


class SensingMode(Enum):
    WIDEBAND_SURVEY = "survey"
    NARROWBAND_MONITORING = "monitor"
    DEEP_ANALYSIS = "analysis"


@dataclass
class ModeConfig:
    mode: SensingMode
    frequency_span_hz: float
    center_frequency_hz: float
    fft_size: int
    dwell_time_per_band_ms: float
    enable_decoders: bool
    decoder_types: List[str]
    enable_iq_recording: bool
    iq_buffer_duration_s: float
    snr_threshold_db: float
    min_signal_duration_ms: float
    max_simultaneous_detections: int
    cpu_budget_percent: float


SURVEY_MODE = ModeConfig(
    mode=SensingMode.WIDEBAND_SURVEY,
    frequency_span_hz=100e6,
    center_frequency_hz=400e6,
    fft_size=4096,
    dwell_time_per_band_ms=100,
    enable_decoders=False,
    decoder_types=[],
    enable_iq_recording=False,
    iq_buffer_duration_s=0,
    snr_threshold_db=-20,
    min_signal_duration_ms=10,
    max_simultaneous_detections=100,
    cpu_budget_percent=20,
)

MONITOR_MODE = ModeConfig(
    mode=SensingMode.NARROWBAND_MONITORING,
    frequency_span_hz=10e6,
    center_frequency_hz=154e6,
    fft_size=16384,
    dwell_time_per_band_ms=500,
    enable_decoders=True,
    decoder_types=['p25', 'dmr'],
    enable_iq_recording=True,
    iq_buffer_duration_s=5,
    snr_threshold_db=-15,
    min_signal_duration_ms=50,
    max_simultaneous_detections=20,
    cpu_budget_percent=40,
)

ANALYSIS_MODE = ModeConfig(
    mode=SensingMode.DEEP_ANALYSIS,
    frequency_span_hz=1e6,
    center_frequency_hz=154e6,
    fft_size=65536,
    dwell_time_per_band_ms=2000,
    enable_decoders=True,
    decoder_types=['p25', 'dmr', 'nxdn', 'd_star'],
    enable_iq_recording=True,
    iq_buffer_duration_s=30,
    snr_threshold_db=-22,
    min_signal_duration_ms=200,
    max_simultaneous_detections=5,
    cpu_budget_percent=80,
)
