# Controller-mode TDOA pipeline (roadmap #7)

The Android Predator-RF app, when running as a Kujhad Fleet **Controller**,
can geolocate emitters from peer measurements without a Python backend.
Three header-only C++17 components carry the load:

| Header | Role |
| --- | --- |
| `core/src/predator/tdoa_coordinator.h` | Per-emitter measurement queue + LSQ solver. Direct port of `backend/fusion/tdoa_coordinator.py`. |
| `core/src/predator/stationarity_gate.h` | TDOA fix sanity filter + motion-state classifier. Port of `backend/fusion/stationarity_gate.py`. |
| `core/src/predator/fleet_tdoa_aggregator.h` | Glue: turns drained Kujhad peer events into per-emitter measurement batches and drives the coordinator. |

All three are pure stdlib so the test runner builds with a single
`g++ -std=c++17` invocation per file. No Eigen, no LAPACK, no JSON.

## Wire-up sketch (Controller mode)

```cpp
#include "predator/fleet_tdoa_aggregator.h"

predator::tdoa::AggregatorConfig cfg;  // defaults are operationally sane
predator::tdoa::FleetTDOAAggregator agg(cfg);

agg.setOnFix([](const predator::tdoa::Result& r) {
    // Render to map, push to MapLibre WebView, log, or fan out via
    // CoT — same downstream as Python's TDOAResult.
});

// In the Kujhad controller event loop, after draining each peer:
predator::tdoa::PeerObservation o;
o.node_id        = peer.identityHash16();
o.timestamp_ns   = parseIsoToNs(event["time"]);
o.frequency_hz   = event["frequency"].get<double>();
o.node_lat       = peer.gps.lat;
o.node_lon       = peer.gps.lon;
o.timing_trust   = predator::tdoa::computeTimingTrust(
                       peer.canDoTDOA(), peer.timingStability());
o.gps_updated_ns = peer.gps.updatedNs;
agg.ingest(o, nowNs());

// Once per controller tick (~1 Hz):
agg.tick(nowNs());
```

`tick()` returns the fixes emitted this cycle and also delivers them
through the `OnFixCb`. The aggregator handles TTL pruning, distinct-node
gating, and per-emitter solve cooldown — callers do not need to track
when to call `solve()`.

## Solver details (parity contract with Python)

The C++ port mirrors the Python iterative LSQ:

1. Per-iteration linearization of `range_diffs[i] = (ri - r0)` around
   the current estimate using ENU coordinates anchored at the
   reference (first) measurement.
2. 50 Gauss-Newton iterations capped per call.
3. Speed of light constant `299_792_458.0`, Earth radius `6_371_000` m.
4. 2-node fallback: midpoint of distinct nodes, `conf=0.3` before the
   `mean(timing_trust)` scaling.
5. Ellipse approximation: base radius `50 + (1-conf) * 4950` m, axis
   ratio from 2x2 node-position covariance, theta rotated `+90 mod 180`
   to align across the cluster baseline (TDOA error is across the
   baseline, not along it).

