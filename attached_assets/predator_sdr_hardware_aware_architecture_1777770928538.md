# Predator-SDR: Hardware-Aware Multi-SDR Architecture
## Refactored for Heterogeneous RF Sensor Networks

**Scope:** Extends previous design to support RTL-SDR, HackRF, LimeSDR, PlutoSDR, Airspy, and other SoapySDR-compatible devices  
**Focus:** Flexibility, scalability, real-world SDR limitations  
**Constraint Handling:** Frequency stability, bandwidth, sensitivity, GPS availability

---

## PART 1: SDR ABSTRACTION LAYER

### 1.1 Hardware Capability Model

First, define what hardware *can* do. This is the foundation.

```python
# backend/sensor/hardware/capabilities.py

from dataclasses import dataclass, field
from enum import Enum
from typing import List, Tuple, Optional

class RXMode(Enum):
    """How node operates in RX mode."""
    WIDEBAND_SCAN = "wideband"      # 100+ MHz spans, low resolution
    NARROWBAND_FOCUS = "narrowband" # 1–10 MHz span, high resolution
    DEEP_ANALYSIS = "deep_analysis" # Single frequency, IQ recording

class GainMode(Enum):
    """Gain control strategy."""
    MANUAL = "manual"               # Fixed gain
    AGC = "agc"                     # Automatic gain control
    ADAPTIVE = "adaptive"           # Based on signal level

@dataclass
class SDRCapabilities:
    """Capabilities of an SDR hardware type."""
    
    # Hardware identification
    hardware_name: str              # "RTL-SDR", "HackRF", etc.
    hardware_code: str              # Device driver identifier
    
    # Frequency coverage
    freq_range_hz: Tuple[float, float]  # (min_Hz, max_Hz)
    tuning_step_hz: float           # Minimum frequency step
    freq_accuracy_ppm: float        # Frequency accuracy (parts per million)
    
    # Bandwidth
    max_sample_rate_hz: int         # Maximum I/Q sample rate
    typical_sample_rate_hz: int     # Recommended for general use
    fft_sizes_supported: List[int] = field(default_factory=lambda: [4096, 8192, 16384])
    
    # Sensitivity (RX)
    noise_figure_db: float          # Noise figure at typical gain
    min_signal_detectable_dbm: float  # Minimum detectable signal
    dynamic_range_db: float         # 1dB compression point
    
    # Gain control
    gain_modes: List[GainMode] = field(default_factory=lambda: [GainMode.MANUAL])
    gain_range_db: Tuple[float, float] = (0, 50)  # (min, max)
    gain_step_db: float = 1.0
    
    # Antenna
    antenna_ports: int = 1          # Number of antenna connectors
    typical_antenna_gain_dbi: float = 3.0
    
    # Timing & GPS
    has_gps: bool = False           # GPS receiver built-in
    timing_uncertainty_ns: int = 100  # Clock jitter
    pps_output: bool = False        # Pulse-per-second output
    
    # Special capabilities
    supports_full_duplex: bool = False
    supports_arbitrary_waveform: bool = False  # TX arbitrary signals
    supports_fast_frequency_switching: bool = False  # <100ms retune
    supports_direct_sampling: bool = False  # Bypass tuner (HF)
    supports_tdoa: bool = False     # Can participate in TDOA
    
    # Processing constraints
    max_parallel_detectors: int = 1  # How many decoders can run
    max_iq_buffer_seconds: float = 10.0  # Max I/Q recording duration
    
    # Reliability
    typical_mtbf_hours: int = 500   # Mean time between failures
    thermal_throttle_temp_c: float = 85.0  # CPU throttle temp
    
    # Cost & power
    price_usd: float = 0.0
    power_consumption_watts: float = 2.0

# Pre-defined capabilities for common hardware

RTL_SDR_CAPABILITIES = SDRCapabilities(
    hardware_name="RTL-SDR",
    hardware_code="rtlsdr",
    freq_range_hz=(25e6, 1700e6),
    tuning_step_hz=1000,
    freq_accuracy_ppm=50,            # Poor frequency stability
    max_sample_rate_hz=3_200_000,
    typical_sample_rate_hz=2_400_000,
    noise_figure_db=6.0,
    min_signal_detectable_dbm=-110,
    dynamic_range_db=60,
    gain_modes=[GainMode.MANUAL, GainMode.AGC],
    gain_range_db=(0, 50),
    has_gps=False,
    timing_uncertainty_ns=1000,      # ±1µs jitter (USB)
    supports_tdoa=False,
    max_parallel_detectors=1,
    typical_mtbf_hours=500,
    price_usd=30,
    power_consumption_watts=0.5,
)

HACKRF_CAPABILITIES = SDRCapabilities(
    hardware_name="HackRF One",
    hardware_code="hackrf",
    freq_range_hz=(1e6, 6e9),       # Better coverage
    tuning_step_hz=1000,
    freq_accuracy_ppm=20,            # Better stability
    max_sample_rate_hz=20_000_000,   # 10x better
    typical_sample_rate_hz=10_000_000,
    noise_figure_db=10.0,            # Noisier
    min_signal_detectable_dbm=-100,
    dynamic_range_db=55,
    gain_modes=[GainMode.MANUAL, GainMode.ADAPTIVE],
    gain_range_db=(0, 40),
    has_gps=False,
    timing_uncertainty_ns=500,       # Better timing
    supports_full_duplex=True,
    supports_fast_frequency_switching=True,
    supports_tdoa=True,              # Can sync externally
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
    freq_accuracy_ppm=5,             # Excellent
    max_sample_rate_hz=61_440_000,
    typical_sample_rate_hz=30_720_000,
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
    noise_figure_db=2.5,             # Best in class for narrowband
    min_signal_detectable_dbm=-125,
    dynamic_range_db=75,
    gain_modes=[GainMode.MANUAL, GainMode.AGC],
    gain_range_db=(0, 21),
    has_gps=False,
    timing_uncertainty_ns=50,        # Excellent timing
    supports_fast_frequency_switching=True,
    supports_tdoa=True,
    max_parallel_detectors=2,
    typical_mtbf_hours=2000,
    price_usd=170,
    power_consumption_watts=1.5,
)

# Registry of known hardware
HARDWARE_REGISTRY = {
    'rtlsdr': RTL_SDR_CAPABILITIES,
    'hackrf': HACKRF_CAPABILITIES,
    'limesdr': LIMESDR_CAPABILITIES,
    'plutosdr': PLUTOSDR_CAPABILITIES,
    'airspy': AIRSPY_CAPABILITIES,
}

def get_hardware_capabilities(hardware_code: str) -> Optional[SDRCapabilities]:
    """Lookup hardware capabilities."""
    return HARDWARE_REGISTRY.get(hardware_code.lower())
```

### 1.2 Abstract SDR Interface

