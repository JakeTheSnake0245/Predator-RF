"""
SignalFingerprinter — compact spectral feature vectors for signal recognition.

Produces a 32-float fingerprint from FFT bins (or simulated from metadata when
raw bins are unavailable).  Stored in signal_fingerprints table.  Recognition
uses cosine similarity; threshold configurable via FINGERPRINT_MATCH_THRESHOLD
env var (default 0.82).

Design:
  - Stateless: instantiate once, call fingerprint() per signal.
  - No external deps: pure Python stdlib math.  numpy used when available for
    speed but the fallback path produces identical results.
  - Fingerprint vector layout (32 floats):
      [0..7]   normalised octave-band energy (8 bands, log-spaced)
      [8..11]  spectral moments: centroid, spread, skewness, kurtosis
      [12]     spectral flatness (Wiener entropy proxy)
      [13]     peak count (normalised 0..1, max=16)
      [14]     bandwidth fraction (signal bw / total bw)
      [15]     power percentile ratio (p90/p50)
      [16..23] autocorrelation lags 1..8 (first 8 normalised AC coeffs)
      [24..31] reserved zeros (future: cyclostationary features)
"""
from __future__ import annotations

import math
import os
from typing import List, Optional, Sequence

FINGERPRINT_DIM = 32
MATCH_THRESHOLD = float(os.getenv("FINGERPRINT_MATCH_THRESHOLD", "0.82"))


def _try_numpy():
    try:
        import numpy as np
        return np
    except ImportError:
        return None


_np = _try_numpy()


def _cosine_sim(a: List[float], b: List[float]) -> float:
    if len(a) != len(b):
        return 0.0
    if _np:
        import numpy as np
        va = np.array(a, dtype=np.float32)
        vb = np.array(b, dtype=np.float32)
        na = np.linalg.norm(va)
        nb = np.linalg.norm(vb)
        if na == 0 or nb == 0:
            return 0.0
        return float(np.dot(va, vb) / (na * nb))
    dot = sum(x * y for x, y in zip(a, b))
    na  = math.sqrt(sum(x * x for x in a))
    nb  = math.sqrt(sum(x * x for x in b))
    return dot / (na * nb) if na > 0 and nb > 0 else 0.0


def _octave_bands(bins: List[float], n_bands: int = 8) -> List[float]:
    n = len(bins)
    if n == 0:
        return [0.0] * n_bands
    bands = []
    for k in range(n_bands):
        lo = int(n * k / n_bands)
        hi = int(n * (k + 1) / n_bands)
        seg = bins[lo:hi] if hi > lo else [0.0]
        bands.append(sum(seg) / len(seg))
    total = sum(bands) + 1e-12
    return [b / total for b in bands]


def _spectral_moments(bins: List[float]) -> List[float]:
    n = len(bins)
    if n == 0:
        return [0.0, 0.0, 0.0, 0.0]
    total = sum(bins) + 1e-12
    freqs = [i / n for i in range(n)]
    centroid = sum(f * b for f, b in zip(freqs, bins)) / total
    spread   = math.sqrt(sum((f - centroid) ** 2 * b for f, b in zip(freqs, bins)) / total + 1e-12)
    if spread < 1e-9:
        return [centroid, spread, 0.0, 0.0]
    skew = sum(((f - centroid) / spread) ** 3 * b for f, b in zip(freqs, bins)) / total
    kurt = sum(((f - centroid) / spread) ** 4 * b for f, b in zip(freqs, bins)) / total - 3.0
    return [centroid, min(spread, 1.0), max(-3.0, min(skew, 3.0)), max(-3.0, min(kurt, 3.0))]


def _spectral_flatness(bins: List[float]) -> float:
    n = len(bins)
    if n == 0:
        return 0.0
    pos = [max(b, 1e-12) for b in bins]
    geo = math.exp(sum(math.log(b) for b in pos) / n)
    arith = sum(pos) / n
    return geo / arith


def _peak_count(bins: List[float], max_peaks: int = 16) -> float:
    n = len(bins)
    if n < 3:
        return 0.0
    peaks = sum(
        1 for i in range(1, n - 1)
        if bins[i] > bins[i - 1] and bins[i] > bins[i + 1]
    )
    return min(peaks, max_peaks) / max_peaks


def _bw_fraction(bins: List[float], threshold_db: float = 10.0) -> float:
    if not bins:
        return 0.0
    peak = max(bins)
    thresh = peak - threshold_db
    active = sum(1 for b in bins if b >= thresh)
    return active / len(bins)


def _power_ratio(bins: List[float]) -> float:
    if not bins:
        return 1.0
    s = sorted(bins)
    n = len(s)
    p50 = s[n // 2]
    p90 = s[int(n * 0.9)]
    return (p90 / p50) if p50 != 0 else 1.0


def _autocorr(bins: List[float], lags: int = 8) -> List[float]:
    n = len(bins)
    if n < 2:
        return [0.0] * lags
    mean = sum(bins) / n
    c0 = sum((b - mean) ** 2 for b in bins) / n + 1e-12
    result = []
    for lag in range(1, lags + 1):
        c = sum((bins[i] - mean) * (bins[i + lag] - mean)
                for i in range(n - lag)) / ((n - lag) * c0)
        result.append(max(-1.0, min(c, 1.0)))
    return result


class SignalFingerprinter:
    """Stateless fingerprint generator and matcher."""

    def fingerprint_from_bins(self, bins: List[float]) -> List[float]:
        """Generate a FINGERPRINT_DIM-float vector from FFT power bins (dBFS)."""
        vec = []
        vec += _octave_bands(bins)               # [0..7]
        vec += _spectral_moments(bins)           # [8..11]
        vec.append(_spectral_flatness(bins))     # [12]
        vec.append(_peak_count(bins))            # [13]
        vec.append(_bw_fraction(bins))           # [14]
        vec.append(min(_power_ratio(bins), 5.0) / 5.0)  # [15]
        vec += _autocorr(bins)                   # [16..23]
        vec += [0.0] * 8                         # [24..31] reserved
        return vec[:FINGERPRINT_DIM]

    def fingerprint_from_metadata(self,
                                  power_dbfs: float,
                                  bandwidth_hz: float,
                                  center_hz: float,
                                  snr_db: float) -> List[float]:
        """Coarse fingerprint when only metadata is available (no FFT bins)."""
        norm_bw  = min(bandwidth_hz / 20e6, 1.0)
        norm_pwr = (power_dbfs + 120.0) / 120.0
        norm_snr = min(snr_db / 60.0, 1.0)
        norm_cf  = (center_hz % 1e9) / 1e9

        vec = [norm_pwr] * 8
        vec[0] = norm_bw
        vec[1] = norm_snr
        vec[2] = norm_cf
        vec += [norm_pwr, norm_bw, norm_snr, norm_cf]
        vec.append(norm_bw)
        vec.append(min(norm_snr, 1.0))
        vec.append(norm_bw)
        vec.append(norm_pwr)
        vec += [0.0] * 8
        vec += [0.0] * 8
        return vec[:FINGERPRINT_DIM]

    def similarity(self, a: List[float], b: List[float]) -> float:
        return _cosine_sim(a, b)

    def is_match(self, a: List[float], b: List[float],
                 threshold: float = MATCH_THRESHOLD) -> bool:
        return self.similarity(a, b) >= threshold