**Drift from Python.** The C++ solver adds a Tikhonov regularizer
(`lambda = 1e-3 * trace(AT*A)`) and a 50 km step cap inside the LSQ
loop. Python's `numpy.linalg.lstsq` uses SVD-based pseudo-inverse and
is intrinsically more stable on rank-deficient or ill-conditioned
geometries; the regularizer is the cheapest stdlib equivalent.
Practical effect: identical fixes for well-conditioned scenarios
(distinct-node count >= 3, time offsets within `baseline / c`);
slightly biased fixes for pathological geometries that Python would
also struggle with. Byte-equal parity is therefore **not** achievable
without porting either side to the other's solver — the parity stance
here is "operationally equivalent" rather than "bit-identical" (unlike
`custody_election.h` which IS byte-equal because there's no float LSQ).

## Aggregator semantics

- **Emitter key:** frequency rounded to the nearest `freq_quantum_hz`
  (default 1 kHz). Two peers reporting 433.920 MHz and 433.920 4 MHz
  bucket together; 433.920 vs 433.940 do not.
- **Measurement TTL:** 5 s default. Older measurements are pruned at
  the start of each `tick()` so stale hearings don't bias new solves.
  TDOA assumes all measurements correlate to the SAME transmission;
  honouring TTL is correctness, not optimization.
- **Solve trigger:** `distinctNodes(emitter) >= solve_min_distinct`
  (default 2) AND `now - last_solve_ns(emitter) >= cooldown_ns`
  (default 2 s). Cooldown prevents thrashing under bursty event loads.
- **GPS freshness:** `gps_max_age_s` default 60 s. Observations with
  stale GPS are rejected at ingest and counted in `droppedStaleGps()`.
  An observation with `gps_updated_ns == 0` bypasses the gate
  (matches the Python "didn't supply timestamp = opted out" behaviour).

## Display path (defer to Android side)

The MapLibre WebView already accepts marker payloads from the C++
side via the existing JS bridge — feed each `Result` through the same
channel as the existing peer markers, with these fields:

| JSON field | Source |
| --- | --- |
| `lat`, `lon` | `Result.estimated_lat / .estimated_lon` |
| `confidence` | `Result.location_confidence` |
| `ellipse_a_m`, `ellipse_b_m`, `ellipse_theta_deg` | direct from `Result` |
| `participating_nodes` | `Result.participating_nodes` |
| `emitter_key` | recompute via `FleetTDOAAggregator::emitterKey(freq, q)` |

Visual verification requires the actual Android build; not testable
here in the Replit landing-page environment.

## Test surface

- `tests/tdoa_coordinator_test.cpp` — 51 assertions across 12 cases.
- `tests/stationarity_gate_test.cpp` — 26 assertions across 12 cases.
- `tests/fleet_tdoa_aggregator_test.cpp` — 29 assertions across 11 cases.

Build & run:
```sh
g++ -std=c++17 -O2 -Icore/src tests/tdoa_coordinator_test.cpp -o /tmp/tdoa_test && /tmp/tdoa_test
g++ -std=c++17 -O2 -Icore/src tests/stationarity_gate_test.cpp -o /tmp/sg_test && /tmp/sg_test
g++ -std=c++17 -O2 -Icore/src tests/fleet_tdoa_aggregator_test.cpp -o /tmp/agg_test && /tmp/agg_test
```

## Footguns

1. **Time-offset ceiling.** TDOA hyperbolas only intersect when
   `|range_diff| < baseline`. Caller-supplied timestamps with offsets
   exceeding `baseline / c` (~33 µs per 10 km of baseline) drive the
   solver into a near-singular Jacobian. The regularizer keeps the
   solve from diverging but the resulting fix is biased; the
   stationarity gate's velocity check is the second line of defence
   against TDOA flips that escape the solver.

2. **timing_trust source authority.** `computeTimingTrust` returns
   different ranges for `can_do_tdoa=true` (`[0.5, 1.0]`) vs the
   system-clock branch (`[0.2, 0.5]`). Calling it with
   `hw_timing_stability` already pre-multiplied by 0.5 will floor
   every cheap-SDR observation at 0.2 — the helper does the halving
   itself.

3. **GPS bypass on zero.** `gpsFresh(0, ...)` returns true. This is
   intentional (matches Python's "test fakes without the field
   opt out") but it means production callers MUST populate
   `gps_updated_ns` or every observation skips the freshness check.

4. **Aggregator key index grows.** `known_keys_` is a one-way set —
   keys are added on ingest but only cleared on `clear()`. For
   long-running missions on a wide band this can grow without bound.
   Acceptable for a single-mission Controller session; if the app
   ever runs the controller indefinitely, add an LRU cap on
   `known_keys_` and track `last_seen_ns` per key for eviction.

5. **Stationarity classifier needs ellipse axes.** `classifyMotion`
   returns `Unknown` when every history entry has zero ellipse axes.
   That's what falls out of a 2-node fallback chain that didn't
   populate the ellipse — make sure the aggregator's `Result` is
   the one fed into history, not a stripped-down marker payload.