```python
# backend/sensor/hardware/sdr_interface.py

from abc import ABC, abstractmethod
from typing import Optional, Tuple
import numpy as np
from backend.sensor.hardware.capabilities import SDRCapabilities, GainMode

class SDRInterface(ABC):
    """
    Abstract base for all SDR hardware.
    
    Design principle: Hide hardware details behind a clean interface.
    Each implementation handles driver-specific quirks.
    """
    
    def __init__(self, device_id: str, capabilities: SDRCapabilities):
        self.device_id = device_id
        self.capabilities = capabilities
        self.is_open = False
        self.current_frequency = None
        self.current_gain = None
        self.current_sample_rate = None
    
    # --- Lifecycle ---
    
    @abstractmethod
    async def open(self):
        """Open device and initialize."""
        self.is_open = True
    
    @abstractmethod
    async def close(self):
        """Close device and cleanup."""
        self.is_open = False
    
    # --- Tuning & Configuration ---
    
    @abstractmethod
    async def set_frequency(self, freq_hz: float) -> float:
        """
        Tune to frequency.
        
        Returns: Actual tuned frequency (may differ due to resolution)
        """
        self.current_frequency = freq_hz
        return freq_hz
    
    @abstractmethod
    async def set_sample_rate(self, rate_hz: int) -> int:
        """
        Set I/Q sample rate.
        
        Returns: Actual sample rate set
        """
        self.current_sample_rate = rate_hz
        return rate_hz
    
    @abstractmethod
    async def set_gain(self, gain_db: float, mode: GainMode = GainMode.MANUAL) -> float:
        """
        Set RF gain.
        
        Returns: Actual gain set
        """
        self.current_gain = gain_db
        return gain_db
    
    @abstractmethod
    async def set_antenna(self, antenna_port: int = 0):
        """Select antenna port (if multiple available)."""
        pass
    
    # --- I/Q Streaming ---
    
    @abstractmethod
    async def start_rx(self):
        """Start receiving I/Q samples."""
        pass
    
    @abstractmethod
    async def stop_rx(self):
        """Stop receiving."""
        pass
    
    @abstractmethod
    async def read_samples(self, num_samples: int) -> np.ndarray:
        """
        Read I/Q samples (complex64).
        
        Blocking call; caller should use asyncio.to_thread() if needed.
        """
        pass
    
    # --- Advanced ---
    
    async def enable_agc(self, enabled: bool = True):
        """Enable automatic gain control (if supported)."""
        if GainMode.AGC not in self.capabilities.gain_modes:
            raise NotImplementedError(f"{self.capabilities.hardware_name} doesn't support AGC")
        # Implementation specific
        pass
    
    async def get_rssi(self) -> Optional[float]:
        """Get RSSI (received signal strength indicator) in dBm."""
        # Not all hardware supports this
        return None
    
    # --- Health & Diagnostics ---
    
    async def get_temperature_c(self) -> Optional[float]:
        """Get device temperature (if available)."""
        return None
    
    async def get_serial_number(self) -> str:
        """Get device serial number."""
        return self.device_id
    
    async def get_driver_version(self) -> str:
        """Get driver version."""
        return "unknown"

```

### 1.3 Hardware-Specific Implementations

```python
# backend/sensor/hardware/rtl_sdr_impl.py

from backend.sensor.hardware.sdr_interface import SDRInterface
from backend.sensor.hardware.capabilities import RTL_SDR_CAPABILITIES, GainMode
import numpy as np

class RTLSDRInterface(SDRInterface):
    """RTL-SDR via librtlsdr (or python-rtlsdr wrapper)."""
    
    def __init__(self, device_index: int = 0):
        import rtlsdr
        self.rtl = rtlsdr.RtlSdr(device_index)
        super().__init__(
            device_id=f"rtlsdr_{device_index}",
            capabilities=RTL_SDR_CAPABILITIES
        )
        self.gain_values = None  # RTL-SDR has discrete gain values
    
    async def open(self):
        """Initialize RTL-SDR."""
        # Get available gain values
        self.gain_values = sorted(self.rtl.get_gains())
        await super().open()
    
    async def close(self):
        """Close RTL-SDR."""
        self.rtl.close()
        await super().close()
    
    async def set_frequency(self, freq_hz: float) -> float:
        """
        Tune to frequency.
        
        RTL-SDR has ~1 kHz resolution; round appropriately.
        """
        # RTL-SDR tunes in 1 kHz steps
        freq_rounded = (int(freq_hz) // 1000) * 1000
        self.rtl.center_freq = int(freq_rounded)
        
        # Account for frequency offset (RTL-SDR can have 20–50 kHz error)
        return freq_rounded
    
    async def set_sample_rate(self, rate_hz: int) -> int:
        """Set sample rate. RTL-SDR supports limited rates."""
        # RTL-SDR has limited sample rates; find closest
        allowed_rates = [250000, 960000, 1024000, 1440000, 1920000, 2048000, 2400000]
        closest = min(allowed_rates, key=lambda x: abs(x - rate_hz))
        self.rtl.sample_rate = closest
        return closest
    
    async def set_gain(self, gain_db: float, mode: GainMode = GainMode.MANUAL) -> float:
        """
        Set gain from discrete values.
        
        RTL-SDR only supports specific gain values (not continuous).
        """
        if mode == GainMode.AGC:
            self.rtl.gain = 'auto'
            return 'auto'
        
        # Find closest available gain
        if not self.gain_values:
            self.gain_values = self.rtl.get_gains()
        
        closest_gain = min(self.gain_values, key=lambda x: abs(x - gain_db))
        self.rtl.gain = closest_gain
        return closest_gain
    
    async def start_rx(self):
        """Start receiving (no explicit start for RTL-SDR)."""
        pass
    
    async def stop_rx(self):
        """Stop receiving."""
        pass
    
    async def read_samples(self, num_samples: int) -> np.ndarray:
        """Read I/Q samples."""
        # RTL-SDR read_samples blocks; caller must handle async
        samples = await asyncio.to_thread(self.rtl.read_samples, num_samples)
        return np.array(samples, dtype=np.complex64)
    
    async def get_driver_version(self) -> str:
        """Get rtlsdr library version."""
        import rtlsdr
        return rtlsdr.__version__ if hasattr(rtlsdr, '__version__') else "unknown"

# backend/sensor/hardware/hackrf_impl.py

class HackRFInterface(SDRInterface):
    """HackRF One via libhackrf."""
    
    def __init__(self, device_index: int = 0):
        import hackrf
        self.hackrf_dev = hackrf.find_devices()[device_index]
        super().__init__(
            device_id=f"hackrf_{device_index}",
            capabilities=HACKRF_CAPABILITIES
        )
        self.stream = None
    
    async def open(self):
        """Open HackRF device."""
        self.hackrf_dev.open()
        await super().open()
    
    async def close(self):
        """Close HackRF."""
        if self.stream:
            self.hackrf_dev.stop_rx()
        self.hackrf_dev.close()
        await super().close()
    
    async def set_frequency(self, freq_hz: float) -> float:
        """Tune HackRF."""
        self.hackrf_dev.frequency = int(freq_hz)
        return freq_hz
    
    async def set_sample_rate(self, rate_hz: int) -> int:
        """Set HackRF sample rate (more flexible than RTL)."""
        # HackRF supports rates from 4 MHz to 20 MHz
        rate_mhz = int(rate_hz / 1e6)
        self.hackrf_dev.sample_rate = rate_mhz * 1e6
        return int(self.hackrf_dev.sample_rate)
    
    async def set_gain(self, gain_db: float, mode: GainMode = GainMode.MANUAL) -> float:
        """Set HackRF gain (continuous)."""
        # HackRF has separate IF and RF gain controls
        lna_gain = min(max(int(gain_db // 8) * 8, 0), 40)  # 0–40 dB, 8 dB steps
        vga_gain = min(max(int((gain_db - lna_gain) * 2), 0), 62)  # 0–62 dB, 2 dB steps
        
        self.hackrf_dev.lna_gain = lna_gain
        self.hackrf_dev.vga_gain = vga_gain
        
        return lna_gain + (vga_gain / 2)
    
    async def start_rx(self):
        """Start HackRF RX streaming."""
        self.hackrf_dev.start_rx(self._rx_callback)
    
    async def stop_rx(self):
        """Stop HackRF RX."""
        self.hackrf_dev.stop_rx()
    
    async def read_samples(self, num_samples: int) -> np.ndarray:
        """Read from HackRF stream buffer."""
        # HackRF uses callback-based streaming; implement buffer
        # This is pseudo-code; actual impl uses queue
        return self._sample_buffer[:num_samples]
    
    def _rx_callback(self, transfer):
        """Callback for HackRF RX (invoked from C)."""
        # Push samples to async queue for read_samples() to retrieve
        pass

# backend/sensor/hardware/limesdr_impl.py

class LimeSDRInterface(SDRInterface):
    """LimeSDR via liblimesuite."""
    
    def __init__(self, device_id: str = "LimeSDR-USB"):
        import lime
        self.lime = lime.LimeSDR(device_id)
        super().__init__(
            device_id=device_id,
            capabilities=LIMESDR_CAPABILITIES
        )
    
    async def open(self):
        """Open LimeSDR."""
        self.lime.open()
        await super().open()
    
    async def close(self):
        """Close LimeSDR."""
        self.lime.close()
        await super().close()
    
    async def set_frequency(self, freq_hz: float) -> float:
        """Tune LimeSDR."""
        self.lime.set_rf_freq(int(freq_hz))
        return freq_hz
    
    async def set_sample_rate(self, rate_hz: int) -> int:
        """Set LimeSDR sample rate (very flexible)."""
        self.lime.set_sample_rate(rate_hz)
        return rate_hz
    
    async def set_gain(self, gain_db: float, mode: GainMode = GainMode.MANUAL) -> float:
        """Set LimeSDR gain (continuous, 0–70 dB)."""
        self.lime.set_gain(int(gain_db))
        return gain_db
    
    async def read_samples(self, num_samples: int) -> np.ndarray:
        """Read from LimeSDR."""
        samples = await asyncio.to_thread(self.lime.read_samples, num_samples)
        return np.array(samples, dtype=np.complex64)

```

