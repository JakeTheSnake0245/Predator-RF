# Predator RF

## Overview

Predator RF is a joint sensing platform for one solo SIGINT operator + a couple
dozen RPi/SDR/GPS sensors + an optional Linux TOC. **RX-only, log-and-map-only.**
The C++ side started as a fork of [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus)
and has grown a Predator-specific shell, mission system, hits/events workflow,
a Kujhad Fleet hub-and-spoke peer protocol (TLS), ATAK CoT export, native
RTL433/P25/ADS-B ingesters, and a Baseline tab. The repo also contains a
Python intelligence backend (`backend/`, ~3.6k LOC) that consumes the Kujhad
HTTP API as its wire contract.

## Architecture (Path 1 — what's actually built)

Two-tier system, **Python backend on top of the Kujhad Fleet HTTP API**:

```
[ C++ Predator-RF nodes ]                    [ Python backend (one-per-fleet) ]
  - DSP / decoders / hits        ──HTTP──▶     KujhadClient (async poller)
  - Kujhad HTTP server (TLS)         /v1/*       │
  - per-node API key                             ▼
  - Native: RTL433, P25(DSD-FME),              TrackManager
            ADS-B, future POCSAG/AIS             │   ▼
                                                 ▼   AnomalyDetector (6 methods)
                                              EmitterTracks      │
                                                 │               ▼
                                                 ▼          DecisionEngine
                                              FastAPI: /api/v1/{tracks,nodes,
                                                              events,assessments}
```

- **Wire contract** is `core/src/predator/kujhad_fleet.h` (header-only HTTP/TLS
  server with `X-Kujhad-Key` auth). Endpoints: `GET /v1/identify`, `/v1/state`,
  `/v1/gps`, `/v1/events?since=`, `POST /v1/command`. Command schema is
  class+action+args; `tx.*` is hard-rejected at the wire (RX-only build).
- **Python client** is `backend/coordination/kujhad_client.py`. Schema verified
  against C++ event-row builders in `core/src/gui/main_window.cpp`
  (appendPredatorEvent ~L1334, RTL433 ~L1490, native ~L1545, ADSB ~L1620,
  P25 ~L1714). Wire fields used: `serial, time(ISO), type∈{hit,decoder},
  frequency, strengthDb, label, protocol, networkId, talkgroup, radioId,
  decoder, hitState, lat, lon, accuracyM, gpsFix, encrypted?, raw`.
- **Multi-transport (RNS/LoRa) is post-MVP**, not v1. Slots in *under* the
  Kujhad HTTP layer in a future release. v1 ships TCP/TLS over IP only.
- **CoT export** is operator-initiated only in v1 (`escalate_to_atak=True`
  flag from `DecisionEngine` is advisory; transmission is manual).
- See `docs/1_conops.md` §1.5 for the full implementation-status snapshot.

## Project Type

Two halves, **one repo**:

1. **C++ Android/desktop application** — CMake build, Dear ImGui UI, OpenGL/GLES,
   FFTW3+Volk DSP, Kotlin/JNI Android wrapper. The shippable end-user artifact.
2. **Python intelligence backend** (`backend/`) — FastAPI + asyncio, consumes
   the Kujhad HTTP API. Optional but recommended for fleets ≥ 2 nodes; the
   C++ side is fully functional standalone (lone-wolf vignette).

## Replit Environment

Since the C++ application is built and run elsewhere (Android via Gradle on
Windows, Linux via CMake), the Replit workflow only serves an unrelated
informational landing page. **Port 5000 = `server.py` static HTML, not the
Predator backend.** Do not attempt to run the Android build or the Python
backend in this Repl; it's a code workspace.

## Project Type

This is a **C++ desktop/Android application** — not a web app. It uses:
- **CMake** as the build system
- **Dear ImGui** for the GUI
- **OpenGL / GLES 3** for rendering
- **FFTW3 + Volk** for DSP processing
- **Kotlin/JNI** for the Android wrapper

## Replit Environment

