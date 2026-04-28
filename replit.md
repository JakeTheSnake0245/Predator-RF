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
- `preview.html` — Interactive HTML mockup of the Predator RF operator interface (Spectrum, Hits, Network Tree, Map, Mission, System tabs) styled in Diablo-tactical dark theme; pure presentation, no backend
- `core/src/predator/decoder_ingest.h` — Receive-only decoder ingestion (header-only). Abstract `predator::LineIngester` base owns the socket/thread/queue plumbing (TCP client + UDP server modes, auto-reconnect with exponential backoff, non-blocking connect with stop-flag polling, bounded queue); per-decoder subclasses override `parseLine()`. Implemented: `Rtl433Ingester` (rtl_433 JSON Lines), `AdsbIngester` (dump1090 / readsb BaseStation port 30003 CSV — extracts ICAO hex, callsign, altitude, lat/lon, squawk; freq pinned to 1090 MHz)
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
- [ ] Native ingestion threads for P25, POCSAG/FLEX, AIS bridges (same `LineIngester` pattern as RTL433/ADS-B)
- [x] Web operator preview (`/preview` route) — interactive HTML mockup of all 6 tabs in Diablo-tactical aesthetic for non-Android viewers
- [x] Android touch ergonomics — `style::applyTouchFriendlyTweaks()` runs after `ScaleAllSizes(uiScale)` in `core/backends/android/backend.cpp::doPartialInit()`. Bumps scrollbar (24×scale), slider grab (22×scale), borders (1px+), frame/grab/scrollbar rounding, item spacing (6×scale), and TouchExtraPadding (4×scale)
- [x] Android APK build documented (`docs/android_build.md`) — NDK 23.2.8568313 + sdr-kit setup, Gradle steps, S22 sideload, troubleshooting, optional rebranding
- [ ] Linux build
- [ ] Windows build
- [ ] Remote SDR ecosystem