---

## PART 2: HARDWARE-AWARE SENSOR NODE MODEL

### 2.1 Enhanced SensorNode with Hardware Properties

```python
# backend/models/sensor_node.py (REFACTORED)

from dataclasses import dataclass, field
from typing import Dict, Optional
from enum import Enum
from backend.sensor.hardware.capabilities import SDRCapabilities

class NodeRole(Enum):
    """How this node operates in the network."""
    WIDEBAND_SCANNER = "wideband"     # Scan 100+ MHz spans
    NARROWBAND_MONITOR = "narrowband"  # Focus on specific frequencies
    DEEP_ANALYZER = "analyzer"        # Record IQ, perform deep processing
    MULTI_ROLE = "multi"              # Switch between roles

@dataclass
class SensorNodeTrust:
    """Enhanced trust model with hardware awareness."""
    
    # Identity
    node_id: str
    node_role: NodeRole = NodeRole.MULTI_ROLE
    
    # Hardware
    hardware_code: str                 # 'rtlsdr', 'hackrf', etc.
    hardware_capabilities: Optional[SDRCapabilities] = None
    hardware_serial: str = ""
    hardware_age_days: int = 0
    
    # Location & timing
    location_gps: Optional[Tuple[float, float]] = None  # (lat, lon)
    location_accuracy_m: float = 10.0
    
    # Clock & synchronization
    gps_synchronized: bool = False     # Has GPS PPS sync
    clock_drift_ppm: float = 0.0       # Measured clock error
    timing_offset_ns: int = 0          # Offset relative to reference
    
    # Operational configuration
    bandwidth_allocated_mhz: float = 100.0  # Typical span monitored
    center_frequencies_monitored: List[float] = field(default_factory=list)
    
    # Trust components
    base_trust: float = 0.6
    uptime_fraction: float = 1.0
    false_positive_rate: float = 0.0
    multi_node_agreement: float = 1.0
    
    # Hardware-specific trust factors
    frequency_stability_trust: float = 1.0  # Based on freq_accuracy_ppm
    sensitivity_trust: float = 1.0          # Based on noise figure
    timing_stability_trust: float = 1.0     # Based on jitter
    
    # Calibration
    frequency_calibration_offset_hz: float = 0.0  # Frequency error correction
    gain_calibration_factor: float = 1.0          # Power measurement correction
    last_calibration_ns: int = 0
    
    # Processing capacity
    can_do_wideband_scan: bool = True
    can_do_narrowband_focus: bool = True
    can_do_iq_recording: bool = True
    can_do_tdoa: bool = False         # Set by hardware capability
    max_concurrent_decoders: int = 1
    
    # Operational limits
    max_sample_rate_hz: int = 2_000_000
    max_fft_size: int = 4096
    thermal_throttling_active: bool = False
    
    # Observations
    total_observations: int = 0
    observations_corroborated: int = 0
    observations_flagged_anomalous: int = 0
    
    def __post_init__(self):
        """Initialize hardware-aware properties."""
        if self.hardware_code:
            from backend.sensor.hardware.capabilities import get_hardware_capabilities
            self.hardware_capabilities = get_hardware_capabilities(self.hardware_code)
            
            if self.hardware_capabilities:
                # Inherit hardware limits
                self.max_sample_rate_hz = self.hardware_capabilities.max_sample_rate_hz
                self.can_do_tdoa = self.hardware_capabilities.supports_tdoa
                
                # Set trust factors based on hardware quality
                self._initialize_hardware_trust_factors()
    
    def _initialize_hardware_trust_factors(self):
        """Set trust factors from hardware specs."""
        caps = self.hardware_capabilities
        
        # Frequency stability: better PPM = higher trust
        # RTL-SDR (50 ppm) → 0.7 trust; Airspy (30 ppm) → 0.9 trust
        max_ppm = 100
        min_ppm = 1
        trust_ppm = 1.0 - ((caps.freq_accuracy_ppm - min_ppm) / (max_ppm - min_ppm))
        self.frequency_stability_trust = max(min(trust_ppm, 1.0), 0.5)
        
        # Sensitivity: better noise figure = higher trust
        # 10 dB → 0.6 trust; 3 dB → 0.95 trust
        trust_nf = 1.0 - ((caps.noise_figure_db - 1.0) / 10.0)
        self.sensitivity_trust = max(min(trust_nf, 1.0), 0.5)
        
        # Timing: smaller jitter = higher trust
        # 1000 ns (RTL-SDR) → 0.7; 50 ns (Airspy) → 0.99
        trust_timing = 1.0 - ((caps.timing_uncertainty_ns - 10) / 1000)
        self.timing_stability_trust = max(min(trust_timing, 0.99), 0.3)
    
    def compute_hardware_aware_trust_score(self) -> float:
        """
        Compute trust score accounting for hardware quality.
        
        Poor hardware (RTL-SDR) can still be trusted if it:
        - Has good uptime
        - Produces low FP rate
        - Has multi-node agreement
        """
        
        base = self.base_trust * self.uptime_fraction
        
        # Operational trust
        operational = base * (1.0 - self.false_positive_rate)
        
        # Multi-node agreement boost
        multi_node_boost = self.multi_node_agreement * 0.2
        
        # Hardware factor (dampen poor hardware, boost good hardware)
        hw_factor = (
            self.frequency_stability_trust * 0.3 +
            self.sensitivity_trust * 0.3 +
            self.timing_stability_trust * 0.2
        ) + 0.2  # Base hardware allowance
        
        final = (operational + multi_node_boost) * hw_factor
        
        # Penalize if thermally throttled
        if self.thermal_throttling_active:
            final *= 0.7
        
        return max(min(final, 0.98), 0.05)
    
    def get_effective_sensitivity_dbm(self) -> float:
        """
        Get effective minimum detectable signal accounting for:
        - Hardware noise figure
        - Current gain setting
        - Thermal throttling
        """
        
        if not self.hardware_capabilities:
            return -100  # Conservative default
        
        mds = self.hardware_capabilities.min_signal_detectable_dbm
        
        # Degrade if thermally throttled
        if self.thermal_throttling_active:
            mds -= 3  # 3 dB degradation
        
        return mds

```

