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

### Python backend
- **RNS commanding wrapper (#6).** `predatorrf/cmd.v1` aspect carries Kujhad-shape `{class,action,args}` tasking over Reticulum. `tx.*` hard-rejected at wrap AND unwrap (RX-only). Per-peer LRU dedupe `(uid, ts_ms//1000)`, peer allowlist + loop suppression shared with cot.v1. Wire body byte-identical to Kujhad HTTP `/v1/command` so a single Device-side dispatcher serves both transports. Opt-in via `config["cmd_v1_enabled"]` (default False). Test surface: `python -m unittest backend.tests.test_rns_cmd -v` (29 cases). Full design + auth model + diagnostics → `docs/rns_commanding.md`.
- **CustodyElector + C++↔Python parity (#3).** Per-track cache must be released on `TrackManager._age_tracks()` via `custody_elector.forget()`; hard-gate ordering puts GPS-sync before stale-GPS; tests must use a far-future `now_ns` (`2e18`) so subtracting 600 s stays positive; opt-in via `config.custody_election_enabled` with `AutoTasker` falling back to legacy heuristic. C++ port in `core/src/predator/custody_election.h` MUST produce byte-identical decisions to the Python elector — drift is caught by `python scripts/test_custody_parity.py`. Five test-helper footguns and the C++ wiring status (consumed only by unit tests until #6/#7 land) live in `docs/custody_election.md`. Read that before touching `backend/coordination/custody_election.py`, `core/src/predator/custody_election.h`, or the parity harness.
- **StationarityGate (#3.5).** Stateless w.r.t. tracks — the caller (`PredatorBackend._try_tdoa_solve`) owns `location_history` and must trim to `gate.history_max` and pass `prior_motion_state` for hysteresis. Velocity-gate `dt_floor_s=2.0`, mobile-track STABLE-at-25 (vs 10 for stationary), invalid-candidate rejection rules, env-var configuration, and diagnostics all live in `docs/stationarity_gate.md`. Read that before touching `backend/fusion/stationarity_gate.py` or the TDOA solve path.

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
