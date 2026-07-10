# Predator RF — Signal Repository (Roadmap B)

## Overview

Three-tier signal storage:

| Tier | When | What |
|------|------|------|
| Metadata | Always | Frequency, power, modulation, node, timestamps, observation count |
| Fingerprint | When track reaches STABLE | 32-float cosine-similarity vector |
| IQ capture | Operator on-demand | Raw complex samples (cs8 / cs16) |

---

## Modules

### `backend/signal_repository/repository.py` — `SignalRepository`

SQLite-backed store, schema version `REPO_SCHEMA_VERSION=1`.

**Tables:**
- `signal_repository` — one row per unique signal (UUID `signal_id`, freq,
  power, modulation, node, timestamps, `observation_count`).
- `signal_fingerprints` — one row per fingerprint (32 floats packed as
  IEEE-754 little-endian binary blob).
- `iq_captures` — one row per IQ file (path, duration, sample-rate, centre
  frequency, captured-by, notes).
- `correlated_intercepts` — one row per confirmed multi-node intercept
  (signal_id, node_ids JSON array, centre frequency, rule_id).

**Key methods:**

```python
repo.save_signal(node_id, freq_hz, *, signal_id=None,
                 power_dbfs=None, modulation=None,
                 fingerprint_vec=None) → str        # returns signal_id

repo.save_fingerprint(signal_id, vector: List[float]) → None
repo.get_fingerprint(signal_id) → Optional[List[float]]

repo.search(freq_hz=None, freq_tol_hz=25_000, modulation=None,
            node_id=None, limit=100) → List[dict]

repo.find_similar(vector, threshold=0.80, limit=10) → List[dict]
    # returns [{signal_id, similarity, freq_hz, modulation, ...}, ...]

repo.record_iq_capture(signal_id, file_path, duration_s, sample_rate_hz,
                        center_hz, captured_by="operator",
                        notes="") → str              # returns capture_id

repo.save_correlated_intercept(signal_id, node_ids, center_hz,
                                rule_id="") → str    # returns intercept_id
```

### `backend/signal_repository/fingerprinter.py` — `SignalFingerprinter`

Generates 32-float fingerprint vectors from either raw FFT power bins or
signal metadata (power, bandwidth, centre frequency, SNR).

Features extracted (total 32 dimensions):
- 8 octave-band energy ratios (normalised)
- 4 spectral moments (mean, variance, skewness, kurtosis)
- 1 spectral flatness (Wiener entropy)
- 1 peak count (normalised)
- 1 occupied-bandwidth fraction
- 8 autocorrelation lags
- 4 metadata: normalised power, log bandwidth, normalised centre, SNR
- 5 padding (reserved for future features)

```python
fp = SignalFingerprinter()
vec = fp.fingerprint_from_bins(power_bins: List[float]) → List[float]
vec = fp.fingerprint_from_metadata(power_dbfs, bandwidth_hz,
                                   center_hz, snr_db) → List[float]
sim = fp.similarity(vec_a, vec_b) → float   # cosine similarity 0..1
ok  = fp.is_match(vec_a, vec_b,
                  threshold=0.80) → bool
```

---

## Wire-up in `PredatorBackend`

- `SignalRepository` is constructed in `__init__` when
  `config.persistence_enabled` is True, sharing the same SQLite path as
  `MissionStore`.
- Fingerprinting fires in `_check_fingerprint_match()`, called from
  `_on_rf_event` for STABLE tracks.
- On a fingerprint match above threshold, `CorrelationEngine.on_known_target_detected()`
  is awaited to trigger the geo-cue + intercept log pipeline.

---

## REST API (`/api/v1/repository/`)

Mounted by `backend/api/repository_routes.py`:

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/v1/repository/signals` | Search signals (query: `freq_hz`, `freq_tol_hz`, `modulation`, `node_id`, `limit`) |
| `GET` | `/api/v1/repository/signals/{id}/fingerprint` | Retrieve fingerprint vector |
| `POST` | `/api/v1/repository/signals/{id}/fingerprint` | Store fingerprint vector |
| `POST` | `/api/v1/repository/signals/{id}/iq-capture` | Trigger IQ capture (body: `duration_s`, `sample_rate_hz`, `node_id`) |
| `GET` | `/api/v1/repository/intercepts` | List correlated intercepts |
| `POST` | `/api/v1/repository/similar` | Find fingerprint matches (body: `vector`, `threshold`, `limit`) |
| `GET` | `/api/v1/repository/fleet/state` | Full fleet snapshot |
| `GET` | `/api/v1/repository/fleet/events` | Events since serial (query: `since`) |
| `GET` | `/api/v1/repository/rules` | List correlation rules |
| `POST` | `/api/v1/repository/rules` | Create rule |
| `DELETE` | `/api/v1/repository/rules/{id}` | Delete rule |

---

## Test suite

```
python -m unittest backend.tests.test_signal_repository -v
```

39 tests covering: fingerprint dimensions, cosine similarity, spectral
features, SQLite round-trip, find_similar, IQ capture records, correlated
intercepts, modulation search, correlation rule CRUD, fleet state
snapshots, event ring, track deduplication.