---

## PART 3: MULTI-RESOLUTION SENSING FRAMEWORK

### 3.1 Sensing Modes

The system operates in three layers simultaneously:

```python
# backend/sensor/modes.py

from enum import Enum
from dataclasses import dataclass

class SensingMode(Enum):
    """Three-layer sensing strategy."""
    
    WIDEBAND_SURVEY = "survey"        # Layer 1: Scan wide spectrum, low res
    NARROWBAND_MONITORING = "monitor"  # Layer 2: Watch high-confidence freqs
    DEEP_ANALYSIS = "analysis"        # Layer 3: Record IQ, classify

@dataclass
class ModeConfig:
    """Configuration for each sensing mode."""
    
    mode: SensingMode
    
    # Spectrum coverage
    frequency_span_hz: float          # How wide to scan
    center_frequency_hz: float        # Where to focus
    
    # Processing
    fft_size: int                     # 4096 (survey) → 65536 (analysis)
    dwell_time_per_band_ms: float     # How long at each frequency
    
    # Decoder engagement
    enable_decoders: bool
    decoder_types: List[str]          # Which decoders to use
    
    # I/Q recording
    enable_iq_recording: bool
    iq_buffer_duration_s: float       # How long to buffer
    
    # Detection sensitivity
    snr_threshold_db: float           # Peak detection threshold
    min_signal_duration_ms: float     # Minimum signal length
    
    # Resource limits
    max_simultaneous_detections: int  # Cap tracks being processed
    cpu_budget_percent: float         # Max CPU to allocate

# Layer 1: Wideband Survey
SURVEY_MODE = ModeConfig(
    mode=SensingMode.WIDEBAND_SURVEY,
    frequency_span_hz=100e6,          # 100 MHz sweep
    center_frequency_hz=400e6,        # Scan around 400 MHz
    fft_size=4096,                    # Fast FFT
    dwell_time_per_band_ms=100,       # Quick scan
    enable_decoders=False,            # No decoding; raw peak detection only
    decoder_types=[],
    enable_iq_recording=False,
    snr_threshold_db=-20,             # Loose threshold; get everything
    min_signal_duration_ms=10,
    max_simultaneous_detections=100,
    cpu_budget_percent=20,
)

# Layer 2: Narrowband Monitoring
MONITOR_MODE = ModeConfig(
    mode=SensingMode.NARROWBAND_MONITORING,
    frequency_span_hz=10e6,           # 10 MHz focus on high-confidence freqs
    center_frequency_hz=154e6,        # Follow tasking
    fft_size=16384,                   # Better resolution
    dwell_time_per_band_ms=500,       # Longer dwell
    enable_decoders=True,             # Try to classify
    decoder_types=['p25', 'dmr'],
    enable_iq_recording=True,         # Buffer for post-detection analysis
    iq_buffer_duration_s=5,
    snr_threshold_db=-15,
    min_signal_duration_ms=50,
    max_simultaneous_detections=20,
    cpu_budget_percent=40,
)

# Layer 3: Deep Analysis
ANALYSIS_MODE = ModeConfig(
    mode=SensingMode.DEEP_ANALYSIS,
    frequency_span_hz=1e6,            # Focus on single anomalous signal
    center_frequency_hz=154e6,
    fft_size=65536,                   # Maximum resolution
    dwell_time_per_band_ms=2000,      # Long observation
    enable_decoders=True,
    decoder_types=['p25', 'dmr', 'nxdn', 'd_star'],
    enable_iq_recording=True,
    iq_buffer_duration_s=30,          # Long buffer for recording
    snr_threshold_db=-22,             # Sensitive detection
    min_signal_duration_ms=200,
    max_simultaneous_detections=5,
    cpu_budget_percent=80,
)

```

### 3.2 Adaptive Mode Selection

```python
# backend/coordination/mode_selector.py

class AdaptiveModeSelector:
    """
    Decide which sensing mode each node should use based on:
    - Hardware capability
    - Available CPU/power
    - Tasking priority
    - Activity detected
    """
    
    async def recommend_mode(self, node: SensorNodeTrust,
                            track_priority: str,
                            cpu_available_percent: float,
                            power_available_percent: float) -> ModeConfig:
        """
        Recommend sensing mode for node.
        
        Args:
            node: Node state + capabilities
            track_priority: 'CRITICAL', 'HIGH', 'NORMAL', 'LOW'
            cpu_available_percent: 0–100
            power_available_percent: 0–100 (for battery nodes)
        
        Returns: ModeConfig to use
        """
        
        # Poor hardware → survey mode only
        if not node.hardware_capabilities:
            return SURVEY_MODE
        
        # Resource-constrained → survey mode
        if cpu_available_percent < 20 or power_available_percent < 30:
            return SURVEY_MODE
        
        # High-priority tracks → analysis mode
        if track_priority == 'CRITICAL':
            if node.hardware_capabilities.supports_tdoa:
                # TDOA-capable hardware: lock frequency, record IQ
                return ANALYSIS_MODE
            else:
                # Limited hardware: still try narrowband monitoring
                return MONITOR_MODE
        
        # Normal operation → monitor high-confidence, survey gaps
        if track_priority in ('HIGH', 'NORMAL'):
            if cpu_available_percent > 50:
                return MONITOR_MODE
            else:
                return SURVEY_MODE
        
        # Low priority → survey mode (background monitoring)
        return SURVEY_MODE

```

