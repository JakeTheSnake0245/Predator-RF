# Predator RF — Repository Map & Gotchas

This is the detailed reference behind the `replit.md` index. `replit.md` stays
a compact key list; every entry there points here or to a `docs/*.md` file.
Keep this file authoritative: when adding a component or a gotcha, add the
detail HERE and (if it's a new top-level area) one index line in `replit.md`.

## Where things live

- `server.py`: Python HTTP server for the informational landing page (`index.html`) and an operator UI mockup (`preview.html`).
- `core/src/predator/kujhad_fleet.h`: Header-only Kujhad Fleet hub-and-spoke peer protocol (HTTP/TLS server).
- `core/src/predator/decoder_ingest.h`: Header-only receive-only decoder ingestion base class.
- `decoder_modules/rtl433_decoder/`: Native rtl_433 ISM decoder module.
- `core/src/gui/style.cpp`: Contains `applyTouchFriendlyTweaks()` for Android UI adjustments. Base font glyph range adds Misc-Symbols (U+2600..U+26FF) for the gear icon (U+2699) used by the Hits page per-marker action sheet.
- `decoder_modules/kraken_lob_decoder/`: KrakenSDR LOB decoder module — WebSocket bridge to krakensdr_doa. See `docs/krakensdr.md`.
- `source_modules/krakensdr_source/`: KrakenSDR source informational panel module.
- `backend/models/lob_measurement.py`: LOBMeasurement dataclass — one bearing observation from one KrakenSDR node.
- `backend/fusion/lob_triangulator.py`: LOBTriangulator — stateless 2-node closed-form + N-node WLS (numpy) solver. 15° crossing-angle veto, 0.70 confidence ceiling.
- `docs/krakensdr.md`: KrakenSDR hardware setup, build flags, Python API, map visualization, CoT export, unit tests, and footguns.
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
- `backend/fusion/df_capability.py`: Fleet DF capability summary (LOB-capable / TDOA-viable / RSSI-only) — served at `GET /api/v1/nodes/df_capability` on BOTH backends (Python `backend/api/routes/nodes.py`, C++ `routeDfCapability` in `core/backends/web/backend.cpp`). Drives the dashboard DF pill + fleet-panel warning.
- `backend/fusion/stationarity_gate.py`: TDOA fix sanity filter (rejects physically-impossible velocity jumps, NaN/inf/out-of-range coords, zero/negative timestamps) + motion-state classifier (RMS-spread vs ellipse → stationary/mobile/unknown with hysteresis). Stateless — caller owns the per-track history list. Wired into `PredatorBackend._try_tdoa_solve` and `EmitterTrack._advance_state` (mobile tracks need 25 obs to promote to STABLE vs 10 for stationary/unknown).
- `core/src/predator/tdoa_coordinator.h`, `core/src/predator/stationarity_gate.h`, `core/src/predator/fleet_tdoa_aggregator.h`: Controller-mode (#7) C++ ports of the Python TDOA pipeline. Header-only, pure stdlib. See `docs/tdoa_controller.md` for parity contract, wire-up sketch, and the MapLibre payload schema.
- `deploy/`: Deployment scripts and configurations for the Python backend. `fetch_snapshot.sh` = standby-kit puller for `GET /api/v1/snapshot` (coordinator failure recovery — see runbook §9); preflight has a `backup` check gated by `PREFLIGHT_STANDBY=1`.
- `deploy/predator-rfd.service`: systemd unit for the headless C++ web backend daemon.
- `core/backends/web/backend.cpp`: Headless web backend — implements the `backend::` interface with an embedded HTTP+WebSocket server (port 5555) instead of a display. Exposes `/tracks/`, `/nodes/`, `/events/stream`, `/api/spectrum`, `/ws` (WebSocket), and `/v1/*` Kujhad aliases. Feeds live data via `backend::webBackendPush*()` hooks.
- `core/src/predator/event_ring_store.h`: Header-only durable JSONL append-log for the device Kujhad event ring (two-segment rotation, fflush-only, corrupt-tail tolerant). Wired into `core/backends/web/backend.cpp` (`gEventStore`); test: `g++ -std=c++17 -O2 -Icore/src tests/event_ring_store_test.cpp -o /tmp/erst && /tmp/erst`.
- `core/backends/web/web_server.h`: Standalone HTTP+WebSocket+SSE server (header-only, POSIX, no external deps beyond optional OpenSSL SHA-1). Used only by the web backend. Self-contained SHA-1 fallback when OpenSSL is absent.
- `predator-rfctl/main.cpp`: CLI tool that connects to the daemon's Unix control socket (`/run/predator-rfd/control.sock`) and sends typed JSON commands. Build with `OPT_BACKEND_WEB=ON`.
- `predator-rfctl/CMakeLists.txt`: Build config for `predator-rfctl`.
- `web/`: Static web assets served by `predator-rfd`. `preview.html` is the canonical dashboard; copy or symlink it here for the installed package.
- `docs/linux_web_frontend.md`: Build instructions, API reference, CLI usage, and parity contract for the web backend.
- `core/src/predator/foxhunt/`: Fox Hunt TX — `tx_driver.h/.cpp` (driver interface + process-wide registry anchored in sdrpp_core) and `replay_engine.h` (header-only IQ/tone/CW replay worker). Drivers: `source_modules/soapy_source/src/foxhunt_tx.cpp` (SoapySDR TX, Linux/RPi) and `source_modules/plutosdr_source/src/foxhunt_tx.cpp` (libiio TX, the Android path). Tab UI in `main_window.cpp` (`PREDATOR_TAB_FOXHUNT`). Test: `g++ -std=c++17 -O2 -Icore/src tests/foxhunt_replay_engine_test.cpp -o /tmp/frt && /tmp/frt`. See `docs/foxhunt.md`.
- `docs/`: Project documentation, including API contracts and integration guides.


## Gotchas

### KrakenSDR LOB (#8)
- **Heading correction is the caller's responsibility.** `LOBMeasurement.bearing_deg` must be true-north. For vehicle-mounted arrays, add `heading_deg` to the raw DOA bearing (mod 360) *before* constructing the measurement. Predator RF does NOT apply this correction internally.
- **Crossing-angle veto.** 2-node triangulation is rejected when `|sin(b1−b2)| < sin(15°)`. The operator should wait for a better-separated pair or a third node rather than forcing a fix.
- **KrakenSDR modules are ON by default.** Use `--no-kraken` with `build_linux.sh` or `-DOPT_BUILD_KRAKENSDR_LOB_DECODER=OFF` in CMake to exclude them. Android always includes the modules (enabled at runtime).
- **numpy is optional but required for N-LOB WLS.** Without `numpy`, `LOBTriangulator` falls back to the 2-LOB closed-form solver. Install `numpy scipy` into the backend venv for 3+-node least-squares.
- **Android KrakenSDR is WebSocket only.** No direct USB access from Android. The KrakenSDR array must run `krakensdr_doa` on a companion RPi reachable over Wi-Fi/LAN.
- See `docs/krakensdr.md` for full details and all five footguns.


### Android soft-keyboard field visibility (Extract UI)
Every keyboard-editable field must remain visible above the floating IME
(`adjustNothing` means nothing resizes). Three mechanisms, by context:
- **Predator tabs:** `openPendEdit` / `openPendEditNumeric` popup editor
  (numeric variant raises the number pad). Helper: `drawEditDoubleButtonNumeric`.
- **Modals (RNS editor, passphrase dialogs):** `iv()`-style
  scroll-into-view called after EVERY input field (45/45 covered in the
  RNS interface editor).
- **Left-menu widgets:** `SmGui::InputText/InputInt` auto-scroll via
  `core/src/gui/widgets/ime_scroll.h`; direct ImGui users were converted to
  `ImGui::InputTextIME/InputIntIME/InputFloatIME/InputDoubleIME` wrappers
  (scanner, frequency_manager, rigctl_*, network_sink, predator_node_source,
  sdrpp_server_source, recorder, radio, kraken_lob_decoder, scheduler).
  NEVER add a raw `ImGui::Input*` in menu/module UI — use the IME wrappers.

### Cross-cutting
- The Replit environment only serves an informational landing page and interactive UI mockup (`server.py`). It does **not** run the Android build, the Python backend, or `predator-rfd`.
- `X-Kujhad-Key` header is required for authentication on all `/v1/*` Kujhad API calls.
- The `predatorrf/cot.v1` RNS Destination is additive, not a replacement for TCP/TLS Kujhad control-plane transport.
- For Android builds, `assembleDebug` is the documented happy path as `release` is unsigned by design.

### Linux web backend (predator-rfd)
- **Build flag:** set `OPT_BACKEND_WEB=ON` and `OPT_BACKEND_GLFW=OFF`. Both backends compile different `backend.cpp` TUs into `sdrpp_core`; you cannot enable both in the same build.
- **Headless mode:** `predator-rfd` never calls ImGui or OpenGL. `backend::render()` and `backend::beginFrame()` are no-ops. The main loop in `backend::renderLoop()` sleeps 50 ms per tick and lets the web server threads handle requests. State that previously lived in `main_window.cpp`'s kujhad snapshots is NOT automatically populated — wire-up hooks `backend::webBackendPush*()` must be called from the signal path or from a future thin shim that reads `sigpath::*` singletons.
- **Dashboard parity:** `preview.html` is the canonical dashboard. It is copied to `web/index.html` and served as the static root. Any change to the dashboard's API surface must be reflected in both `backend/main.py` (Python) and `core/backends/web/backend.cpp` (C++). See architecture decision above.
- **Control socket perms:** `/run/predator-rfd/control.sock` is created with `chmod 600` so only the `predator` user (daemon owner) and root can talk to it. `predator-rfctl` must run as the same user or root.
- **Plain-HTTP lockout observability:** when Kujhad TLS is off, non-loopback rejects are counted + rate-limit logged, surfaced as a red LOCKOUT warning in the Kujhad tab, and an opt-in `kujhadPlainHttpAllowCidrs` overlay CIDR allowlist (default empty) can admit tailnet/ZeroTier ranges. See `docs/linux_web_frontend.md`.
- **Event ring persistence:** the web backend's event ring is rehydrated on start from `<root>/kujhad_events/` (override: `webEventStoreDir` config or `PREDATOR_EVENT_STORE_DIR` env) and `gLastEventId` restarts ABOVE the highest persisted id — never reset it, or `since=` coordinator cursors silently skip events. The GUI/Android build persists events via `predatorEvents` in config.json instead and restores `predatorEventSerial` from the persisted max serial on first frame (same serial-reuse rule).
- **tx.* rejection:** `tx.*` class commands are rejected by the control socket AND by the web `/api/command` endpoint, matching the Kujhad and RNS rejection posture. The `predator_node_source` remote-cockpit module defines `OPT_ALLOW_TX_COMMANDS` `PRIVATE` to its own target as a declaration of Controller intent — it does NOT propagate to `sdrpp_core`/`backend.cpp`, and the Controller forwards via its own `PredatorNodeClient` HTTP client (no local guard), so behavior is unaffected. Device nodes reject tx.* unconditionally at their web backend. Never set the flag globally. See `docs/remote_cockpit.md`.

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

### Fox Hunt TX
- **`OPT_BUILD_FOXHUNT` gates ALL Fox Hunt code** (tab, engine, drivers). Default ON; forced OFF when `OPT_BACKEND_WEB=ON` so predator-rfd stays RX-only. `build_linux.sh --no-foxhunt` for an RX-only GUI build; Android passes `-DOPT_BUILD_FOXHUNT=ON` in build.gradle.
- **Local-hardware-only TX.** The Kujhad HTTP / RNS cmd.v1 / web `/api/command` / control-socket `tx.*` hard-rejects are untouched — no remote peer can reach the Fox Hunt path. ARM state is never persisted (every launch starts disarmed).
- **Drivers live inside existing source modules** (globbed `src/*.cpp`, `#ifdef`-guarded) so no new CMake target or Gradle native target exists; the registry singleton is anchored in `tx_driver.cpp` inside sdrpp_core to avoid one-instance-per-DSO.
- Operator is responsible for TX legality (frequency, power, station ID); CW ID is a convenience, not compliance. See `docs/foxhunt.md`.

### Non-Kraken fleet TDOA hardening + DF capability
- **System-clock gating.** When every TDOA participant is a system-clock node (`can_do_tdoa=False`, e.g. all-RTL fleets), a solve needs ≥3 distinct hearers (`SYSTEM_CLOCK_MIN_DISTINCT`) — 2-node hyperbolas are suppressed with measurements re-merged — and the resulting confidence is hard-capped at 0.35 (`SYSTEM_CLOCK_CONF_CAP`). Mirrored in C++ via `Measurement::hardware_timed` / `kSystemClockMinDistinct` / `kSystemClockConfCap` (defaults `hardware_timed=true` keep old callers unchanged). GPS freshness is still gated at record time. See `docs/tdoa_controller.md` item 6.
- **DF capability summary.** `GET /api/v1/nodes/df_capability` (both backends) reports `df_mode` (lob|tdoa|rssi_only|none), LOB/TDOA counts, and an operator warning; the dashboard shows a DF header pill and a fleet-panel banner. LOB-capable = hardware_code contains "kraken" or a KRAKEN_LOB detector. Offline nodes never count.
- **Proximity labeling.** Track payloads carry `location_is_proximity` (true when `location_method == "rssi_proximity"`); the dashboard renders a ~PROX badge in the tracks table and detail modal so RSSI rings are never mistaken for fixes.

### C++ Controller-mode TDOA (#7)
- **Header-only TDOA pipeline.** Three pure-stdlib headers in `core/src/predator/`: `tdoa_coordinator.h` (per-emitter measurement queue + iterative LSQ solver, port of `backend/fusion/tdoa_coordinator.py`), `stationarity_gate.h` (fix sanity filter + motion classifier, port of `backend/fusion/stationarity_gate.py`), `fleet_tdoa_aggregator.h` (Kujhad event → measurement glue with TTL prune, distinct-node gate, per-emitter solve cooldown). No Eigen / LAPACK / JSON — the test runner builds each TU with one g++ invocation. Test surface: `tests/tdoa_coordinator_test.cpp` (51 assertions), `tests/stationarity_gate_test.cpp` (26 assertions), `tests/fleet_tdoa_aggregator_test.cpp` (29 assertions). Five footguns + the wire-up sketch + the Python parity stance live in `docs/tdoa_controller.md`. **Parity is operational, not bit-identical** — the C++ LSQ adds a Tikhonov ridge (`1e-3 * trace`) and a 50 km step cap because numpy's SVD-based `lstsq` is intrinsically more stable on rank-deficient geometry; well-conditioned scenarios (distinct >= 3, time offsets within `baseline / c`) match Python within numerical noise, ill-conditioned scenarios diverge by design (Python's solver also struggles there, just less explosively). UI wire-up to the MapLibre WebView is defer-able and documented in the same doc; the JSON contract is fixed so the Android side can be built without further C++ changes. Read `docs/tdoa_controller.md` before touching any of the three headers, the `requiredVfoBandwidth`-style emitter-key helper, or the (forthcoming) main_window.cpp tick hook.

### Python backend
- **RNS commanding wrapper (#6).** `predatorrf/cmd.v1` aspect carries Kujhad-shape `{class,action,args}` tasking over Reticulum. `tx.*` hard-rejected at wrap AND unwrap (RX-only). Per-peer LRU dedupe `(uid, ts_ms//1000)`, peer allowlist + loop suppression shared with cot.v1. Wire body byte-identical to Kujhad HTTP `/v1/command` so a single Device-side dispatcher serves both transports. Opt-in via `config["cmd_v1_enabled"]` (default False). Test surface: `python -m unittest backend.tests.test_rns_cmd -v` (29 cases). Full design + auth model + diagnostics → `docs/rns_commanding.md`.
- **CustodyElector + C++↔Python parity (#3).** Per-track cache must be released on `TrackManager._age_tracks()` via `custody_elector.forget()`; hard-gate ordering puts GPS-sync before stale-GPS; tests must use a far-future `now_ns` (`2e18`) so subtracting 600 s stays positive; opt-in via `config.custody_election_enabled` with `AutoTasker` falling back to legacy heuristic. C++ port in `core/src/predator/custody_election.h` MUST produce byte-identical decisions to the Python elector — drift is caught by `python scripts/test_custody_parity.py`. Five test-helper footguns and the C++ wiring status (consumed only by unit tests until #6/#7 land) live in `docs/custody_election.md`. Read that before touching `backend/coordination/custody_election.py`, `core/src/predator/custody_election.h`, or the parity harness.
- **StationarityGate (#3.5).** Stateless w.r.t. tracks — the caller (`PredatorBackend._try_tdoa_solve`) owns `location_history` and must trim to `gate.history_max` and pass `prior_motion_state` for hysteresis. Velocity-gate `dt_floor_s=2.0`, mobile-track STABLE-at-25 (vs 10 for stationary), invalid-candidate rejection rules, env-var configuration, and diagnostics all live in `docs/stationarity_gate.md`. Read that before touching `backend/fusion/stationarity_gate.py` or the TDOA solve path.


## Where things live (continued)

- `source_modules/predator_node_source/`: Remote-cockpit source module — phone/tablet acts as Controller, RPi/mini-PC runs predator-rfd as Device. Link auto-detection (Android hotspot → RPi AP → static IP), full command forwarding, NO tx.* restriction on the Controller side. See `docs/remote_cockpit.md`.
- `backend/signal_repository/repository.py`: Three-tier signal store — metadata (always), fingerprint (STABLE tracks), IQ captures (on demand). SQLite, `REPO_SCHEMA_VERSION=1`. See `docs/signal_repository.md`.
- `backend/signal_repository/fingerprinter.py`: 32-float cosine-similarity fingerprint vectors from FFT bins or signal metadata (octave bands, moments, flatness, autocorr, BW fraction). See `docs/signal_repository.md`.
- `backend/coordination/fleet_state_manager.py`: FleetStateManager — in-memory snapshot of all peer nodes (tracks, events, GPS, SDR status). Global event ring with monotonic serials for lossless client catch-up. See `docs/correlation_engine.md`.
- `backend/coordination/phone_gps.py`: PhoneGPSSource — coordinator kit pulls GPS from its paired phone's Kujhad `/v1/gps` (LOCAL_GPS_PHONE_* env). Live fix → gps_source="phone"; outage keeps last fix honestly, then reverts to manual (LOCAL_NODE_LAT/LON, updated_ns=0 so TDOA excludes it) — never a fake fix. Standalone coordinator node exposed via `KujhadFleetManager.local_node`/`all_nodes()`; nodes API + dashboard show gps_source/gps_age_s. Tests: `python -m unittest backend.tests.test_phone_gps -v` (17 cases).
- `backend/coordination/link_health.py`: LinkHealthMonitor — node online↔offline transition detector (120 s staleness rule). Fed by `_link_health_loop` in `main.py`; emits `node_online`/`node_offline` events onto SSE + fleet event ring. Tests: `python -m unittest backend.tests.test_link_health -v`.
- `backend/coordination/correlation_engine.py`: CorrelationEngine — operator-defined band+node rules; fires when ≥N distinct nodes hear a signal in a time window. Also handles known-target fingerprint match → geo-cue + intercept log. See `docs/correlation_engine.md`.
- `backend/coordination/iq_capture_service.py`: IQCaptureService — operator-demand raw IQ recording forwarded to the target Device node via Kujhad.
- `backend/coordination/target_nomination.py`: NominationManager — operator "nominate target" (single active mission target, v1). Persists in MissionStore table `op_nominated_target`; on nominate: known-target repo entry (kept on clear) + correlation rule `nominated_target` (±25 kHz, removed on clear). REST: `backend/api/routes/nomination.py` at `/api/v1/target` (GET nomination / POST nominate / DELETE nomination). Track dicts get `is_nominated_target` at the route layer (`backend/api/routes/tracks.py`). C++ parity: GET returns `{"nominated":null,"supported":false}`, writes 501 (`routeTargetNomination*` in `core/backends/web/backend.cpp`). Dashboard: ◎ TARGET pill + row badge + modal Nominate/Clear; map draws a double red ring on the nominated dot. Tests: `python -m unittest backend.tests.test_target_nomination -v` (17 cases). No auto-CoT on nomination.
- `backend/api/repository_routes.py`: REST API for signal search, fingerprint similarity, IQ capture trigger, intercept list, correlation rule CRUD, fleet state/events (`/api/v1/repository/`).
- `backend/tests/test_signal_repository.py`: 39-case test suite — fingerprint math, repository CRUD, correlation rules, fleet state. Run: `python -m unittest backend.tests.test_signal_repository -v`.
- `docs/remote_cockpit.md`: Remote-cockpit architecture, tx.* unlock model, link auto-detection, operator runbook.
- `docs/signal_repository.md`: Signal repository API, schema, fingerprinter feature set, REST routes.
- `docs/correlation_engine.md`: CorrelationEngine rule API, firing logic, known-target response, FleetStateManager event ring, IQCaptureService.

