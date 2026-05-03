"""
Hardware → decoder/detector capability inference.

The C++ Kujhad /v1/state surface does NOT publish a per-node decoder list (the
header comment at kujhad_fleet.h L16 is stale — actual /v1/state exposes
mission/scan-control state, not bridge configuration). For Path 1 we infer
"what could this node decode" by intersecting:

  * the SDR hardware's freq range (from /v1/identify hwProfile.hardware,
    looked up in HARDWARE_REGISTRY), and
  * each registered decoder's declared frequency bands (pulled live from
    decoder_registry — the registry is the source of truth for which
    decoders exist and what bands they want).

Output is advisory — the C++ side is the source of truth for what is actually
running. The Python orchestrator uses these lists to:
  * pick which node to task with a given protocol when a frequency hit lands,
  * size confidence weighting (only nodes that *could* decode contribute to
    the protocol-classification confidence of an emitter track).

Decoder name strings here always match decoder_registry.list_decoders(); a
decoder that exists in this module but not the registry is a bug. (Architect-
caught: prior version listed 'adsb' which the registry doesn't expose.)
"""
from __future__ import annotations

from typing import Dict, List, Tuple, TYPE_CHECKING

if TYPE_CHECKING:
    from backend.models.sensor_node import SensorNodeTrust


# Coarse band ranges — match DecoderRegistry._freq_to_band so a decoder
# declaring frequency_bands=['vhf','uhf'] resolves identically here.
_BAND_RANGES_HZ: Dict[str, Tuple[float, float]] = {
    'hf':  (3e6,    30e6),
    'vhf': (30e6,   300e6),
    'uhf': (300e6,  3e9),
    'shf': (3e9,    30e9),
}

# Detectors are independent of frequency (they run on whatever the SDR is
# tuned to) but vary by parallelism budget. fft_peak/energy both fit in 1 slot.
_DETECTOR_MIN_PARALLELISM: Dict[str, int] = {
    'energy':   1,
    'fft_peak': 1,
}

# Cached registry → bands map. Populated lazily on first inference call so
# we don't pay registry import cost (or risk circular imports) at module load.
_decoder_bands_cache: Dict[str, List[Tuple[float, float]]] | None = None


def _decoder_bands() -> Dict[str, List[Tuple[float, float]]]:
    """Build {decoder_name: [(lo,hi), ...]} from decoder_registry. Cached."""
    global _decoder_bands_cache
    if _decoder_bands_cache is not None:
        return _decoder_bands_cache
    out: Dict[str, List[Tuple[float, float]]] = {}
    try:
        from backend.sensor.decoders.decoder_registry import decoder_registry
    except Exception:
        _decoder_bands_cache = out
        return out
    for name in decoder_registry.list_decoders():
        dec = decoder_registry.get_decoder(name)
        if dec is None:
            continue
        bands = []
        try:
            cap = dec.get_capability() or {}
            for band_name in cap.get('frequency_bands', []) or []:
                rng = _BAND_RANGES_HZ.get(str(band_name).lower())
                if rng is not None:
                    bands.append(rng)
        except Exception:
            continue
        if bands:
            out[name] = bands
    _decoder_bands_cache = out
    return out


def _reset_cache_for_tests() -> None:
    """Invalidate the cached decoder→bands map. Tests use this when they
    register a custom decoder and need the next inference call to see it."""
    global _decoder_bands_cache
    _decoder_bands_cache = None


def _overlap(a: Tuple[float, float], b: Tuple[float, float]) -> bool:
    """True iff two (lo,hi) freq ranges intersect at all."""
    return a[0] < b[1] and b[0] < a[1]


def infer_decoder_capabilities(node: "SensorNodeTrust") -> List[str]:
    """Return decoder names this node's hardware can host (any band overlap).

    Empty when the hardware is unknown or the registered SDRCapabilities
    have no frequency range — caller should not infer "no decoders" from
    that, only "unknown"."""
    caps = getattr(node, 'hardware_capabilities', None)
    hw_range = getattr(caps, 'freq_range_hz', None) if caps else None
    if not hw_range:
        return []
    return [name for name, bands in _decoder_bands().items()
            if any(_overlap(hw_range, b) for b in bands)]


def infer_detector_capabilities(node: "SensorNodeTrust") -> List[str]:
    """Return detector names this node can run within its parallel budget."""
    caps = getattr(node, 'hardware_capabilities', None)
    parallel = getattr(caps, 'max_parallel_detectors', 1) if caps else 1
    return [name for name, need in _DETECTOR_MIN_PARALLELISM.items()
            if parallel >= need]


def covers_frequency(node: "SensorNodeTrust", freq_hz: float) -> bool:
    """Cheap freq-in-range check used by the fleet manager when picking
    which node to task with a tune command."""
    caps = getattr(node, 'hardware_capabilities', None)
    if not caps or not getattr(caps, 'freq_range_hz', None):
        return False
    lo, hi = caps.freq_range_hz
    return lo <= freq_hz <= hi


def search_bands_to_tuples(raw) -> List[Tuple[float, float]]:
    """Normalise the C++ /v1/state.searchBands array (each element a JSON
    object with startHz/endHz keys, or sometimes a 2-element list) to a
    flat list of (lo, hi) tuples. Garbage entries are silently dropped so
    a single malformed row from the device doesn't poison the whole list."""
    out: List[Tuple[float, float]] = []
    if not isinstance(raw, list):
        return out
    for entry in raw:
        if isinstance(entry, dict):
            try:
                lo = float(entry.get('startHz', entry.get('start', 0)))
                hi = float(entry.get('endHz',   entry.get('end',   0)))
            except (TypeError, ValueError):
                continue
        elif isinstance(entry, (list, tuple)) and len(entry) >= 2:
            try:
                lo = float(entry[0]); hi = float(entry[1])
            except (TypeError, ValueError):
                continue
        else:
            continue
        if hi > lo > 0:
            out.append((lo, hi))
    return out