---

## PART 4: ADAPTIVE PROCESSING PIPELINES

### 4.1 Hardware-Specific DSP

```python
# backend/sensor/dsp_engine.py (REFACTORED)

class HardwareAdaptiveDSP:
    """
    DSP processing that adapts to hardware capabilities.
    """
    
    def __init__(self, node: SensorNodeTrust, config: ModeConfig):
        self.node = node
        self.config = config
        self.capabilities = node.hardware_capabilities
        
        # Adjust FFT size based on hardware + mode
        self.fft_size = self._select_fft_size()
        
        # Peak detection threshold based on hardware sensitivity
        self.snr_threshold = self._adjust_snr_threshold()
        
        # Frequency resolution based on hardware accuracy
        self.freq_resolution = self._compute_freq_resolution()
        
        # Deduplication window based on hardware stability
        self.dedup_window_ns = self._compute_dedup_window()
    
    def _select_fft_size(self) -> int:
        """Pick FFT size based on hardware + mode."""
        
        requested_size = self.config.fft_size
        
        # Check hardware limits
        if not self.capabilities:
            return 4096  # Safe default
        
        # RTL-SDR might not support large FFTs; cap it
        if self.capabilities.hardware_code == 'rtlsdr':
            return min(requested_size, 8192)
        
        # LimeSDR can do larger FFTs
        if self.capabilities.hardware_code == 'limesdr':
            return requested_size  # Up to 65536
        
        return min(requested_size, 16384)
    
    def _adjust_snr_threshold(self) -> float:
        """
        Adjust peak detection threshold based on hardware noise figure.
        
        Better hardware (lower NF) → more sensitive detection
        Worse hardware (higher NF) → less sensitive detection
        """
        
        base_threshold = self.config.snr_threshold_db
        
        if not self.capabilities:
            return base_threshold
        
        # Normalize noise figure to adjustment
        # 3 dB (best) → +5 dB to threshold (sensitive)
        # 10 dB (worst) → -3 dB to threshold (loose)
        
        nf = self.capabilities.noise_figure_db
        adjustment = (3.0 - nf) * 0.4  # Scale by 0.4 dB per dB of NF
        
        return base_threshold + adjustment
    
    def _compute_freq_resolution(self) -> float:
        """
        Frequency resolution = sample_rate / FFT_size.
        
        RTL-SDR (poor tuning) → need coarse resolution, bigger tolerance
        LimeSDR (excellent tuning) → can use fine resolution
        """
        
        sample_rate = self.node.max_sample_rate_hz
        resolution = sample_rate / self.fft_size
        
        # Apply hardware tolerance
        if not self.capabilities:
            return resolution
        
        # Poor frequency accuracy → pad tolerance
        freq_accuracy_hz = (self.capabilities.freq_accuracy_ppm / 1e6) * sample_rate
        
        return max(resolution, freq_accuracy_hz / 2)
    
    def _compute_dedup_window(self) -> int:
        """
        Deduplication window based on frequency stability.
        
        Stable hardware (Airspy): short window (2s)
        Unstable hardware (RTL-SDR): long window (10s)
        """
        
        if not self.capabilities:
            return 5_000_000_000  # 5 seconds default
        
        # PPM error → window_seconds
        # 10 ppm (good) → 2 seconds
        # 50 ppm (poor) → 10 seconds
        
        ppm = self.capabilities.freq_accuracy_ppm
        window_s = 2.0 + ((ppm - 10) / 40) * 8  # 2–10 seconds
        window_s = max(min(window_s, 15), 1)
        
        return int(window_s * 1e9)
    
    async def process_chunk(self, iq_samples: np.ndarray,
                           timestamp_ns: int) -> List[RFEvent]:
        """
        Process I/Q chunk with hardware-aware adjustments.
        """
        
        events = []
        
        # Window selection based on hardware
        if self.capabilities and self.capabilities.freq_accuracy_ppm > 30:
            # Poor hardware: use wider window (less spectral leakage sensitivity)
            window = np.blackman(len(iq_samples))
        else:
            # Good hardware: use Hann window (good all-around)
            window = np.hanning(len(iq_samples))
        
        windowed = iq_samples * window
        fft = np.abs(np.fft.fft(windowed, n=self.fft_size))
        
        # Normalize
        max_val = np.max(fft) if np.max(fft) > 0 else 1
        fft_dbfs = 20 * np.log10(fft / max_val)
        
        # Peak detection with hardware-adjusted threshold
        from scipy.signal import find_peaks
        peaks, _ = find_peaks(fft_dbfs, height=self.snr_threshold)
        
        for peak_idx in peaks:
            if peak_idx == 0 or peak_idx >= len(fft) // 2:
                continue
            
            freq_hz = (peak_idx / len(fft)) * self.node.max_sample_rate_hz
            power_dbfs = fft_dbfs[peak_idx]
            
            # Apply frequency calibration correction
            freq_corrected = freq_hz + self.node.frequency_calibration_offset_hz
            
            # Apply gain calibration correction
            power_corrected = power_dbfs * self.node.gain_calibration_factor
            
            event = RFEvent(
                frequency=freq_corrected,
                power_dbfs=power_corrected,
                snr_db=power_dbfs - np.median(fft_dbfs),
                timestamp_ns=timestamp_ns,
                node_id=self.node.node_id,
                node_trust_score=self.node.compute_hardware_aware_trust_score(),
                hardware_id=self.node.hardware_serial,
                detector="hardware_adaptive_fft",
            )
            
            events.append(event)
        
        return events

```

---

## PART 5: HARDWARE-AWARE FUSION ENGINE

### 5.1 Enhanced Track Associator

