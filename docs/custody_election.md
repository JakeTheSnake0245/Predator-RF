# Predator RF — CustodyElector

Read this before touching `backend/coordination/custody_election.py`,
`core/src/predator/custody_election.h`,
`backend/tests/test_custody_election.py`,
`tests/custody_election_test.cpp`,
`tests/fixtures/custody_scenarios.json`, or
`scripts/test_custody_parity.py`.

---

## 1. Cache must be released on track archive

`CustodyElector` keeps a per-track decision cache (`_last_decisions`)
so it can compute handover overlap without callers having to thread
previous-decision state through. `TrackManager._age_tracks()` calls
`custody_elector.forget(track_id)` at the moment a track moves from
`self.tracks` to `self._archived` — without that hook the cache grows
without bound across long missions (one entry per emitter ever seen,
~bytes per entry × 100s/hour for a busy band).

The hook is wrapped in `try/except` because `forget()` is idempotent
and we never want a custody-cache bug to take down track archival.

**Diagnostic for regression:** `CustodyElector.stats()["tracks_in_cache"]`
should track `len(track_manager.tracks) + handover_overlap_window_count`,
NOT grow monotonically.

## 2. Hard-gate ordering inside `_hard_gate()` is load-bearing

GPS-sync gate runs **before** the stale-GPS gate, so a
`gps_synchronized=False` node short-circuits with
`tdoa_threat_requires_gps_sync` instead of falling through to a
misleading `gps_fix_stale_*s` reason.

## 3. Test wall-clock footgun

Tests in `backend/tests/test_custody_election.py` use a wall-clock
value of `2_000_000_000_000_000_000` ns (year ~2033) so subtracting
600 s from "now" stays positive — using `time.time_ns()` directly works
in production but `now=10_000_000_000` (10 s) goes negative when
subtracting 600 s and silently bypasses the `> 0` guard in the
stale-GPS check, producing a false-positive test pass.

## 4. Opt-in flag and AutoTasker fallback

The elector is opt-in via `config.custody_election_enabled` (default
True). When False, `DecisionEngine` falls back to the legacy
`_select_nodes_for_tasking()` heuristic and `AssessmentReport.custody`
is None — `AutoTasker` keeps working unchanged either way because
`recommended_nodes` is populated from `custody.tasked_nodes` when the
elector is on, and from the legacy heuristic when off.

---

## 5. C++ ↔ Python parity

The custody election logic exists in two places —
`backend/coordination/custody_election.py` (Python TOC backend) and
`core/src/predator/custody_election.h` (C++ header for Controller-mode
Predator-RF nodes that run without a Python backend). Both MUST produce
identical decisions for identical inputs because in mixed deployments
(Python TOC + Controller-mode Android) the operator's on-device
tasking would diverge from the TOC otherwise.

Drift is caught by `python scripts/test_custody_parity.py` which
compiles `tests/custody_election_test.cpp`, runs both electors against
`tests/fixtures/custody_scenarios.json`, and diffs outputs with
`FLOAT_TOL=1e-4`. Both sides round score components to 4 decimals
before emitting JSON, so a 1e-4 epsilon catches algorithmic drift
while ignoring last-bit float reordering.

### 5.1 Five footguns the parity test pins down

1. **Default `gps_updated_ns` in test helpers MUST be within
   `stale_gps_after_s` of `now_ns`.** The obvious "1e18 ns" sentinel
   is ~30 years stale relative to a `kTestNowNs=2e18` and silently
   hard-gates every node on every high-threat scenario, producing
   false-pass tests where both implementations agree on "no primary"
   for the wrong reason. The C++ helper sets
   `gps_updated_ns = kTestNowNs - 10s`.
2. **C++ emits `""` where Python emits `None`** for absent primary /
   handover_from. `_normalize()` in the parity script collapses both
   to `None` before comparison so this encoding difference doesn't
   mask real algorithmic mismatches.
3. **`SensorNodeTrust.compute_trust_score()` is monkey-patched in the
   harness** to return the fixture's `trust_score` verbatim — the C++
   port doesn't reimplement compute_trust_score (it expects the
   Controller to compute trust from peer history), so without the
   monkey-patch Python's score floats from 0.05..0.98 while C++'s is
   whatever the fixture says.
4. **`gps_age_component` returns 0.0 for `age_s >= stale_gps_after_s`
   BEFORE the weighted sum.** Both sides clamp identically; if either
   side ever switches to "negative component goes through, clamp at
   total" the parity test fails immediately because the negative
   component value would dominate the 0..1-bounded ones.
5. **`detecting_nodes` is a `List[str]` on `EmitterTrack`, NOT a
   `set`.** Appending the same node twice in fixture conversion would
   silently double-count `heard==True` membership in the future if the
   SNR component ever switches from set-membership to count-of-
   occurrences.

### 5.2 C++ wiring status

Controller-mode UI and tasking dispatch are not yet built (queued
behind roadmap items #6 RNS commanding wrapper and #7 Android TDOA
viewer), so today the header is consumed only by the unit tests.

When Controller-mode UI lands: instantiate `predator::custody::Elector`
once per Controller session, call `elect()` per peer-state-update tick
with `TrackInput` + `NodeInput` derived from `KujhadPeerSnapshot.state`
/ `.gps`, and route the `setOnChange` callback into the same Kujhad
event queue the spectrum overlays use.

### 5.3 Diagnostic for parity regression

`python scripts/test_custody_parity.py` fails with a per-step diff
naming the specific field that diverged, e.g.:

```
step[3].handover_until_ns: 1500000000017000000 != 1500000000016999999
```

`--keep-build` retains the compiled binary at
`/tmp/custody_parity_*/custody_election_test` for `lldb`/`gdb`
follow-up.
