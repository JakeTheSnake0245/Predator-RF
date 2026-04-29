# Predator SDR

## Overview

Predator SDR is a fork of [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus), a high-performance Software Defined Radio application. This project aims to provide a cleaner, more mission-focused interface for working in the electromagnetic environment (EME).

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
- `decoder_modules/rtl433_decoder/` — **Native rtl_433 ISM decoder module (Phase 1 scaffold).** Vendors rtl_433 24.10 (GPL-2.0-or-later) under `vendor/rtl_433/` minus its desktop SDR / mongoose / HTTP / MQTT / InfluxDB / GPSD layers. `src/predator_stubs.c` provides no-op symbol stubs for the excluded layers. `src/main.cpp` registers as a SDRPP module: creates `r_cfg_t`, registers all 235 protocols, attaches a custom `data_output_t` whose `output_print` callback converts each decoded `data_t` into a `predator::DecoderIngestEvent` and queues it. DSP path: VFO at 250 kHz BW → handler sink → CF32→AM-envelope (int16) + CS16→FM (int16) → `pulse_detect_package` → `run_ook_demods`/`run_fsk_demods` on `cfg->demod->r_devs`. Toggleable from the menu; existing `Rtl433Ingester` bridge stays as a fallback. Wired via `OPT_BUILD_RTL433_DECODER` in `CMakeLists.txt` and enabled in `android/app/build.gradle`.
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

**Config keys**: `predatorRole`, `kujhadDeviceServerEnabled`, `kujhadDeviceListenPort`, `kujhadApiKey` (auto-generated 32-hex on first run), `kujhadDeviceName`, `kujhadAdvertiseAddress`, `kujhadPeers`. All persisted via the existing `core::configManager`.

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
- [ ] Phase 2: Native rtl_433 — wire decoded events into `main_window.cpp` per-frame topology drain; tune AM scaling + pulse_detect against real S22 captures
- [ ] Phase 3: Native P25 (DSD-FME + mbelib + codec2) — voice + metadata; user accepted mbelib/AMBE patent risk for personal/hobby use
- [x] Web operator preview (`/preview` route) — interactive HTML mockup of all 6 tabs in Diablo-tactical aesthetic for non-Android viewers
- [x] Android touch ergonomics — `style::applyTouchFriendlyTweaks()` runs after `ScaleAllSizes(uiScale)` in `core/backends/android/backend.cpp::doPartialInit()`. Bumps scrollbar (24×scale), slider grab (22×scale), borders (1px+), frame/grab/scrollbar rounding, item spacing (6×scale), and TouchExtraPadding (4×scale)
- [x] Android APK build documented (`docs/android_build.md`) — NDK 23.2.8568313 + sdr-kit setup, Gradle steps, S22 sideload, troubleshooting, optional rebranding
- [ ] Linux build
- [ ] Windows build
- [ ] Remote SDR ecosystem