```python
# backend/fusion/track_associator.py (REFACTORED)

class HardwareAwareAssociator:
    """
    Track association accounting for hardware differences.
    """
    
    def __init__(self):
        self.freq_tolerance_hz = 5000
        self.time_window_s = 60
        self.power_variation_db = 20
    
    async def associate(self, event: RFEvent,
                       tracks: Dict[str, EmitterTrack],
                       sensor_nodes: Dict[str, SensorNodeTrust]) -> Tuple[Optional[str], float]:
        """
        Match event to track, accounting for hardware differences.
        """
        
        candidates = self._generate_candidates(event, tracks)
        
        if not candidates:
            return None, 0.0
        
        # Score candidates with hardware-aware logic
        scored = []
        for cand_id in candidates:
            score = self._compute_hardware_aware_score(
                event,
                tracks[cand_id],
                sensor_nodes[event.node_id],
                sensor_nodes  # For multi-node scoring
            )
            scored.append((cand_id, score))
        
        scored.sort(key=lambda x: x[1], reverse=True)
        
        best_id, best_score = scored[0]
        
        if best_score < 0.4:
            return None, 0.0
        
        return best_id, best_score
    
    def _compute_hardware_aware_score(self, event: RFEvent,
                                     track: EmitterTrack,
                                     detecting_node: SensorNodeTrust,
                                     all_nodes: Dict[str, SensorNodeTrust]) -> float:
        """
        Score includes hardware capability weighting.
        """
        
        score = 0.0
        
        # Frequency match (0.3 weight)
        freq_delta = abs(track.primary_frequency - event.frequency)
        
        # Account for detecting node's frequency accuracy
        freq_tolerance = self._compute_freq_tolerance(detecting_node)
        freq_match = max(0, 1.0 - (freq_delta / freq_tolerance))
        score += freq_match * 0.3
        
        # Time match (0.2 weight)
        time_delta_s = (event.timestamp_ns - track.last_seen_ns) / 1e9
        time_match = max(0, 1.0 - (time_delta_s / self.time_window_s))
        score += time_match * 0.2
        
        # Power match (0.2 weight), accounting for hardware sensitivity
        if track.last_power_dbfs is not None:
            # Only compare power if hardware similar sensitivity
            power_delta = abs(event.power_dbfs - track.last_power_dbfs)
            
            # Hardware with poor sensitivity might report noise as signal
            sensitivity_diff = self._compute_sensitivity_difference(
                detecting_node,
                track.most_trustworthy_node,
                all_nodes
            )
            
            # More lenient comparison if sensitivity differs
            power_match = max(0, 1.0 - (power_delta / (self.power_variation_db + sensitivity_diff)))
            score += power_match * 0.2
        else:
            score += 0.2
        
        # Track state (0.15 weight)
        state_bonus = {
            TrackState.NEW: 0.0,
            TrackState.TRACKING: 0.1,
            TrackState.STABLE: 0.15,
        }
        score += state_bonus.get(track.state, 0.0)
        
        # Hardware concordance (0.15 weight)
        # Bonus if detecting with different hardware than track primary observer
        if track.most_trustworthy_node and detecting_node.node_id != track.most_trustworthy_node:
            if self._hardware_types_compatible(
                detecting_node,
                all_nodes[track.most_trustworthy_node]
            ):
                score += 0.15  # Multi-hardware corroboration bonus
        
        return min(score, 1.0)
    
    def _compute_freq_tolerance(self, node: SensorNodeTrust) -> float:
        """
        Frequency matching tolerance based on hardware quality.
        
        Poor hardware (RTL-SDR, 50 ppm) → 10 kHz tolerance
        Good hardware (Airspy, 30 ppm) → 5 kHz tolerance
        Excellent (LimeSDR, 5 ppm) → 2 kHz tolerance
        """
        
        if not node.hardware_capabilities:
            return 5000
        
        ppm = node.hardware_capabilities.freq_accuracy_ppm
        tolerance_hz = (ppm / 1e6) * node.max_sample_rate_hz
        
        # Add minimum tolerance
        return max(tolerance_hz, 1000)
    
    def _compute_sensitivity_difference(self, node1: SensorNodeTrust,
                                       node2_id: Optional[str],
                                       all_nodes: Dict[str, SensorNodeTrust]) -> float:
        """
        How different are these nodes' sensitivities?
        
        Similar hardware → 0 dB difference
        Different hardware → adjust power tolerance
        """
        
        if not node2_id or node2_id not in all_nodes:
            return 0.0
        
        node2 = all_nodes[node2_id]
        
        if not node1.hardware_capabilities or not node2.hardware_capabilities:
            return 0.0
        
        # Difference in noise figure (proxy for sensitivity)
        nf_diff = abs(node1.hardware_capabilities.noise_figure_db -
                     node2.hardware_capabilities.noise_figure_db)
        
        return nf_diff * 2  # Convert dB difference to power tolerance
    
    def _hardware_types_compatible(self, node1: SensorNodeTrust,
                                  node2: SensorNodeTrust) -> bool:
        """Can these two hardware types meaningfully corroborate?"""
        
        # All SoapySDR devices are compatible with each other
        # (even if sensitivities differ)
        return True

```

---

## PART 6: PLUGIN ARCHITECTURE

### 6.1 Detection Algorithm Registry

```python
# backend/sensor/detection/detector_registry.py

from abc import ABC, abstractmethod
from typing import Dict, Type

class DetectionAlgorithm(ABC):
    """Base class for detection algorithms."""
    
    @abstractmethod
    async def detect(self, iq_samples: np.ndarray,
                    center_freq: float,
                    sample_rate: int) -> List[RFEvent]:
        """Detect signals in I/Q samples."""
        pass
    
    @abstractmethod
    def get_capability(self) -> Dict:
        """Return what this detector can do."""
        pass

class FFTPeakDetector(DetectionAlgorithm):
    """Standard FFT-based peak detection."""
    
    async def detect(self, iq_samples, center_freq, sample_rate):
        # ... FFT implementation ...
        pass
    
    def get_capability(self):
        return {
            'name': 'fft_peak',
            'hardware_requirements': ['any'],
            'processing_delay_ms': 10,
            'cpu_percent': 20,
        }

class EnergyDetector(DetectionAlgorithm):
    """Energy-based detection (good for low-power hardware)."""
    
    async def detect(self, iq_samples, center_freq, sample_rate):
        # ... Energy detection implementation ...
        pass
    
    def get_capability(self):
        return {
            'name': 'energy',
            'hardware_requirements': ['low_power'],
            'processing_delay_ms': 5,
            'cpu_percent': 5,  # Very light
        }

class DetectorRegistry:
    """Plugin registry for detection algorithms."""
    
    def __init__(self):
        self.detectors: Dict[str, Type[DetectionAlgorithm]] = {}
        self._register_defaults()
    
    def _register_defaults(self):
        """Register built-in detectors."""
        self.register('fft_peak', FFTPeakDetector)
        self.register('energy', EnergyDetector)
    
    def register(self, name: str, detector_class: Type[DetectionAlgorithm]):
        """Register a detector algorithm."""
        self.detectors[name] = detector_class
    
    def get_detector(self, name: str) -> Optional[DetectionAlgorithm]:
        """Instantiate a detector."""
        if name in self.detectors:
            return self.detectors[name]()
        return None
    
    def select_optimal_detector(self, node: SensorNodeTrust,
                               cpu_available: float) -> DetectionAlgorithm:
        """
        Select best detector for node + resources.
        """
        
        # Poor hardware + limited CPU → energy detector
        if not node.hardware_capabilities or cpu_available < 20:
            return self.get_detector('energy')
        
        # Default: FFT peak detector
        return self.get_detector('fft_peak')

# Global registry
detector_registry = DetectorRegistry()

```

### 6.2 Decoder Plugin System

