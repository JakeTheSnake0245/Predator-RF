# Predator RF — StationarityGate

Read this before touching `backend/fusion/stationarity_gate.py`,
`PredatorBackend._try_tdoa_solve` in `backend/main.py`, or
`EmitterTrack.location_history` in `backend/tracking/track_manager.py`.

---

## 1. Stateless w.r.t. tracks — the caller owns history

`backend/fusion/stationarity_gate.py` was deliberately built without
any per-track cache (unlike `CustodyElector` which carries
`_last_decisions`). The gate's `evaluate()` takes the track's
`location_history` list inline and returns a verdict; the caller
(`PredatorBackend._try_tdoa_solve`) is responsible for appending to and
trimming `track.location_history` on accept.

**Reason:** `TrackManager` already owns track lifecycle and archival,
so a parallel gate-side cache would just duplicate that bookkeeping
AND create another `forget()`-on-archive contract like the elector
has.

### Trade-offs the caller must remember

- **(a) Trim history to `gate.history_max` after appending**, otherwise
  long-lived tracks balloon memory.
- **(b) Pass `prior_motion_state=track.motion_state`** so the
  classifier's hysteresis works (without it, borderline tracks flap
  between stationary/mobile each fix).

---

## 2. History field shape

The history field is a `List[tuple]` of
`(lat, lon, timestamp_ns, ellipse_a_m_or_None)` rather than
`List[HistoryPoint]` so `EmitterTrack.to_dict()` serialises naturally
without needing the dataclass to be JSON-aware.

`to_dict()` deliberately does NOT ship `location_history` to the wire
— operators see `motion_state` in the SSE payload, the trail is for
the gate's internal use; if a future UI wants to render breadcrumbs we
add an opt-in flag.

---

## 3. Velocity gate `dt_floor_s=2.0` is load-bearing

Without it, a small TDOA error spike at 0.5s dt alone implies hundreds
of m/s and rejects legitimate updates from a fast-moving target.
Zero-dt and negative-dt (out-of-order arrival from peer clock skew)
MUST also hit the dt-floor branch; tests pin both.

---

## 4. Mobile-track STABLE promotion (25 vs 10) is intentional

Mobile-track STABLE promotion at 25 observations vs 10 for
stationary/unknown is intentionally NOT a hard penalty — it just means
a mobile emitter takes longer to be considered "stably tracked"
because its position is by definition not converging. Setting it equal
to stationary would label every car-borne emitter STABLE after 10 hits
which misleads the operator about position confidence.

---

## 5. Invalid-candidate handling

Invalid candidates (NaN/inf coords, |lat|>90, |lon|>180,
timestamp<=0) are rejected by `evaluate()` BEFORE any history check
and never mutate `estimated_lat/lon`.

The dt-floor bypass branch classifies against `history + candidate`
(not just history) so motion_state doesn't lag a step at sub-floor
cadence.

---

## 6. Configuration

Configurable via `STATIONARITY_V_MAX_MPS` / `STATIONARITY_DT_FLOOR_S`
/ `STATIONARITY_HISTORY_MAX` env vars, surfaced as real `BackendConfig`
fields (not `getattr`) so a typo fails fast at startup.

---

## 7. Diagnostics

- `StationarityGate.stats()["fixes_rejected_velocity"]` should be > 0
  on a long mission with a noisy multi-node TDOA.
- `fixes_rejected_invalid` > 0 indicates the TDOA solver is producing
  garbage coords.
