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
- `backend/rns/cmd.py`, `backend/rns/cmd_handler.py`, `backend/coordination/kujhad_rns_client.py`: roadmap #6 RNS commanding wrapper. CBOR envelope on `predatorrf/cmd.v1` aspect with two-sided `tx.*` reject + class allowlist; `RNSCmdBridge` mirrors `RNSCotBridge`; `KujhadRNSClient.send_{tune,scan,mission}_command(peer_h16, …)` shape mirrors `KujhadClient`. Daemon wires it on `config["cmd_v1_enabled"]=True`. See `docs/rns_commanding.md`.
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
- **Multi-VFO Hold + Hold decoder auto-activation (#5).** Two intertwined gotchas — `predatorHoldOnNewHit` (scan-side hold) vs `Predator M<n>` (per-hit marker VFO) vs `predatorHoldManager` (persistent multi-VFO hold list); plus the two-phase pre/post tick contract that lets `HoldDecoderBinder` auto-spawn `rtl433_decoder` instances against held VFOs without racing the dsp stream destructor. Sacred call order, in-band math sharing, cross-plugin binding registry, RTL433-only scope (#5.5/#5.6 deferred), and three architect-flagged hardenings (effective-bw consistency, external-delete recovery, drop-before-existsCb ordering) all live in `docs/predator_hold.md`. Read that before touching `core/src/predator/hold_*.{h,cpp}`, `decoder_modules/rtl433_decoder/src/main.cpp`, or the hold wire-up block in `core/src/gui/main_window.cpp`.

<!-- legacy block trimmed; expanded version in docs/predator_hold.md
- **Hold decoder auto-activation (roadmap #5) — only RTL433 in this cut.** `core/src/predator/hold_decoder_binder.h` spawns a `core::moduleManager` instance per held entry whose `decoder` maps to a known SDRPP module (currently only `Native_RTL433 → "rtl433_decoder"`; `Native_DSDFME_P25` and the `Radio_*` family are explicitly deferred to roadmap #5.5 / #5.6 — `decoderModuleName()` returns `""` for them and the binder simply skips). Three load-bearing pieces: (a) the binder is **two-phase** — `preTick` runs BEFORE `HoldManager.tick` to tear down any decoder whose VFO is about to disappear (entry removed/disabled, decoder kind changed, or out-of-band after this tick) so the bound `dsp::sink::Handler` is detached BEFORE its source stream is freed; `postTick` runs AFTER to spawn instances for entries whose VFOs now exist. Single-phase tick would race the stream destructor and segfault. (b) The Binder needs the same in-band math `HoldManager.tick` is about to apply — `HoldManager::inBand` is therefore promoted to `public static` and the binder calls it directly on the same `(sourceCenter, sampleRate)` pair the manager will use that frame. If `HoldManager` ever changes the in-band rule (e.g. adds a hysteresis margin), the binder needs the same change or it'll predict differently than the manager acts. (c) Decoder modules CANNOT see `HoldManager` / `Binder` symbols (each is a separate `.so` plugin with its own private `ConfigManager`) — so the binding is passed through `core/src/predator/hold_binding_registry.h` (process-wide map of `instance_name → bound_vfo_name`, mirrors the `native_decoder_registry` pattern). Wire-up call order is sacred: **`setBoundVfoFor → moduleManager.createInstance` (the module's ctor reads the binding)**, then on teardown **`moduleManager.deleteInstance → clearBoundVfoFor`** (deletion runs the dtor which calls `handler_.stop()`; the binding stays alive across that window so a re-spawn under the same name doesn't see an empty binding mid-teardown). The `rtl433_decoder` ctor inspects `predator::hold::getBoundVfoFor(name_)`; non-empty → "bound mode" which (i) skips its own `createVFO`, (ii) calls `sigpath::vfoManager.findVFO(boundVfoName_)` to grab the held VFO's stream, (iii) skips `deleteVFO` on stop (HoldManager owns the VFO), (iv) forces `enabled_=true` regardless of persisted state because operator pause/resume lives on the `HoldEntry` not on the per-instance config. `findVFO()` was added to `VFOManager` for this — DO NOT delete the returned pointer; subscribe to `onVfoDelete` if you need pre-destruction notification (we use `Binder.preTick`'s in-band prediction instead). VFO bandwidth: held entries with `decoder=Native_RTL433` get their VFO bandwidth force-overridden to 250 kHz (`requiredVfoBandwidth(DecoderKind)`) at create time because rtl_433 needs a fixed input rate; the operator-displayed `bandwidth_hz` on the `HoldEntry` is a hint that's only honoured for non-decoder-bound entries. Diagnostics: `predator::hold::boundInstanceCount()` reports live bindings; `HoldDecoderBinder::activeCount()` reports spawned instances — they should match in steady state. If they diverge, either a `setBoundVfoFor`/`clearBoundVfoFor` was missed in the wire-up OR a module dtor crashed before reaching `clearBoundVfoFor`. Test surface: `tests/hold_decoder_binder_test.cpp` (19 cases / 94 assertions) + `tests/hold_manager_test.cpp` bandwidth-override block (141/141). Beyond the basic spawn / teardown / retry / no-double-spawn / null-callback coverage the architect insisted on three extra hardening points that are easy to regress: (i) **effective-bandwidth consistency end-to-end** — `HoldManager.tick()` takes an optional `BandwidthOverrideFn` so `anchorCb` doesn't reset a bound RTL433 VFO back to the operator's UI bandwidth every frame, and the binder's `preTick` uses the same `requiredVfoBandwidth(decoder)` math so its in-band predictions match the manager's keep-alive decisions (without this a narrow UI bw with a wide effective bw would predict in-band by one and out-of-band by the other → torn-down decoder still attached to a live VFO); (ii) **external-instance-delete recovery** — `preTick` and `postTick` both take an optional `instanceExistsCb` (wire-up passes `core::moduleManager.instances.find(name) != end()`); when external delete is detected on an entry that wants to KEEP its instance, binder silently drops `active_` and `postTick` respawns same frame; if a stale exists check spuriously erased a still-live entry, `postTick`'s adoption path catches the resulting `createInstance` collision and re-tracks the live instance instead of looping forever in deferred-spawn (mirrors the `existsCb` recovery `HoldManager` itself has for VFOs); (iii) **preTick drop-decision must run BEFORE `instanceExistsCb` short-circuit** — if a false-negative `instanceExistsCb` is allowed to erase `active_` without firing `destroyCb` when the entry ALSO wants to be torn down (removed / disabled / out-of-band), `HoldManager` would destroy the bound VFO while a live `dsp::sink::Handler` is still attached — the exact race #5 was designed to prevent. `destroyCb` is therefore called for every drop case unconditionally and must be idempotent (production `moduleManager.deleteInstance` returns -1 silently if name is gone, satisfies contract). Three sub-tests in `test_preTick_destroyCb_runs_even_when_existsCb_false_negative` pin entry-removed, entry-disabled, and out-of-band each combined with a `falseExists` callback.

-->

### Python backend
- **RNS commanding wrapper (#6).** `predatorrf/cmd.v1` aspect carries Kujhad-shape `{class,action,args}` tasking over Reticulum. `tx.*` hard-rejected at wrap AND unwrap (RX-only). Per-peer LRU dedupe `(uid, ts_ms//1000)`, peer allowlist + loop suppression shared with cot.v1. Wire body byte-identical to Kujhad HTTP `/v1/command` so a single Device-side dispatcher serves both transports. Opt-in via `config["cmd_v1_enabled"]` (default False). Test surface: `python -m unittest backend.tests.test_rns_cmd -v` (29 cases). Full design + auth model + diagnostics → `docs/rns_commanding.md`.
- **CustodyElector + C++↔Python parity (#3).** Per-track cache must be released on `TrackManager._age_tracks()` via `custody_elector.forget()`; hard-gate ordering puts GPS-sync before stale-GPS; tests must use a far-future `now_ns` (`2e18`) so subtracting 600 s stays positive; opt-in via `config.custody_election_enabled` with `AutoTasker` falling back to legacy heuristic. C++ port in `core/src/predator/custody_election.h` MUST produce byte-identical decisions to the Python elector — drift is caught by `python scripts/test_custody_parity.py`. Five test-helper footguns and the C++ wiring status (consumed only by unit tests until #6/#7 land) live in `docs/custody_election.md`. Read that before touching `backend/coordination/custody_election.py`, `core/src/predator/custody_election.h`, or the parity harness.
- **StationarityGate (#3.5).** Stateless w.r.t. tracks — the caller (`PredatorBackend._try_tdoa_solve`) owns `location_history` and must trim to `gate.history_max` and pass `prior_motion_state` for hysteresis. Velocity-gate `dt_floor_s=2.0`, mobile-track STABLE-at-25 (vs 10 for stationary), invalid-candidate rejection rules, env-var configuration, and diagnostics all live in `docs/stationarity_gate.md`. Read that before touching `backend/fusion/stationarity_gate.py` or the TDOA solve path.

<!-- legacy block trimmed; expanded version in docs/custody_election.md
- **CustodyElector cache must be released on track archive:** `CustodyElector` keeps a per-track decision cache (`_last_decisions`) so it can compute handover overlap without callers having to thread previous-decision state through. `TrackManager._age_tracks()` calls `custody_elector.forget(track_id)` at the moment a track moves from `self.tracks` to `self._archived` — without that hook the cache grows without bound across long missions (one entry per emitter ever seen, ~bytes per entry × 100s/hour for a busy band). The hook is wrapped in `try/except` because `forget()` is idempotent and we never want a custody-cache bug to take down track archival. Diagnostic for regression: `CustodyElector.stats()["tracks_in_cache"]` should track `len(track_manager.tracks) + handover_overlap_window_count`, NOT grow monotonically. Hard-gate ordering inside `_hard_gate()` is also load-bearing: GPS-sync gate runs BEFORE the stale-GPS gate, so a `gps_synchronized=False` node short-circuits with `tdoa_threat_requires_gps_sync` instead of falling through to a misleading `gps_fix_stale_*s` reason. Tests in `backend/tests/test_custody_election.py` use a wall-clock value of `2_000_000_000_000_000_000` ns (year ~2033) so subtracting 600 s from "now" stays positive — using `time.time_ns()` directly works in production but `now=10_000_000_000` (10 s) goes negative when subtracting 600 s and silently bypasses the `> 0` guard in the stale-GPS check, producing a false-positive test pass. The elector is opt-in via `config.custody_election_enabled` (default True); when False, `DecisionEngine` falls back to the legacy `_select_nodes_for_tasking()` heuristic and `AssessmentReport.custody` is None — `AutoTasker` keeps working unchanged either way because `recommended_nodes` is populated from `custody.tasked_nodes` when the elector is on, and from the legacy heuristic when off.
- **CustodyElector C++ ↔ Python parity:** The custody election logic exists in two places — `backend/coordination/custody_election.py` (Python TOC backend) and `core/src/predator/custody_election.h` (C++ header for Controller-mode Predator-RF nodes that run without a Python backend). Both MUST produce identical decisions for identical inputs because in mixed deployments (Python TOC + Controller-mode Android) the operator's on-device tasking would diverge from the TOC otherwise. Drift is caught by `python scripts/test_custody_parity.py` which compiles `tests/custody_election_test.cpp`, runs both electors against `tests/fixtures/custody_scenarios.json`, and diffs outputs with `FLOAT_TOL=1e-4` (both sides round score components to 4 decimals before emitting JSON, so a 1e-4 epsilon catches algorithmic drift while ignoring last-bit float reordering). Five footguns the parity test pins down: (1) **default `gps_updated_ns` in test helpers MUST be within `stale_gps_after_s` of `now_ns`** — the obvious "1e18 ns" sentinel is ~30 years stale relative to a `kTestNowNs=2e18` and silently hard-gates every node on every high-threat scenario, producing false-pass tests where both implementations agree on "no primary" for the wrong reason. The C++ helper sets `gps_updated_ns = kTestNowNs - 10s`. (2) **C++ emits `""` where Python emits `None`** for absent primary / handover_from — `_normalize()` in the parity script collapses both to `None` before comparison so this encoding difference doesn't mask real algorithmic mismatches. (3) **`SensorNodeTrust.compute_trust_score()` is monkey-patched in the harness** to return the fixture's `trust_score` verbatim — the C++ port doesn't reimplement compute_trust_score (it expects the Controller to compute trust from peer history), so without the monkey-patch Python's score floats from 0.05..0.98 while C++'s is whatever the fixture says. (4) **`gps_age_component` returns 0.0 for `age_s >= stale_gps_after_s` BEFORE the weighted sum** — both sides clamp identically; if either side ever switches to "negative component goes through, clamp at total" the parity test fails immediately because the negative component value would dominate the 0..1-bounded ones. (5) **`detecting_nodes` is a `List[str]` on `EmitterTrack`, NOT a `set`** — appending the same node twice in fixture conversion would silently double-count `heard==True` membership in the future if the SNR component ever switches from set-membership to count-of-occurrences. Wiring on the C++ side: Controller-mode UI and tasking dispatch are not yet built (queued behind roadmap items #6 RNS commanding wrapper and #7 Android TDOA viewer), so today the header is consumed only by the unit tests. When Controller-mode UI lands, instantiate `predator::custody::Elector` once per Controller session, call `elect()` per peer-state-update tick with `TrackInput` + `NodeInput` derived from `KujhadPeerSnapshot.state` / `.gps`, and route the `setOnChange` callback into the same Kujhad event queue the spectrum overlays use. Diagnostic for parity regression: `python scripts/test_custody_parity.py` fails with a per-step diff naming the specific field that diverged (e.g. `step[3].handover_until_ns: 1500000000017000000 != 1500000000016999999`); `--keep-build` retains the compiled binary at `/tmp/custody_parity_*/custody_election_test` for `lldb`/`gdb` follow-up.
- **StationarityGate is stateless w.r.t. tracks — the caller owns history.** (See `docs/stationarity_gate.md` for the full text — trimmed here to keep replit.md scannable.)
-->

## Pointers

- [SDR++ GitHub](https://github.com/AlexandreRouma/SDRPlusPlus)
- `docs/predator_hold.md`
- `docs/custody_election.md`
- `docs/stationarity_gate.md`
- `docs/rns_commanding.md`
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