```python
# backend/sensor/decoders/decoder_registry.py

class SignalDecoder(ABC):
    """Base class for signal decoders."""
    
    @abstractmethod
    async def decode(self, center_freq: float, iq_stream: bytes) -> Optional[Dict]:
        """
        Attempt to decode signal.
        
        Returns: Decoded payload or None if not this signal type
        """
        pass
    
    @abstractmethod
    def get_capability(self) -> Dict:
        """Return decoder metadata."""
        pass

class P25Decoder(SignalDecoder):
    """P25 voice decoder (via DSD-FME)."""
    
    async def decode(self, center_freq, iq_stream):
        # Invoke dsd-fme subprocess
        pass
    
    def get_capability(self):
        return {
            'name': 'p25',
            'frequency_bands': ['vhf', 'uhf'],
            'processing_latency_ms': 100,
            'cpu_percent': 30,
            'external_dependency': 'dsd-fme',
        }

class DMRDecoder(SignalDecoder):
    """DMR voice decoder."""
    
    async def decode(self, center_freq, iq_stream):
        # DMR decoding
        pass
    
    def get_capability(self):
        return {
            'name': 'dmr',
            'frequency_bands': ['vhf', 'uhf'],
            'processing_latency_ms': 80,
            'cpu_percent': 25,
        }

class DecoderRegistry:
    """Plugin registry for decoders."""
    
    def __init__(self):
        self.decoders: Dict[str, Type[SignalDecoder]] = {}
        self._register_defaults()
    
    def _register_defaults(self):
        """Register built-in decoders."""
        self.register('p25', P25Decoder)
        self.register('dmr', DMRDecoder)
        # ... other decoders ...
    
    def register(self, name: str, decoder_class: Type[SignalDecoder]):
        """Register a decoder."""
        self.decoders[name] = decoder_class
    
    def get_decoders_for_frequency(self, center_freq: float) -> List[SignalDecoder]:
        """Get all decoders that might work at this frequency."""
        
        decoders = []
        
        for name, decoder_class in self.decoders.items():
            decoder = decoder_class()
            cap = decoder.get_capability()
            
            # Check if frequency band matches
            band = self._freq_to_band(center_freq)
            if band in cap.get('frequency_bands', []):
                decoders.append(decoder)
        
        return decoders
    
    @staticmethod
    def _freq_to_band(freq_hz: float) -> str:
        """Map frequency to band (vhf, uhf, etc.)."""
        if 30e6 <= freq_hz < 300e6:
            return 'vhf'
        elif 300e6 <= freq_hz < 3e9:
            return 'uhf'
        elif 3e9 <= freq_hz < 30e9:
            return 'shf'
        else:
            return 'hf'

decoder_registry = DecoderRegistry()

```

---

## PART 7: CALIBRATION LAYER

### 7.1 Frequency & Gain Calibration

```python
# backend/sensor/calibration/calibrator.py

class SensorCalibrator:
    """
    Calibrate frequency offset and gain per node + hardware.
    """
    
    async def calibrate_frequency(self, node: SensorNodeTrust,
                                  sdr: SDRInterface,
                                  reference_frequency_hz: float,
                                  reference_power_dbm: float) -> float:
        """
        Measure frequency offset using a known reference signal.
        
        Common sources:
        - GPS disciplined oscillator (best, <1 ppm)
        - NIST radio station (WWV at 10 MHz)
        - Known FM station
        - Test signal from signal generator
        
        Returns: Frequency offset in Hz
        """
        
        # Tune to reference frequency
        await sdr.set_frequency(reference_frequency_hz)
        
        # Capture samples
        samples = await sdr.read_samples(100_000)
        
        # Compute FFT
        fft = np.abs(np.fft.fft(samples))
        
        # Find peak
        peak_idx = np.argmax(fft[:len(fft)//2])
        measured_freq_hz = (peak_idx / len(fft)) * sdr.current_sample_rate
        
        # Compute offset
        offset_hz = measured_freq_hz - reference_frequency_hz
        
        # Store calibration
        node.frequency_calibration_offset_hz = offset_hz
        node.last_calibration_ns = time.time_ns()
        
        logger.info(f'{node.node_id}: Frequency offset = {offset_hz:.1f} Hz')
        
        return offset_hz
    
    async def calibrate_gain(self, node: SensorNodeTrust,
                            sdr: SDRInterface,
                            reference_power_dbm: float,
                            reference_frequency_hz: float) -> float:
        """
        Measure gain correction factor using known reference signal.
        
        Args:
            reference_power_dbm: Known signal power at antenna
        
        Returns: Calibration factor (multiply measured power by this)
        """
        
        await sdr.set_frequency(reference_frequency_hz)
        
        # Measure received power
        samples = await sdr.read_samples(100_000)
        measured_power_linear = np.mean(np.abs(samples) ** 2)
        measured_power_dbfs = 10 * np.log10(measured_power_linear)
        
        # Compute correction
        # reference_power_dbm = measured_power_dbfs + correction
        correction_db = reference_power_dbm - measured_power_dbfs
        correction_factor = 10 ** (correction_db / 10)
        
        node.gain_calibration_factor = correction_factor
        node.last_calibration_ns = time.time_ns()
        
        logger.info(f'{node.node_id}: Gain calibration = {correction_db:.1f} dB')
        
        return correction_factor
    
    async def full_calibration(self, node: SensorNodeTrust,
                              sdr: SDRInterface):
        """
        Perform full calibration (frequency + gain).
        
        Requires:
        - GPS-disciplined oscillator (or NIST radio station)
        - Known reference signal
        """
        
        # For testing: use NIST station (WWV, 10 MHz)
        reference_freq_hz = 10e6
        reference_power_dbm = -50  # Typical signal strength at 10 MHz
        
        await self.calibrate_frequency(node, sdr, reference_freq_hz, reference_power_dbm)
        await self.calibrate_gain(node, sdr, reference_power_dbm, reference_freq_hz)

```

---

## PART 8: TDOA COORDINATION

### 8.1 TDOA-Aware Tracking

