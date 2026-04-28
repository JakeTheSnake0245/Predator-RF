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

- `server.py` — Python HTTP server serving the landing page on port 5000
- `index.html` — Project info/landing page
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
- [ ] Network/topology view
- [ ] Linux build
- [ ] Windows build
- [ ] Remote SDR ecosystem
