# Predator RF

Predator RF is a joint sensing platform for a solo SIGINT operator using
Raspberry Pi/SDR/GPS sensors for RX-only signal logging and mapping.

## Run & Operate

_Populate as you build_

## Stack

**C++ Application:**
- **Build System:** CMake
- **UI Framework:** Dear ImGui
- **Graphics:** OpenGL / GLES 3
- **DSP:** FFTW3, Volk
- **Android Wrapper:** Kotlin/JNI

**Python Backend:**
- **Framework:** FastAPI, asyncio

## Where things live

- `server.py`: Python HTTP server for the informational landing page (`index.html`) and an operator UI mockup (`preview.html`).
- `core/src/predator/kujhad_fleet.h`: Header-only Kujhad Fleet hub-and-spoke peer protocol (HTTP/TLS server).
- `core/src/predator/decoder_ingest.h`: Header-only receive-only decoder ingestion base class.
- `decoder_modules/rtl433_decoder/`: Native rtl_433 ISM decoder module.
- `core/src/gui/style.cpp`: Contains `applyTouchFriendlyTweaks()` for Android UI adjustments. Base font glyph range adds Misc-Symbols (U+2600..U+26FF) for the gear icon (U+2699) used by the Hits page per-marker action sheet.
- `docs/android_build.md`: End-to-end APK build guide.
- `docs/android_gotchas.md`: All deeply Android-specific gotchas (manifest IME mode, soft-keyboard EditText capture, popup sizing, warm-restart SIGABRT, DSD-FME freeze fixes, USB receiver leak, NDK `long long` vs `int64_t`). Read this before touching `MainActivity.kt`, `backend.cpp`, `AndroidManifest.xml`, or any native decoder module.
- `android/sdr-kit/arm64-v8a/`: Prebuilt native SDR libraries for Android.
- `scripts/fetch-sdr-kit.sh`: Script to refresh `android/sdr-kit/`.
- `CMakeLists.txt`: CMake build configuration for the C++ application.
- `backend/`: Python intelligence backend.
- `backend/coordination/custody_election.py`: N-best scored sensor election with hard gates, soft scoring, handover overlap, stand-down list. Wired into `DecisionEngine.assess()` and `TrackManager._age_tracks()` via `main.py`.
- `core/src/predator/custody_election.h`: header-only C++ port of the same elector for Controller-mode Predator-RF nodes (no Python backend needed). Pure stdlib — no JSON or HTTP deps — so the test runner builds with a single g++ invocation.
- `core/src/predator/hold_manager.h`: header-only multi-VFO hold list (roadmap #4). Persists across restart via `core::configManager.conf["predatorHeldFrequencies"]`; per-frame tick reconciles in-band geometry and creates/destroys `Predator H<id>` VFOs via caller-injected lambdas (so the logic stays sigpath/ImGui-free and unit-testable). Wire-up lives in `main_window.cpp` immediately after the marker re-anchor loop; UI panel "Held Frequencies" + "+ Hold" button on hit rows on the Hits tab.
- `tests/hold_manager_test.cpp`: 12 test cases / 127 assertions covering add/remove, in-band boundary, lifecycle across source retunes, JSON round-trip, decoder-kind enum stability, disabled-entry semantics, create-failure retry, GC-on-remove, and null-callback safety. Build: `g++ -std=c++17 -O2 -Icore/src tests/hold_manager_test.cpp -o /tmp/hmt && /tmp/hmt`.
- `tests/custody_election_test.cpp`, `tests/fixtures/custody_scenarios.json`, `scripts/test_custody_parity.py`: standalone C++ unit tests + shared JSON fixture + parity harness that asserts the C++ and Python electors produce byte-identical decisions for the same scenarios.
- `backend/fusion/stationarity_gate.py`: TDOA fix sanity filter (rejects physically-impossible velocity jumps, NaN/inf/out-of-range coords, zero/negative timestamps) + motion-state classifier (RMS-spread vs ellipse → stationary/mobile/unknown with hysteresis). Stateless — caller owns the per-track history list. Wired into `PredatorBackend._try_tdoa_solve` and `EmitterTrack._advance_state` (mobile tracks need 25 obs to promote to STABLE vs 10 for stationary/unknown).
- `deploy/`: Deployment scripts and configurations for the Python backend.
- `docs/`: Project documentation, including API contracts and integration guides.

## Architecture decisions

- **Two-tier system:** Python backend consumes the Kujhad Fleet HTTP API from C++ Predator-RF nodes.
- **RX-only focus:** The C++ build hard-rejects `tx.*` commands at the wire layer for security and simplicity.
- **Multi-transport for CoT:** CoT XML fans out over both IP (TAK UDP/TCP) and RNS (`predatorrf/cot.v1`) simultaneously.
- **Manual CoT export:** CoT export is operator-initiated only in v1, even if the DecisionEngine advises escalation.
- **Android UI scaling:** Dynamic UI scaling with touch-friendly tweaks applied based on device detection.

## Product

- **Signal Intelligence:** DSP, decoders (RTL433, P25, ADS-B), and hit/event management.
- **Mission System:** Manual, Classify, Scan, and QuickScan modes with search bands, targets/excludes, and peak detection.
- **Kujhad Fleet Console:** Hub-and-spoke peer protocol for linking multiple Predator RF instances, with Controller and Device roles. Includes spectrum mirroring.
- **Native Decoders:** Integrated native modules for RTL433 and DSD-FME (P25).
- **Mapping:** MapLibre GL JS with 2D/3D views, layer toggles, and compass.
- **Intelligence Backend (Python):** Consumes Kujhad API, manages tracks, detects anomalies, handles operator missions, approvals, and overrides.
- **CoT Export:** Exports data in Cursor-on-Target (CoT) format for external systems like ATAK.

## User preferences

- _Populate as you build_

## Gotchas

### Cross-cutting
- The Replit environment only serves an informational landing page and interactive UI mockup (`server.py`). It does **not** run the Android build or the Python backend.
- `X-Kujhad-Key` header is required for authentication on all `/v1/*` Kujhad API calls.
- The `predatorrf/cot.v1` RNS Destination is additive, not a replacement for TCP/TLS Kujhad control-plane transport.
- For Android builds, `assembleDebug` is the documented happy path as `release` is unsigned by design.

### Android
All deeply Android-specific gotchas live in `docs/android_gotchas.md`. The
short list of what's covered there:
- `AndroidManifest.xml` MUST set `windowSoftInputMode="adjustNothing"` (NOT `adjustResize`).
- Soft-keyboard input capture via 4×4 alpha=0.01 `EditText`, focus race vs `NativeContentView`, backspace de-dup, IME show/hide debouncing.
- `BeginPopupModal` sizing rules — full safe-area height, top header bar for actions, no `getImeBottomInset()` subtraction; `iv()` lambda for active-field scroll.
- CoT enable bridge between C++ `config.json` and Python env (`bridgeCppConfigToEnv()` must run before `Python.start()`).
- Warm-restart SIGABRT in `ImGui_ImplOpenGL3_Init` and the defensive teardown in `backend::init()`.
- DSD-FME decoder freeze (4 root causes) and the `flog::warn` / NDK `long long` vs `int64_t` gotcha.
- `usbReceiver` leak in `MainActivity.onDestroy`.

### C++ Predator UI
- **Multi-VFO Hold ≠ scan-side hold ≠ marker assignment.** Three nearby concepts in `main_window.cpp` that must not be conflated: (a) `predatorHoldOnNewHit` is *scan-side hold* — pauses the scan loop when a new hit arrives so the operator can inspect; nothing to do with VFOs. (b) "Marker assignment" creates a `Predator M<n>` VFO tied to a `hits[]` row and dies when the hit ages out; bandwidth-limited to the current SDR window. (c) `predatorHoldManager` (predator/hold_manager.h) is the *persistent* multi-VFO hold list — entries survive hit pruning AND app restart, each carries its own decoder kind, and out-of-band entries are torn down cleanly rather than left clipped at the spectrum edge. The "+ Hold" button on hit rows pushes a hit's frequency into (c). When the source retunes such that a held frequency falls outside `[center − sr/2 + bw/2, center + sr/2 − bw/2]`, the next tick destroys the VFO; when it comes back in-band, the next tick re-creates it. This is NOT a round-robin source-retune scheduler — held entries that span more than the SDR's instantaneous bandwidth are simply marked "out-of-band" and remain dormant. A future "Multi-band scheduler" feature would wrap HoldManager with a dwell-weighted retune loop. Diagnostic for regression: `predatorHoldManager.runtimeFor(id).vfo_active` should match `vfoExists("Predator H" + id)` after every tick; if they diverge, a callback returned false silently or an exception escaped a lambda. The fifth `tick()` parameter `existsCb` is load-bearing — without it, an external teardown of the VFO (decoder module reload, manual `sigpath::vfoManager.deleteVFO` from another path) would leave `rt.vfo_active=true` forever and the entry would never re-create. The wire-up always passes `existsCb`; tests pin both the with-existsCb (recovers) and without-existsCb (documented stuck state) branches. Per-entry `decoder` is *persisted and surfaced in the UI* but does NOT yet drive actual decoder-module activation — every held VFO currently runs through whatever decoder is bound to the matching `Predator H<id>` channel via the existing module system. Wiring decoder_kind into `native_decoder_registry` so e.g. an `id=h3, decoder=native_rtl433` entry auto-spins an Rtl433Ingester is roadmap item #5.

### Python backend
- **CustodyElector cache must be released on track archive:** `CustodyElector` keeps a per-track decision cache (`_last_decisions`) so it can compute handover overlap without callers having to thread previous-decision state through. `TrackManager._age_tracks()` calls `custody_elector.forget(track_id)` at the moment a track moves from `self.tracks` to `self._archived` — without that hook the cache grows without bound across long missions (one entry per emitter ever seen, ~bytes per entry × 100s/hour for a busy band). The hook is wrapped in `try/except` because `forget()` is idempotent and we never want a custody-cache bug to take down track archival. Diagnostic for regression: `CustodyElector.stats()["tracks_in_cache"]` should track `len(track_manager.tracks) + handover_overlap_window_count`, NOT grow monotonically. Hard-gate ordering inside `_hard_gate()` is also load-bearing: GPS-sync gate runs BEFORE the stale-GPS gate, so a `gps_synchronized=False` node short-circuits with `tdoa_threat_requires_gps_sync` instead of falling through to a misleading `gps_fix_stale_*s` reason. Tests in `backend/tests/test_custody_election.py` use a wall-clock value of `2_000_000_000_000_000_000` ns (year ~2033) so subtracting 600 s from "now" stays positive — using `time.time_ns()` directly works in production but `now=10_000_000_000` (10 s) goes negative when subtracting 600 s and silently bypasses the `> 0` guard in the stale-GPS check, producing a false-positive test pass. The elector is opt-in via `config.custody_election_enabled` (default True); when False, `DecisionEngine` falls back to the legacy `_select_nodes_for_tasking()` heuristic and `AssessmentReport.custody` is None — `AutoTasker` keeps working unchanged either way because `recommended_nodes` is populated from `custody.tasked_nodes` when the elector is on, and from the legacy heuristic when off.
- **CustodyElector C++ ↔ Python parity:** The custody election logic exists in two places — `backend/coordination/custody_election.py` (Python TOC backend) and `core/src/predator/custody_election.h` (C++ header for Controller-mode Predator-RF nodes that run without a Python backend). Both MUST produce identical decisions for identical inputs because in mixed deployments (Python TOC + Controller-mode Android) the operator's on-device tasking would diverge from the TOC otherwise. Drift is caught by `python scripts/test_custody_parity.py` which compiles `tests/custody_election_test.cpp`, runs both electors against `tests/fixtures/custody_scenarios.json`, and diffs outputs with `FLOAT_TOL=1e-4` (both sides round score components to 4 decimals before emitting JSON, so a 1e-4 epsilon catches algorithmic drift while ignoring last-bit float reordering). Five footguns the parity test pins down: (1) **default `gps_updated_ns` in test helpers MUST be within `stale_gps_after_s` of `now_ns`** — the obvious "1e18 ns" sentinel is ~30 years stale relative to a `kTestNowNs=2e18` and silently hard-gates every node on every high-threat scenario, producing false-pass tests where both implementations agree on "no primary" for the wrong reason. The C++ helper sets `gps_updated_ns = kTestNowNs - 10s`. (2) **C++ emits `""` where Python emits `None`** for absent primary / handover_from — `_normalize()` in the parity script collapses both to `None` before comparison so this encoding difference doesn't mask real algorithmic mismatches. (3) **`SensorNodeTrust.compute_trust_score()` is monkey-patched in the harness** to return the fixture's `trust_score` verbatim — the C++ port doesn't reimplement compute_trust_score (it expects the Controller to compute trust from peer history), so without the monkey-patch Python's score floats from 0.05..0.98 while C++'s is whatever the fixture says. (4) **`gps_age_component` returns 0.0 for `age_s >= stale_gps_after_s` BEFORE the weighted sum** — both sides clamp identically; if either side ever switches to "negative component goes through, clamp at total" the parity test fails immediately because the negative component value would dominate the 0..1-bounded ones. (5) **`detecting_nodes` is a `List[str]` on `EmitterTrack`, NOT a `set`** — appending the same node twice in fixture conversion would silently double-count `heard==True` membership in the future if the SNR component ever switches from set-membership to count-of-occurrences. Wiring on the C++ side: Controller-mode UI and tasking dispatch are not yet built (queued behind roadmap items #6 RNS commanding wrapper and #7 Android TDOA viewer), so today the header is consumed only by the unit tests. When Controller-mode UI lands, instantiate `predator::custody::Elector` once per Controller session, call `elect()` per peer-state-update tick with `TrackInput` + `NodeInput` derived from `KujhadPeerSnapshot.state` / `.gps`, and route the `setOnChange` callback into the same Kujhad event queue the spectrum overlays use. Diagnostic for parity regression: `python scripts/test_custody_parity.py` fails with a per-step diff naming the specific field that diverged (e.g. `step[3].handover_until_ns: 1500000000017000000 != 1500000000016999999`); `--keep-build` retains the compiled binary at `/tmp/custody_parity_*/custody_election_test` for `lldb`/`gdb` follow-up.
- **StationarityGate is stateless w.r.t. tracks — the caller owns history.** `backend/fusion/stationarity_gate.py` was deliberately built without any per-track cache (unlike `CustodyElector` which carries `_last_decisions`). The gate's `evaluate()` takes the track's `location_history` list inline and returns a verdict; the caller (`PredatorBackend._try_tdoa_solve`) is responsible for appending to and trimming `track.location_history` on accept. Reason: `TrackManager` already owns track lifecycle and archival, so a parallel gate-side cache would just duplicate that bookkeeping AND create another `forget()`-on-archive contract like the elector has. The trade-off: the caller must remember to (a) trim history to `gate.history_max` after appending, otherwise long-lived tracks balloon memory; (b) pass `prior_motion_state=track.motion_state` so the classifier's hysteresis works (without it, borderline tracks flap between stationary/mobile each fix). The history field is a `List[tuple]` of `(lat, lon, timestamp_ns, ellipse_a_m_or_None)` rather than `List[HistoryPoint]` so `EmitterTrack.to_dict()` serialises naturally without needing the dataclass to be JSON-aware. `to_dict()` deliberately does NOT ship `location_history` to the wire — operators see `motion_state` in the SSE payload, the trail is for the gate's internal use; if a future UI wants to render breadcrumbs we add an opt-in flag. Velocity-gate `dt_floor_s=2.0` is load-bearing — without it, a small TDOA error spike at 0.5s dt alone implies hundreds of m/s and rejects legitimate updates from a fast-moving target. Zero-dt and negative-dt (out-of-order arrival from peer clock skew) MUST also hit the dt-floor branch; tests pin both. Mobile-track STABLE promotion at 25 observations vs 10 for stationary/unknown is intentionally NOT a hard penalty — it just means a mobile emitter takes longer to be considered "stably tracked" because its position is by definition not converging; setting it equal to stationary would label every car-borne emitter STABLE after 10 hits which misleads the operator about position confidence. Invalid candidates (NaN/inf coords, |lat|>90, |lon|>180, timestamp<=0) are rejected by `evaluate()` BEFORE any history check and never mutate `estimated_lat/lon`; the dt-floor bypass branch classifies against `history + candidate` (not just history) so motion_state doesn't lag a step at sub-floor cadence. Configurable via `STATIONARITY_V_MAX_MPS` / `STATIONARITY_DT_FLOOR_S` / `STATIONARITY_HISTORY_MAX` env vars, surfaced as real `BackendConfig` fields (not `getattr`) so a typo fails fast at startup. Diagnostic for regression: `StationarityGate.stats()["fixes_rejected_velocity"]` should be > 0 on a long mission with a noisy multi-node TDOA; `fixes_rejected_invalid` > 0 indicates the TDOA solver is producing garbage coords.

## Pointers

- [SDR++ GitHub](https://github.com/AlexandreRouma/SDRPlusPlus)
- `backend/rns/README.md`
- `docs/rns_parity.md`
- `docs/rns_field_log.md`
- `docs/1_conops.md`
- `docs/android_build.md`
- `docs/android_gotchas.md`
- `docs/OPERATOR_RUNBOOK.md`
- `docs/MISSION_READY_CHECKLIST.md`
- `docs/ATAK_COT_FORMAT.md`
- `docs/ANDROID_INTEGRATION.md`
- `docs/SIDELOAD_README.md`