```python
# backend/fusion/tdoa_coordinator.py

from enum import Enum

class TDOACapability(Enum):
    """TDOA support level."""
    NONE = "none"                # Cannot do TDOA
    PASSIVE = "passive"          # Can receive sync signal
    ACTIVE = "active"            # Can transmit/receive sync (dual clock)
    GPS_DISCIPLINED = "gps"      # GPS-locked timing

@dataclass
class TDOATrack:
    """Track with TDOA metadata."""
    
    base_track: EmitterTrack
    tdoa_nodes: List[str]                      # Nodes that have timing data
    time_differences: Dict[Tuple[str, str], int]  # (node1, node2) → time diff (ns)
    estimated_location: Optional[Tuple[float, float]] = None  # (lat, lon)
    location_confidence: float = 0.0
    tdoa_capable_hardware: bool = False
    
    async def update_with_tdoa(self, timing_measurements: Dict[str, int]):
        """
        Update track with new TDOA measurements.
        
        Args:
            timing_measurements: {node_id: timestamp_ns, ...}
        """
        
        if len(timing_measurements) < 2:
            return  # Need at least 2 nodes for TDOA
        
        # Compute pairwise time differences
        nodes = list(timing_measurements.keys())
        for i, node1 in enumerate(nodes):
            for node2 in nodes[i+1:]:
                time_diff = timing_measurements[node1] - timing_measurements[node2]
                self.time_differences[(node1, node2)] = time_diff
        
        # Solve for location (requires node positions)
        # This is complex; simplified version:
        estimated_loc = await self._solve_tdoa(timing_measurements)
        self.estimated_location = estimated_loc
        self.tdoa_nodes = nodes

class TDOACoordinator:
    """
    Coordinate TDOA measurements between nodes.
    """
    
    def __init__(self):
        self.tdoa_tracks: Dict[str, TDOATrack] = {}
    
    async def register_tdoa_capable_nodes(self, nodes: List[SensorNodeTrust]):
        """
        Register nodes that can participate in TDOA.
        """
        
        tdoa_nodes = [n for n in nodes
                     if n.can_do_tdoa and n.hardware_capabilities.supports_tdoa]
        
        logger.info(f'TDOA system: {len(tdoa_nodes)} capable nodes')
    
    async def coordinate_tdoa_measurement(self, track: EmitterTrack,
                                         available_nodes: List[SensorNodeTrust]):
        """
        Attempt TDOA measurement for a track.
        
        If >=2 nodes have timing sync + GPS, can triangulate emitter location.
        """
        
        tdoa_capable = [n for n in available_nodes if n.can_do_tdoa]
        
        if len(tdoa_capable) < 2:
            return  # Not enough nodes
        
        # Request timing measurements from capable nodes
        timing_measurements = {}
        
        for node in tdoa_capable:
            # Each node records timestamp when it detects the emitter
            # (details implementation-specific)
            timestamp = await self._request_timing_from_node(node, track)
            if timestamp:
                timing_measurements[node.node_id] = timestamp
        
        # Create TDOA track
        if len(timing_measurements) >= 2:
            tdoa_track = TDOATrack(
                base_track=track,
                tdoa_capable_hardware=True,
            )
            await tdoa_track.update_with_tdoa(timing_measurements)
            self.tdoa_tracks[track.emitter_id] = tdoa_track
            
            logger.info(f'TDOA location estimate for {track.emitter_id}: '
                       f'{tdoa_track.estimated_location}')

```

---

## PART 9: INTEGRATION SUMMARY

### 9.1 Updated Sensor Node Manager

```python
# backend/sensor/node_manager.py (REFACTORED)

class SensorNodeManager:
    """
    Manage sensor node with hardware awareness.
    """
    
    def __init__(self, config: Dict):
        self.config = config
        self.node_id = config['node_id']
        
        # Initialize node trust model (hardware-aware)
        self.node = SensorNodeTrust(
            node_id=self.node_id,
            hardware_code=config['hardware'],
            hardware_serial=config.get('hardware_serial', ''),
            location_gps=config.get('location'),
        )
        
        # Get hardware capabilities
        from backend.sensor.hardware.capabilities import get_hardware_capabilities
        self.node.hardware_capabilities = get_hardware_capabilities(self.node.hardware_code)
        
        # Initialize SDR interface (polymorphic)
        self.sdr = self._create_sdr_interface()
        
        # Initialize DSP (hardware-adaptive)
        self.current_mode = SURVEY_MODE
        self.dsp = HardwareAdaptiveDSP(self.node, self.current_mode)
        
        # Initialize calibrator
        self.calibrator = SensorCalibrator()
        
        # Initialize detection algorithm registry
        self.detector_registry = detector_registry
        
        # Initialize decoder registry
        self.decoder_registry = decoder_registry
    
    def _create_sdr_interface(self) -> SDRInterface:
        """Factory for creating SDR interface (hardware-specific)."""
        
        hardware = self.node.hardware_code.lower()
        
        if hardware == 'rtlsdr':
            from backend.sensor.hardware.rtl_sdr_impl import RTLSDRInterface
            return RTLSDRInterface(device_index=self.config.get('device_index', 0))
        
        elif hardware == 'hackrf':
            from backend.sensor.hardware.hackrf_impl import HackRFInterface
            return HackRFInterface(device_index=self.config.get('device_index', 0))
        
        elif hardware == 'limesdr':
            from backend.sensor.hardware.limesdr_impl import LimeSDRInterface
            return LimeSDRInterface(device_id=self.config.get('device_id'))
        
        # ... more hardware types ...
        
        else:
            raise ValueError(f"Unsupported hardware: {hardware}")
    
    async def run(self):
        """Main sensor node loop (hardware-aware)."""
        
        await self.sdr.open()
        
        try:
            # Perform initial calibration
            if self.config.get('auto_calibrate', True):
                await self.calibrator.full_calibration(self.node, self.sdr)
            
            # Launch background tasks
            asyncio.create_task(self._process_loop())
            asyncio.create_task(self._mode_adaptive_loop())
            asyncio.create_task(self.event_pub.flush_loop())
            asyncio.create_task(self.tasking_recv.poll_loop())
            
            while True:
                await asyncio.sleep(1)
        
        finally:
            await self.sdr.close()
    
    async def _mode_adaptive_loop(self):
        """Periodically adjust sensing mode based on CPU, power, tasking."""
        
        while True:
            # Check CPU/power available
            cpu_pct = self._get_cpu_available()
            power_pct = self._get_power_available()
            track_priority = await self._get_highest_priority_track()
            
            # Select new mode
            from backend.coordination.mode_selector import AdaptiveModeSelector
            selector = AdaptiveModeSelector()
            new_mode = await selector.recommend_mode(
                self.node,
                track_priority,
                cpu_pct,
                power_pct
            )
            
            # Update if mode changed
            if new_mode != self.current_mode:
                logger.info(f'{self.node_id}: Switching mode {self.current_mode.mode} '
                           f'→ {new_mode.mode}')
                self.current_mode = new_mode
                self.dsp = HardwareAdaptiveDSP(self.node, new_mode)
                
                # Reconfigure SDR
                await self._reconfigure_sdr_for_mode(new_mode)
            
            await asyncio.sleep(10)  # Check every 10 seconds

```

---

## SUMMARY: HARDWARE-AWARE ARCHITECTURE

### Key Principles

1. **Abstraction:** Hardware details hidden behind `SDRInterface`; implementations for RTL-SDR, HackRF, LimeSDR, PlutoSDR, Airspy

2. **Capability-Driven:** System knows what each device *can* do (frequency range, bandwidth, TDOA, timing accuracy)

3. **Adaptive Processing:** DSP, detection algorithms, decoders selected based on hardware + available resources

4. **Multi-Resolution:** System operates 3 layers: wideband survey, narrowband monitoring, deep analysis

5. **Hardware-Aware Fusion:** Track association and confidence account for hardware differences

6. **Calibration:** Automatic frequency + gain correction per node

7. **TDOA Coordination:** System aware of which nodes can contribute to TDOA

8. **Plugin Architecture:** Extensible detectors and decoders registered in runtime

### Real-World Benefits

- **Heterogeneous network:** Mix $30 RTL-SDRs with $600 LimeSDRs; system uses each appropriately
- **Graceful degradation:** Poor hardware is still useful (just with lower trust)
- **Resource efficiency:** Low-power nodes switch to survey mode when CPU-constrained
- **Scalability:** Add new SDR type → just implement `SDRInterface`
- **Operational realism:** Accounts for frequency stability, timing jitter, thermal throttling

**This architecture enables production RF intelligence networks at any scale.**

---

**End of Hardware-Aware Architecture**