Since this is a native C++ application (not a web app), a simple Python HTTP server (`server.py`) serves an informational landing page (`index.html`) at **port 5000**. This page describes the project, its tech stack, roadmap, and build instructions.

### Files

- `server.py` — Python HTTP server serving the landing page on port 5000 with routes `/` (info) and `/preview` (interactive operator UI mockup)
- `index.html` — Project info/landing page (links to `/preview`)
- `preview.html` — Interactive HTML mockup of the Predator RF operator interface (Spectrum, Hits, Network Tree, Map, Mission, Kujhad Fleet, System tabs) styled in Diablo-tactical dark theme; pure presentation, no backend
- `core/src/predator/kujhad_fleet.h` — **Kujhad Fleet console (Task #1).** Header-only hub-and-spoke peer protocol. `KujhadDeviceServer` is a tiny embedded HTTP/JSON server (listener thread + per-connection worker) that publishes the local SDR state to peers. Endpoints: `GET /v1/identify`, `GET /v1/state`, `GET /v1/gps`, `GET /v1/events?since=`, `POST /v1/command`, plus `GET /` which returns a self-contained operator console HTML page. Auth: `X-Kujhad-Key` header on every `/v1/*` call. Command schema is class+action (`tune`, `scan`, `mission`, `identify`); the `tx.*` class is rejected at the wire (RX-only build, returns 403). `KujhadControllerClient` is the controller-side per-peer worker that polls identify/state/gps once a second, drains events, and sends typed commands. `kujhadEnumerateInterfaces()` scans non-loopback IPv4 interfaces and ranks them (ZeroTier > Tailscale > LAN) for the Reachable Addresses list. v1 ships plaintext over private overlays; the socket layer is connection-typed so a future release can swap for OpenSSL BIO without touching the protocol or auth code.
- `core/src/predator/decoder_ingest.h` — Receive-only decoder ingestion (header-only). Abstract `predator::LineIngester` base owns the socket/thread/queue plumbing (TCP client + UDP server modes, auto-reconnect with exponential backoff, non-blocking connect with stop-flag polling, bounded queue); per-decoder subclasses override `parseLine()`. Implemented: `Rtl433Ingester` (rtl_433 JSON Lines), `AdsbIngester` (dump1090 / readsb BaseStation port 30003 CSV — extracts ICAO hex, callsign, altitude, lat/lon, squawk; freq pinned to 1090 MHz), `P25Ingester` (DSD-FME / OP25 JSON-line; multi-alias keys; Hz/MHz heuristic with sanity range; surfaces WACN/RFSS/Site/TG/Radio + encrypted/algid/keyid; site/system status records retained)
- `decoder_modules/rtl433_decoder/` — **Native rtl_433 ISM decoder module.** Pre-flight clean: full `gcc -std=gnu11 -fsyntax-only` sweep across all 264 vendored `.c` files passes (one fix applied — `vendor/rtl_433/src/r_api.c` was missing `#include <limits.h>` for `UINT_MAX` and `#include <errno.h>` for the dumper fstat error path). Vendors rtl_433 24.10 (GPL-2.0-or-later) under `vendor/rtl_433/` minus its desktop SDR / mongoose / HTTP / MQTT / InfluxDB / GPSD layers. `src/predator_stubs.c` provides no-op symbol stubs for the excluded layers. `src/main.cpp` registers as a SDRPP module: creates `r_cfg_t`, registers all 235 protocols, attaches a custom `data_output_t` whose `output_print` callback converts each decoded `data_t` into a `predator::DecoderIngestEvent` and queues it. DSP path: VFO at 250 kHz BW → handler sink → CF32→AM-envelope (int16) + CS16→FM (int16) → `pulse_detect_package` → `run_ook_demods`/`run_fsk_demods` on `cfg->demod->r_devs`. Toggleable from the menu; existing `Rtl433Ingester` bridge stays as a fallback. Wired via `OPT_BUILD_RTL433_DECODER` in `CMakeLists.txt` and enabled in `android/app/build.gradle`.
- `core/src/gui/style.cpp` — Includes `applyTouchFriendlyTweaks()` for phone/tablet builds: bumps scrollbar, slider grab, frame border, rounding, and item spacing for thumb input. Called from `core/backends/android/backend.cpp::doPartialInit()` after `ScaleAllSizes(uiScale)` so the upstream desktop ImGui style is comfortable on a Samsung S22-class screen
- `docs/android_build.md` — End-to-end APK build guide: NDK 23.2 setup, sdr-kit installation, Gradle build, sideloading to S22, troubleshooting, and optional rebranding
- `android/sdr-kit/arm64-v8a/` — **Prebuilt native SDR libraries for the Android APK build, committed to the repo**. 15 `.so` files (~6.8 MB) extracted from the upstream SDR++ Android nightly APK + 94 public headers (~1 MB) assembled from each library's pinned source. Covers libusb, libfftw3f, libvolk, libzstd, librtlsdr, libairspy(hf), libhackrf, libhydrasdr, libiio, libxml2, libad9361, libcodec2, libcorrect, libfec. Total ~8 MB. Means `git clone` → `gradle assembleDebug` works without any sdr-kit setup. See `android/sdr-kit/README.md`.
- `scripts/fetch-sdr-kit.sh` — Reproducible refresh script for `android/sdr-kit/`. Downloads upstream sdrpp.apk, extracts arm64 `.so` files, parallel-clones each library at its pinned upstream version, generates volk's auto-generated headers via a native cmake build, copies public headers into the kit. Run from repo root: `bash scripts/fetch-sdr-kit.sh`.
- `CMakeLists.txt` — CMake build configuration for the C++ application
- `core/` — Core SDR engine (C++)
- `source_modules/` — Hardware driver plugins (RTL-SDR, HackRF, Airspy, etc.)
- `sink_modules/` — Audio/network output handlers
- `decoder_modules/` — Signal decoders (AM/FM/SSB, Meteor, M17, etc.)
- `misc_modules/` — Utility plugins (scanner, recorder, frequency manager, etc.)
- `android/` — Android Gradle project + Kotlin wrapper

## Building Natively (Linux)

```bash
sudo apt install cmake libfftw3-dev libglfw3-dev \
  librtlsdr-dev libhackrf-dev libairspy-dev \
  portaudio19-dev libsoapysdr-dev

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

## Key Subsystems (active development)

### Mission System (`core/src/gui/main_window.cpp`)
Diablo-inspired mission engine with four modes: **Manual, Classify, Scan, QuickScan**.
- **Search Bands** — operator-defined frequency ranges scanned continuously
- **Targets / Excludes** — specific frequencies to dwell on or skip
- `detectScanPeaks()` — FFT analysis with configurable dB threshold + SNR threshold
- `recordPeakHit()` — clustering deduplication, hit record creation, event logging
- `routeHitToVfo()` — dynamic M1…Mn VFO assignment for confirmed signals

### Hits Tab (`core/src/gui/main_window.cpp` ~line 1750)
Full implementation:
- Scrollable hit list (BeginChild, ~5 entries visible) with colored state dots (green=target, red=exclude, yellow=unknown)
- Unread badge highlighting (amber row background) and amber frequency text for new hits
- RSSI fill bar (green→amber→red) scaled -120 to -20 dBm per row
- Sort modes: Newest / Strongest / Most Hits / Most Events / Unread First / Marked First
- Quick filter: All / Targets / Excludes / Unknown
- Per-hit actions: Select, Assign/Release Marker, Decoder combo, Promote Target/Exclude
- Selected Hit panel: color-coded state header, RSSI bar, Rename/Notes, Tune, Route VFO, Mark Viewed
- Per-hit event log (scrollable BeginChild, newest-first, color-coded by type)
- Global Event Log (scrollable, newest-first, color-coded: green=hit, yellow=manual, blue=target, purple=decoder)

### Kujhad Fleet Console (`core/src/predator/kujhad_fleet.h` + `core/src/gui/main_window.cpp`)
Hub-and-spoke peer console for linking multiple Predator RF instances on a private overlay (Tailscale, ZeroTier, or loopback). The Kujhad tab (7th tab, between Mission and System) carries a **Role** dropdown:

- **Device** mode runs an embedded HTTP/JSON server on a configurable port (default 41947). It publishes `/v1/identify`, `/v1/state`, `/v1/gps`, `/v1/events?since=`, accepts `POST /v1/command`, and serves a self-contained operator console at `GET /` that any browser can load. Auth: `X-Kujhad-Key` header. The Reachable Addresses panel ranks local NICs (ZT > TS > LAN) so an operator can quickly tell a peer where to point.
- **Controller** mode reads a persisted peer list (`kujhadPeers` array of `{name,host,port,apiKey,enabled}`), spins up a `KujhadControllerClient` per enabled peer, and drains their `/v1/events` tail into the local event log tagged with `sourceDevice = <peer name>`. The Active Peer Commands panel lets the operator send `tune`, `scan`, `identify` commands to whichever peer is selected via "Take control".

**Safety**: command schema is typed by class so a future `tx.*` class can be added behind explicit per-device permissions without reshaping the protocol. v1 rejects any inbound `tx` command at the wire layer (returns 403) — the build is RX-only end to end. Commands accepted by the device server are enqueued onto a thread-safe queue and applied on the UI thread, so SDR / tuner mutation never happens off the GUI thread.

**Config keys**: `predatorRole`, `kujhadDeviceServerEnabled`, `kujhadDeviceListenPort`, `kujhadApiKey` (auto-generated 32-hex on first run), `kujhadDeviceName`, `kujhadAdvertiseAddress`, `kujhadPeers`, `kujhadSpectrumIntervalMs` (50–5000, default 200), `kujhadSpectrumBins` (32–1024, default 256), `kujhadMirrorPeerSpectrum` (default false). All persisted via the existing `core::configManager`.

**Spectrum mirror (Task #4)**: `KujhadDeviceServer` adds `GET /v1/spectrum` — a chunked NDJSON stream of downsampled FFT frames (`{serial,tsMs,centerFreq,bandwidth,fftMinDb,fftMaxDb,bins[]}`). The provider on the device side captures one row per FFT tick from `MainWindow::releaseFFTBuffer` (gated on `kujhadDeviceServerEnabled`) into a thread-owned buffer, then max-buckets it into ~256 bins on demand. Server cadence is bounded (50ms floor, 5000ms ceiling). `KujhadControllerClient` adds `startSpectrum/stopSpectrum/latestSpectrum/spectrumStreaming` plus a dedicated worker that parses the chunked stream with backoff reconnect. The Kujhad tab (Controller side) gains a **View Source** panel with a "Mirror active peer spectrum" toggle; when on, the local waterfall is retuned to the peer's centerFreq/bandwidth, the peer's bins are linearly resampled into the local FFT buffer, and a red **PEER: \<name\>** banner is drawn over the FFT area. Local SDR FFT processing continues in the background so toggling off restores the local view immediately. Bandwidth budget surfaces in the Device tab as "~X kb/s per subscriber".

**Linux web GUI**: instead of replacing the GLFW backend with a web shim (a multi-month effort), v1 ships an additional operator console served by the same HTTP listener at `GET /`. A Linux operator can run Predator RF headless-friendly on a remote box and drive it from any browser at `http://host:41947/`. The native ImGui backend still runs in parallel and remains the full-control surface.

### Auto Marker Detection (`misc_modules/frequency_manager/src/main.cpp`)
Passive always-on FFT analysis layer:
- Noise floor: 20th-percentile via `nth_element` O(N)
- Peak detection: local maxima ≥ configurable SNR threshold with min-separation guard
- Persistence hysteresis: hitCount/missCount (default 4 frames to confirm, 8 to expire)
- Frequency EMA smoothing (70/30)
- Renders cyan/teal markers distinct from yellow manual bookmarks
- UI controls: Enable checkbox, Min SNR slider (5–40 dB), Min Sep (1–500 kHz), Persist Frames slider, live detected count

### Map (`root/res/maps/index.html`)
MapLibre GL JS v4.7.1, OpenFreeMap dark style, 2D/3D toggle, layer toggles (Roads, Road Names, Businesses, Railways), 3D building extrusions, custom compass with live-rotating SVG needle, bearing readout, snap-to-north tap with pulse animation.

## Roadmap

- [x] Android app
- [x] Mission system (Scan / QuickScan / Classify / Manual)
- [x] Hits tab full implementation
- [x] Auto-marker passive detection
- [x] MapLibre 2D/3D map with compass
- [x] Hits/Events export (CSV) — `exportHitsCsv`, `exportEventsCsv` in main_window.cpp; writes to `root/exports/`
- [ ] Audio demod capture pipeline
- [x] Network/topology view — Diablo-style hierarchical Protocol → Network → Talkgroup tree with radio IDs, frequency aggregation, search filter, alias persistence, bulk Target/Exclude/Marker actions, Topology CSV export
- [x] Decoder Bridges scaffold (P25 / RTL433 / POCSAG-FLEX / ADS-B / AIS) — config persisted to `predatorDecoderBridges`; live status indicators in Network tree; protocol→bridge auto-mapping
- [x] RTL433 native ingestion thread (`predator::Rtl433Ingester`) — TCP client / UDP server modes; auto-reconnect with exponential backoff; thread-safe queue drained into `predatorEvents` each frame; live link/status display in Network → Decoder Bridges
- [x] ADS-B native ingestion thread (`predator::AdsbIngester`) — dump1090 / readsb BaseStation port 30003 CSV; aircraft lat/lon surfaced at top level of each row (`aircraftLat`/`aircraftLon`) for tactical-map plotting; live link/status badge under the ADS-B bridge entry
- [x] `LineIngester` base class extracted — shared socket/thread plumbing for all decoder bridges; future POCSAG/AIS/P25 ingesters become ~50-line `parseLine()` overrides
- [x] P25 native ingestion thread (`predator::P25Ingester`) — DSD-FME / OP25 JSON-line; multi-alias keys; Hz/MHz frequency heuristic with 1 MHz–6 GHz sanity range; surfaces WACN/RFSS/Site/TG/Radio + encrypted/algid/keyid; site/system status records retained for control-channel state tracking
- [ ] Native ingestion threads for POCSAG/FLEX and AIS bridges (same `LineIngester` pattern as RTL433/ADS-B/P25)
- [x] **Phase 1: Native rtl_433 module scaffold** (`decoder_modules/rtl433_decoder/`) — vendors rtl_433 24.10 GPL-2.0+ source, strips desktop SDR/HTTP/MQTT layers, hooks SDRPP DSP graph as the sample source, routes decoded events into `predator::DecoderIngestEvent`. Compiles standalone; toggleable from menu; bridge fallback retained
- [x] **Phase 2a: Native decoder registry** (`core/src/predator/native_decoder_registry.{h,cpp}`) — process-wide registry living in sdrpp_core. Native modules call `predator::registerNativeDecoder(this, "RTL433", drainFn)` on construct and `predator::unregisterNativeDecoder(this)` on destruct. `main_window.cpp` calls `drainAllNativeDecoders(64)` each frame and folds returned events into the same `predatorEvents` stream the bridge ingesters feed (tagged `source = "Native:RTL433"`). Hits tab, Network tree, Map, and CSV exporter pick them up automatically. Future native P25 / POCSAG / AIS modules become a one-line registration call
- [ ] Phase 2b: Tune native rtl_433 AM scaling + pulse_detect parameters against real S22 captures
- [x] **Phase 3a: Native DSD-FME module scaffold** (`decoder_modules/dsdfme_decoder/`) — vendors lwvmobile/dsd-fme + szechyjs/mbelib (both GPL-2.0+) for in-APK P25 Phase 1+2 + DMR Tier 1/2/3. Strips PulseAudio/sndfile/ncurses/portaudio/hamlib/rtl-sdr deps via `predator_dsdfme_stubs.c` + ring-buffer bridge (`predator_dsd_bridge.h`); excludes 8 desktop-only TUs (`dsd_ncurses_*`, `dsd_rigctl`, `pa_devs`, `pulse_devices`, `dsd_serial`, `dsd_import`). C++ wrapper (`src/main.cpp`) registers with `predator::registerNativeDecoder("DSDFME")` and feeds the SDRPP DSP graph (CF32 → FM demod → int16 → input ring → `dsd_symbol.c` `audio_in_type==9` Predator branch). Top-level CMake gated by `OPT_BUILD_DSDFME_DECODER`; `android/app/build.gradle` adds the cmake arg + `"dsdfme_decoder"` target
- [~] **Phase 3b: DSD-FME runtime hookup** — `dsd_main.c` and `dsd_file.c` re-added to the build with surgical `#ifndef PREDATOR_BUILD` gates split into FOUR regions (handler/exitflag at line 35, gate A `usage`+`atofs` at 1376–1636, the inner `ncursesOpen()` call at 1670–1672 inside liveScanner, and gate B `cleanupAndExit`+`main` at 1767–3584). `liveScanner()` itself (lines 1641–1766) stays compiled — it is what `predator_dsd_run_decoder_loop()` blocks on. Stubs added for the rigctl rump (`SetModulation`, `SetFreq`) plus a defensive `ncursesOpen`; `SFM_RDWR` macro added to the Predator path of `dsd.h` so `dsd_file.c`'s `sf_open(..., SFM_RDWR, ...)` calls compile (the libsndfile shim returns NULL anyway). New `predator_dsd_init_decoder()` + `predator_dsd_run_decoder_loop()` bridge entry points (in `predator_dsdfme_stubs.c`) call the upstream initializers once, force `audio_in_type=9` / `audio_out_type=0` / `use_ncurses_terminal=0` / `use_rigctl=0`, then block on `liveScanner`. C++ wrapper spawns a decoder worker thread + a voice-pump thread, and chains `dsp::stream<float>` (8 kHz hand-pumped) → `RationalResampler` → `MonoToStereo` → `SinkManager::Stream` registered with `sigpath::sinkManager` so mbelib-synthesized voice (captured by the `pa_simple_write` stub) reaches the operator's headset. SR-change handler simplified to just `setOutSamplerate()` (which already takes `ctrlMtx` + does `tempStop`/`reconfigure`/`tempStart` internally). Architect review pass complete — 2 CRITICAL + 1 HIGH findings fixed. **APK link/runtime validation pending on Windows NDK build.**
- [ ] Phase 4: Native TETRA (osmo-tetra-rx) — metadata + lossy codec2-mapped voice attempt (user accepts quality tradeoff)
- [x] Web operator preview (`/preview` route) — interactive HTML mockup of all 6 tabs in Diablo-tactical aesthetic for non-Android viewers
- [x] Android touch ergonomics — `style::applyTouchFriendlyTweaks()` runs after `ScaleAllSizes(uiScale)` in `core/backends/android/backend.cpp::doPartialInit()` and on every live `thememenu::applyTheme()`. Gate is `backend::isTouchPrimary()` (true on Android, false on GLFW) so phones get thumb-sized hit targets at every scale including 100% and 125%, while desktop keeps its slim default at 1.0x. Floors: scrollbar 32xscale, grab 32xscale, borders 1px+, item spacing 6xscale, FramePadding.y 6xscale (controls collapsing-header / checkbox / combo height), IndentSpacing 18xscale, TouchExtraPadding 4xscale
- [~] **Task #2: Native UI scaling for cellphones** — Android default uiScale is no longer hardcoded `3.0f`. Flow: `defConfig["uiScale"] = "auto"` (string sentinel) -> `core::init()` calls `style::computeAutoScale()` -> `backend::getNativeUiScale()` (Android JNI `MainActivity.getDisplayDensity()` returning `DisplayMetrics.density`; desktop GLFW returns `1.0f`) -> `style::snapToSupportedScale()` clamps to `[1.0, 4.0]` and snaps to one of 11 steps (1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.5, 4.0). Display menu combo expanded from 4 entries to "Auto (device)" + 11 percentages, picks combo entry from the stored preference so "Auto" stays highlighted across launches even when it resolved to 3.0. Combo onChange writes config and calls `thememenu::applyTheme()` for live update; "Restart required." appears only when `|uiScale - style::loadedFontScale| > 0.05`. Legacy float configs still load via `is_string()` / `is_number()` branching. Audit of `widgets/menu.cpp` confirmed all hand-rolled offsets already multiply by `style::uiScale`. APK validation pending on Windows NDK build.
- [x] Android APK build documented (`docs/android_build.md`) — NDK 23.2.8568313 + sdr-kit setup, Gradle steps, S22 sideload, troubleshooting, optional rebranding
- [ ] Linux build
- [ ] Windows build
- [ ] Remote SDR ecosystem

## Backend operational gap closure (Tier 1+2+3, 15 items)

This commit lands the operator-facing pieces that turn the existing 76-test
fusion engine into a fieldable backend. All new behavior is opt-in via env
flags so the original 76 tests stay green.

### New modules
- `backend/operator/missions.py` — MissionRegistry. Operator marks start/end
  of a SIGINT mission; events/tracks/assessments tagged with `mission_id`.
  Active mission survives a restart (replayed from `missions` table).
- `backend/operator/approvals.py` — ApprovalQueue. Two-key gate for CoT
  pushes when `COT_REQUIRE_MANUAL_APPROVAL=1`. Back-pressure (drop oldest),
  TTL expiry, sync+async on_approved callbacks.
- `backend/operator/overrides.py` — OverrideRegistry. Friendly-emitter list,
  freq blacklist (drops at ingest), manual-location override (replaces TDOA
  estimate when operator has DF gear they trust more).
- `backend/observability/metrics.py` — Prometheus text-format registry,
  pure stdlib. Counters + gauges with label maps.
- `backend/observability/logging.py` — structured JSON logger + idempotent
  `configure_logging(fmt="text"|"json")`.
- `backend/api/middleware/auth.py` — bearer-token gate for `/api/v1/*`.
  No-op when `API_BEARER_TOKEN` is unset (lab posture, logged loudly at
  boot). Supports `?token=` fallback for SSE.
- `backend/fusion/cross_station_dedup.py` — coalesces tracks for the same
  physical emitter heard by both local fleet and CoC peers (only merges
  when origins differ; freq + location tolerances).
- `backend/api/routes/{health,missions,approvals,overrides}.py` — REST
  surface for the new subsystems plus `/healthz`, `/readyz`, `/metrics`.

### Edits to existing files
- `backend/persistence/store.py` — schema v1→v2: `missions` table,
  `op_friendly` / `op_blacklist` / `op_manual_location` / `op_approvals_log`
  tables, `mission_id` + `gps_age_s` + `upstream_source` columns on
  `rf_events` / `emitter_tracks` / `assessment_reports`. Forward-only,
  idempotent, gated on `PRAGMA user_version`.
- `backend/coordination/kujhad_client.py` — new `_poll_timing()` for
  `/v1/timing` (NTP/GPSDO offset, PPS lock, drift); recomputes
  `timing_stability_trust` from device-measured state instead of guessing
  from the hardware code. Stamps `location_gps_updated_ns` on each GPS
  poll.
- `backend/coordination/auto_tasker.py` — global per-minute fleet budget
  brake (`AUTO_TASKER_GLOBAL_MAX_PER_MIN`) so an assessment loop bug can't
  thrash every node simultaneously.
- `backend/fusion/tdoa_coordinator.py` — stale-GPS guard
  (`GPS_MAX_AGE_S` drops nodes whose lock is older); adds error ellipse
  (semi-major / semi-minor / theta) derived from confidence + node-cluster
  geometry so the operator UI can render uncertainty.
- `backend/models/sensor_node.py` — new fields:
  `location_gps_updated_ns`, `timing_source`, `timing_pps_lock`,
  `timing_offset_ms`, `timing_last_sync_ns`.
- `backend/main.py` — wires MissionRegistry → store mission_id provider,
  blacklist gate at `_on_rf_event` ingest, friendly-list short-circuit on
  CoT escalation, manual-approval queue gate, dedup loop (CoC mode only),
  approval-expiry loop. Replaces `logging.basicConfig` with
  `configure_logging` so JSON logs come online with `LOG_FORMAT=json`.
- `backend/api/server.py` — mounts auth middleware (gated on token),
  injects backend ref into health/missions/approvals/overrides routes.
- `backend/config.py` — 14 new env flags
  (`API_BEARER_TOKEN`, `LOG_FORMAT`, `COT_REQUIRE_MANUAL_APPROVAL`,
  `COT_APPROVAL_EXPIRY_S`, `AUTO_TASKER_GLOBAL_MAX_PER_MIN`,
  `GPS_MAX_AGE_S`, `TIMING_POLL_INTERVAL_S`, `COC_DEDUP_*`, etc.).

### Tests
9 new test modules, 134 total passing (3 skipped: `aiohttp`-required
loopback tests + `numpy`-required DSP smoke).

## Mission-readiness deploy pack (post-Tier-1+2+3)

`deploy/` and `docs/` now hold everything needed to field the backend
on a Raspberry Pi without a separate build step:

* `deploy/predator-rf.service` — hardened systemd unit (non-root,
  ProtectSystem, MemoryMax, journald) with restart-on-failure
* `deploy/predator-rf.env.example` — every documented env flag
* `deploy/preflight.py` — pure-stdlib readiness check (time sync,
  disk, DB schema v2, token, port-free, RX/TX posture, fleet
  reachability). Importable as `run_all()` for the live HTTP route.
* `deploy/install_rpi.sh` — one-shot installer
* `deploy/backup_mission.sh` — `VACUUM INTO`-based online snapshot
* `docs/OPERATOR_RUNBOOK.md` + `docs/MISSION_READY_CHECKLIST.md`
* `backend/api/routes/preflight.py` — `GET /api/v1/preflight`

## Tier 4 — Android/Windows client integration hooks

Backend-side hooks the user's separately-built Android APK consumes
(C++ Android build is on Windows; this repl ships only the contracts):

* `backend/api/routes/android_pull.py` — `GET /api/v1/android-pull`
  delta-sync snapshot tuned for a phone on a flaky link. Cursor-based
  (strict `>` so cursor=server-now never re-includes), gracefully
  degrades when any subsystem (store/missions/approvals/fleet) is
  missing. Caches preflight result for 30 s.
* `backend/api/routes/cot_export.py` — `GET /api/v1/cot/export`
  XML pull for ATAK plugins on networks where multicast is dropped.
  Bulk wraps in `<events>`, single returns bare `<event>`. Honors
  `assessment.escalate_to_atak` for bulk; single-track form bypasses
  (explicit operator pull).
* `backend/persistence/store.py` — added `fetch_events_since()` and
  `latest_assessments()` (the new routes' read paths).
* `docs/ATAK_COT_FORMAT.md` — frozen v2.0.0 CoT XML schema, with an
  explicit UDP-vs-HTTP delta table.
* `docs/ANDROID_INTEGRATION.md` — full endpoint contract from the
  phone's perspective, schema versioning rules, and a known-issues
  list (no idempotency keys, no WebSocket, TLS out of scope, etc.).
* `docs/SIDELOAD_README.md` — Galaxy S22 sideload walkthrough using
  `assembleDebug` (the sample gradle leaves `release` unsigned by
  design, so debug-signed is the documented happy path).
* `android/sample/{build.gradle.kts, CMakeLists.txt,
  AndroidManifest.snippet.xml, local.properties.example}` —
  reference-only configs for the Predator-RF.git Android repo.

### Tests
174 total passing (3 skipped: `aiohttp` loopback + `numpy` DSP
smoke). New modules: `test_preflight`, `test_android_pull`,
`test_store_tier4`.
