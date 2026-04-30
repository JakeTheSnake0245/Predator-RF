<p align="center">
  <img src="root/res/icons/sdrpp.png" alt="Predator RF" width="160"/>
</p>

# Predator RF

**A mission-focused software-defined radio platform for Android, Linux, Windows, and macOS.**

Built on top of the open-source [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) core, Predator RF replaces the traditional radio-hobby interface with a clean, operator-oriented UI designed for real-world spectrum work — automated frequency monitoring, signal classification, target tracking, and ATAK integration.

> **Status:** Active development. Android is the primary target.

---

## Table of Contents

- [What's New](#whats-new)
- [Features](#features)
- [Supported Hardware](#supported-hardware)
- [Installation — Android](#installation--android)
- [Building from Source](#building-from-source)
- [User Guide](#user-guide)
  - [Screen Layout](#screen-layout)
  - [Spectrum Tab (SPEC)](#spectrum-tab-spec)
  - [Hits & Events Tab (HITS)](#hits--events-tab-hits)
  - [Network Tab (NET)](#network-tab-net)
  - [Map Tab (MAP)](#map-tab-map)
  - [Mission Tab (MIS)](#mission-tab-mis)
  - [Kujhad Fleet Tab (KUJ)](#kujhad-fleet-tab-kuj)
  - [System Tab (SYS)](#system-tab-sys)
- [ATAK / TAK CoT Integration](#atak--tak-cot-integration)
- [Kujhad Fleet Console](#kujhad-fleet-console)
- [Decoder Modules](#decoder-modules)
- [Third-Party Licenses](#third-party-licenses)
- [Contributing](#contributing)

---

## What's New

### v1.2.0
- **ATAK CoT integration** — sends GeoChat hit alerts and SA beacons to any TAK endpoint when a targeted frequency is detected; device appears on the ATAK map as a friendly unit at the phone's GPS fix
- **Right rail fully visible on all phone sizes** — deterministic layout, NoScrollbar, adaptive slider heights
- **Spectrum no longer freezes during scanning** — debounced event disk writes prevent I/O from blocking the render thread
- **Confirmation dialogs** before clearing hits or events — no more accidental data loss
- **Keyboard-safe text editing popup** — naming a hit opens a modal anchored above the Android keyboard so you can see what you're typing

### v1.1.0
- Live spectrum scanning without freezing on signal detection
- Hit and target frequency marker overlays on FFT and waterfall
- VFO markers that track signal frequencies during scanner retunes
- Touch-scroll fix in overlay panels
- Enhanced Mission tab with frequency display and improved delete buttons
- Resolved keyboard overlap issue on Android

---

## Features

### Signal Detection & Recording
- **Continuous FFT peak detection** — identifies peaks above a configurable SNR threshold every render frame
- **Hit recording** — each confirmed detection is saved with frequency, strength (dBFS), SNR, decoder output, label, notes, and GPS-stamped timestamp
- **Duplicate suppression** — configurable window (default 20 s) prevents the same frequency spamming the hit list
- **Strong-hit extended dwell** — stays on a strong signal instead of stepping away mid-transmission
- **State classification** — tag each hit as Target, Exclude, Unknown, or Archived

### Operating Modes
- **Manual** — direct operator tuning; all detector resources are under operator control
- **Classify** — manual control while background watchers check idle spectrum segments
- **Scan** — automated stepping across configured search bands with per-band dwell times
- **QuickScan** — rapid single-pass sweep across all enabled bands for situational awareness

### Spectrum Display
- Full waterfall + FFT display inherited from SDR++
- Hit, target, and exclude markers overlaid on the live spectrum (yellow / green / red)
- Search band shading directly on the waterfall
- Right-rail sliders for Zoom, FFT Max, and FFT Min — always accessible without opening a menu
- Peer spectrum mirroring via Kujhad (cyan dashed overlays distinguish peer markers from local ones)

### Target Management
- Named **target frequency slots** — scanner prioritises these; hits here are flagged as "target"
- **Search bands** — define start/stop frequency ranges for automated scanning
- **Exclude bands** — mark frequencies the scanner skips entirely
- All lists persist across sessions in the app directory

### ATAK / TAK Integration
- **CoT GeoChat** messages sent to any TAK endpoint on every new hit
- **SA beacons** (type `a-f-G-U-C`) broadcast on a configurable interval — shows as a friendly unit on the ATAK map
- GPS coordinates from the phone embedded in every SA beacon and hit report
- Supports UDP multicast (ATAK LAN), UDP unicast, and direct TCP to a TAK Server
- User-selectable callsign and chat room
- Sensor mode: Predator RF appears as a dedicated sensor entity separate from your operator unit

### Kujhad Fleet Console
- Peer-to-peer spectrum sharing over LAN or VPN
- **Device role** — publishes live spectrum, hits, mission config, and GPS position to controller peers
- **Controller role** — mirrors a peer's live waterfall; can push mission commands (retune, update targets/bands/excludes, start/stop scan)
- Per-peer API key authentication
- Optional TLS with SHA-256 certificate pinning
- Controller sees peer markers in cyan; local markers remain yellow/green

### Decoder Integration
- **RTL-433** — hundreds of ISM-band sensors (weather, temperature, humidity, power meters, garage openers)
- **DSD-FME** — P25 Phase 1 & 2, DMR, D-STAR, NXDN digital voice
- **ADS-B** — aircraft transponders at 1090 MHz
- **M17** — M17 amateur digital voice

### Android-Specific
- GPS-driven SA beacons and hit location tagging
- Touch-first ergonomics: large tap targets, finger-sized scrollbars, immersive fullscreen
- USB OTG SDR attachment via Android USB host API
- Persistent app directory — config, hits, events, and mission lists survive restarts and updates

---

## Supported Hardware

Any hardware supported by SDR++ is compatible. Tested on Android:

| Device | Notes |
|---|---|
| RTL-SDR (RTL2832U) | All variants — R820T, R820T2, V3, V4, Blog |
| HackRF One | RX mode only in Predator RF |
| Airspy R2 / Mini | |
| Airspy HF+ Discovery | |
| SDRplay RSPdx, RSP1A | |
| PlutoSDR (ADALM-PLUTO) | Via SpyServer on LAN |
| KiwiSDR | Via network source |
| SpyServer | Remote spectrum via TCP |

---

## Installation — Android

### Requirements
- Android 9 (API 28) or newer
- USB OTG cable for directly attached SDRs, **or** a network SDR source (SpyServer, KiwiSDR, RTL-TCP)
- "Install from unknown sources" enabled in Android settings

### Sideload Steps
1. Download `app-debug.apk` from the [Releases page](https://github.com/JakeTheSnake0245/Predator-SDR/releases)
2. Transfer the APK to your device (USB, ADB, cloud, etc.)
3. Tap the APK and confirm installation when prompted
4. Launch **Predator RF** from the app drawer
5. Grant location permission when asked (required for GPS SA beacons)
6. Connect your SDR via USB OTG or configure a network source in the **System** tab

### ADB Sideload
```bash
adb install -r app-debug.apk
```

---

## Building from Source

### Prerequisites
- Android NDK r23.2 (set path in `android/local.properties`)
- Android SDK 33+
- CMake 3.21+
- Gradle 8+

### Android Debug APK
```bash
git clone https://github.com/JakeTheSnake0245/Predator-SDR.git
cd Predator-SDR/android
./gradlew assembleDebug
# Output: android/app/build/outputs/apk/debug/app-debug.apk
```

### Desktop (Linux / Windows / macOS)
```bash
mkdir build && cd build
cmake .. -DOPT_BUILD_PREDATOR_MODULES=ON
make -j$(nproc)
```

Refer to the upstream [SDR++ build guide](https://github.com/AlexandreRouma/SDRPlusPlus#building) for dependency details.

---

## User Guide

### Screen Layout

```
┌──────────────────────────────────────────────────────┬────────┐
│ Status bar  [LIVE]  [Source]  [Mode]  [GPS READY]    │        │
├──────────────────────────────────────────────────────┤  SPEC  │
│ Control bar  — frequency selector, tuning mode        │  HITS  │
├──────────────────────────────────────────────────────┤  NET   │
│                                                       │  MAP   │
│              Spectrum + Waterfall                     │  MIS   │
│         (hit/target/exclude markers overlaid)         │  KUJ   │
│                                                       │  SYS   │
│  ┌────────────────────────────────────────────────┐  │        │
│  │  Overlay panel (opens when a rail tab is       │  │  Zoom  │
│  │  tapped; closes on second tap of same tab)     │  │  Max   │
│  └────────────────────────────────────────────────┘  │  Min   │
└──────────────────────────────────────────────────────┴────────┘
```

The **right rail** (seven tab buttons + Zoom/Max/Min sliders) is always visible. Tapping a tab button opens its overlay panel on top of the spectrum; tapping the active tab again closes it.

The **status bar** shows:
- `LIVE` / `READY` / `NOT READY` — tap to start/stop the SDR
- Source name — tap to jump to System → SDR selection
- Mission mode selector
- `GPS READY` / `GPS WAIT` — tap to jump to the Map tab

---

### Spectrum Tab (SPEC)

The spectrum is always visible in the background. The SPEC overlay adds:

- **Peak Detection toggle** — enables/disables automatic hit recording
- Threshold controls for SNR and minimum peak spacing

The right-rail **Zoom**, **Max**, and **Min** vertical sliders let you adjust the view without opening any overlay.

**Overlays on the waterfall/FFT:**
| Marker | Colour | Meaning |
|---|---|---|
| Vertical line | Yellow | Recorded hit |
| Tick at top | Green | Configured target frequency |
| Band shading | Blue-tinted | Active search band |
| Band shading | Red-tinted | Exclude band |
| Vertical line (dashed, cyan) | Cyan | Peer hit (Kujhad mirror mode) |

---

### Hits & Events Tab (HITS)

**Hits list** — all recorded signal detections, most recent first.

Each row shows: frequency, strength, label (if named), decoder, and hit count. Tap a row to expand the **Hit Detail** view:

- Frequency, bandwidth, strength bar, SNR histogram
- **Rename / Notes** — opens a popup above the keyboard where you can type a label for this frequency
- **Tune** — retunes the SDR to this frequency immediately
- **Assign Marker / Release Marker** — routes a VFO slot to permanently track this frequency
- **Route VFO / Release Route** — same as marker assignment
- **Promote Target** — adds the frequency to the Mission target list
- State buttons: **Target**, **Exclude**, **Unknown**, **Archive**

**Clear All Hits** requires a confirmation dialog to prevent accidental deletion.

**Events list** (scroll down or switch sub-tab) — chronological log including decoder events, manual log entries, and ADS-B / RTL-433 ingestion. Filter by state. **Clear Events** also requires confirmation.

**Sort modes:** Recent | Frequency | Strength | Hit Count | State

---

### Network Tab (NET)

Displays structured metadata from decoder-bridged networks:

- **P25 / DMR** traffic from DSD-FME: talkgroup ID, radio ID, network, alias
- **ADS-B** from the 1090 MHz decoder: ICAO, callsign, altitude, squawk
- **RTL-433** sensor readings: temperature, humidity, power usage, etc.

Use the **filter box** at the top to search across all fields. Tap a row to see the full metadata card and set an alias for a radio ID or talkgroup.

**Bridge configuration** (scroll down in the panel):
- Add RTL-433 / DSD-FME bridge hosts so Predator RF can pull decoded output from processes running on the same or a networked device
- P25 / DMR bridge: set the decoder host and port, choose which VFO feeds the decoder

---

### Map Tab (MAP)

Launches the native Android map tied to the phone GPS. Shows:
- Your current position
- Hit locations where GPS was active during detection
- Target frequency labels

Tap **Open Map** to launch the map activity. GPS status is visible in the status bar badge.

---

### Mission Tab (MIS)

Drives automated scanning and target management.

#### Mission Modes
Select the mode in the **top status bar** combo:

| Mode | Behaviour |
|---|---|
| Manual | Operator tunes freely; scanner inactive |
| Classify | Manual + background watcher on idle frequencies |
| Scan | Automated stepping across search bands, dwells on active signals |
| QuickScan | Single rapid sweep across all enabled bands |

#### Search Bands
Define frequency ranges for the scanner to sweep:

1. Enter a **Band Name**, **Start Hz**, and **Stop Hz**
2. Tap **Add Band**
3. Toggle each band's checkbox to include/exclude it from the current scan

Dwell controls:
- **Dwell time** — how long the scanner monitors an active signal before stepping to the next candidate
- **QuickScan delay** — pause between candidates in rapid-sweep mode
- **QuickScan duration** — maximum time a single rapid sweep runs

#### Targets
Named frequencies the scanner prioritises:
- Add manually via the **Targets** section or tap **Promote Target** in any hit's detail view
- Enable/disable per-entry

#### Excludes
Frequency ranges skipped during scanning:
- Enter a center frequency + bandwidth (or tap **Exclude Current** while tuned there)

#### Mission Run Controls
- **Start Scan / Stop Scan** — begin or stop the automated loop
- **Previous / Next** — manually step one candidate in the scan list
- **Target Current** — immediately adds the current VFO frequency to targets
- **Exclude Current** — immediately adds the current VFO frequency to excludes
- **Log Event** — records a manual hit event at the current frequency

---

### Kujhad Fleet Tab (KUJ)

Multi-operator spectrum sharing. See [Kujhad Fleet Console](#kujhad-fleet-console) for full details.

---

### System Tab (SYS)

**SDR Source** — select your device or network source, configure sample rate, gain, and other hardware settings.

**Display** — UI scale (Auto / 1×–4×), colour theme, FFT window function.

**TAK Integration** — see [ATAK / TAK CoT Integration](#atak--tak-cot-integration).

**Session Export** — export the current hits/events list as JSON.

**Module Manager** — enable or disable optional decoder and utility modules.

**Health** — shows SDR status, build version, and runtime diagnostics.

---

## ATAK / TAK CoT Integration

Predator RF can send **Cursor on Target (CoT)** XML to any TAK-compatible endpoint whenever a hit is recorded, and periodically broadcast a **Situation Awareness (SA)** position update so the device appears as a friendly unit on the ATAK map.

### Quick Setup

1. Open **System** tab → **TAK Integration**
2. Toggle **Enable TAK CoT reporting**
3. Set **Protocol**:
   - **UDP** — fire-and-forget; recommended for LAN multicast and most use cases
   - **TCP** — direct connection to a TAK Server; message delivery is confirmed
4. Set **Host**:
   - `239.2.3.1` — ATAK LAN multicast; all ATAK devices on the same subnet receive it without needing a TAK Server
   - `127.0.0.1` — local device (if running WinTAK or another TAK client on the same phone)
   - TAK Server IP — for centralised distribution
5. Set **Port**:
   - `6969` — ATAK SA multicast (UDP)
   - `4242` — ATAK direct UDP
   - `8087` — TAK Server TLS (TCP)
   - `8088` — TAK Server plain TCP
6. Set your **Callsign** and **Chat Room** (default: "All Chat Rooms")
7. Enable **Sensor mode** if you want Predator RF to appear as a dedicated sensor icon on the map (recommended)
8. Set **SA interval** — how often your position beacon is broadcast (5–300 s, default 30 s)
9. Tap **Send Test Message** to send a synthetic hit to the configured endpoint and verify reception in ATAK

### Message Formats

**SA Beacon** (`a-f-G-U-C`, friendly ground unit):
- Sent every *SA interval* seconds when Sensor mode is enabled
- Contains: callsign, unique UID, GPS lat/lon, and circular error (CE) from the phone's location fix
- Appears as a blue diamond on the ATAK 2D/3D map

**Hit Alert** (`b-t-f`, GeoChat):
```
[Predator RF] HIT: 154.5750 MHz | -62.3 dB | SNR: 14.2 dB | #3 | target | Fire Dispatch
```
- Sent to the configured chat room whenever a new, non-suppressed hit is recorded
- Fields: formatted frequency, signal strength (dBFS), SNR, running hit count, hit state, label (if the frequency has been named)

### Sensor Mode vs. Operator Mode

| Setting | Sensor Mode (recommended) | Operator Mode |
|---|---|---|
| SA entity type | Dedicated "Predator RF" sensor with its own UID | Uses your callsign UID |
| SA beacons | Yes — device appears independently on the map | No — use your existing ATAK SA |
| Message sender | Predator RF sensor UID | Your callsign |
| Use when | Running as a dedicated monitoring asset alongside your normal ATAK presence | You want hits attributed directly to your operator callsign |

### Endpoint Reference

| Scenario | Host | Port | Protocol |
|---|---|---|---|
| ATAK on same LAN (no server) | `239.2.3.1` | `6969` | UDP |
| ATAK on same device | `127.0.0.1` | `4242` | UDP |
| TAK Server (unencrypted) | Server IP | `8088` | TCP |
| TAK Server (TLS) | Server IP | `8087` | TCP |
| WinTAK on same network | WinTAK machine IP | `4242` | UDP |

---

## Kujhad Fleet Console

Kujhad enables multiple Predator RF instances to share spectrum and coordinate missions across a local network or VPN.

### Concepts

**Device** — the SDR-attached unit collecting live spectrum data. Runs a local server that exposes:
- Live spectrum frames (downsampled to a configurable bin count, default 256 bins)
- All current hits, targets, search bands, excludes
- Real-time GPS position
- Mission state (mode, scan running, dwell settings)

**Controller** — a phone or tablet without an attached SDR. Pulls data from one or more devices and can:
- Mirror a device's live spectrum on its own waterfall (peer markers shown in cyan dashes)
- Push mission commands: retune, start/stop scan, update targets / search bands / excludes
- Monitor multiple devices simultaneously and switch the mirrored view between them

### Device Setup

1. Open **Kujhad Fleet** tab → set **Role** to **Device**
2. Enter a **Device name** (human-readable identifier for controllers)
3. Enter an **API key** — a shared secret that all authorised controllers must know; use a random string of at least 16 characters
4. Set **Listen port** (default: 41947; must be reachable by controllers)
5. (Optional) Enable **TLS**:
   - Provide paths to a PEM certificate and private key on the device's filesystem
   - The SHA-256 fingerprint is displayed in the UI — share it with controllers for pinning
6. Toggle **Enable Kujhad server**
7. Share the device's IP address (or hostname), port, and API key with your controllers

### Controller Setup

1. Open **Kujhad Fleet** tab → set **Role** to **Controller**
2. Scroll to **Add Peer** and fill in:
   - **Name** — label for this device in your fleet list
   - **Host** — IP address or hostname of the device
   - **Port** — the device's listen port (default: 41947)
   - **API Key** — the shared secret
   - **Pinned fingerprint** (optional but recommended) — paste the device's TLS certificate fingerprint to prevent MITM attacks
3. The peer appears in the fleet list with a live connection status
4. Tap **Take Control** to start mirroring that device's spectrum on your local waterfall
5. Tap **Take Control** again or tap **Release** to stop mirroring and restore your local view

### Mirroring Behaviour

When you take control of a device:
- Your waterfall recentres on the device's current frequency and bandwidth
- Incoming spectrum frames overwrite local SDR data — the waterfall shows only the peer's view
- Hit / target / search band / exclude markers from the peer are painted in **cyan** with dashed vertical lines so they are visually distinct from your own (yellow/green) markers
- The status bar shows a banner identifying the mirrored device
- Releasing mirror control restores your previous center frequency, bandwidth, and view

### Security

| Threat | Mitigation |
|---|---|
| Unauthorised access | Per-connection API key required |
| Key interception (cleartext) | Enable TLS on the device |
| MITM against TLS | Paste the SHA-256 fingerprint on the controller to pin the certificate |

The device server refuses non-loopback connections over plain HTTP when the API key has been set — the key is never sent unless TLS is active or the connection is loopback.

---

## Decoder Modules

| Module | Protocol(s) | Typical Frequencies |
|---|---|---|
| RTL-433 | OOK / FSK ISM-band sensors (300+ device types) | 315 / 433 / 868 / 915 MHz |
| DSD-FME | P25 Phase 1 & 2, DMR, D-STAR, NXDN | VHF/UHF public safety |
| ADS-B | Mode S aircraft transponders | 1090 MHz |
| M17 | M17 open amateur digital voice | Any VHF/UHF |

Decoders attach to VFO slots configured in the **Network** tab. Each decoder can optionally inject its decoded metadata directly into the Predator RF hits and events lists.

---

## Third-Party Licenses

Predator RF is licensed under **GPLv3**. See [LICENSE](LICENSE).

All incorporated projects retain their original licenses:

| Project | License | Role in Predator RF |
|---|---|---|
| [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) | GPLv3 | Core DSP engine, waterfall, module system, source/sink architecture |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | UI rendering |
| [FFTW3](https://www.fftw.org/) | GPLv2+ | FFT computation |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | Configuration and data serialisation |
| [rtl_433](https://github.com/merbanan/rtl_433) | GPLv2 | ISM-band sensor decoder |
| [DSD-FME](https://github.com/lwvmobile/dsd-fme) | GPLv2 | Digital voice (P25, DMR, D-STAR, NXDN) decoder |
| [MBElib](https://github.com/szechyjs/mbelib) | Non-commercial research | IMBE/AMBE voice codec (required by DSD-FME) |
| [libcorrect](https://github.com/quiet/libcorrect) | BSD 3-Clause | Reed-Solomon / Convolutional FEC |
| [Volk](https://github.com/gnuradio/volk) | LGPLv3 | SIMD-accelerated DSP kernels |
| Roboto font (Google) | Apache 2.0 | UI typography |

> **MBElib note:** MBElib carries a non-commercial research restriction. Using DSD-FME / MBElib in a commercial product without a separate patent licence from the IMBE/AMBE codec patent holders may require additional rights. Consult your legal counsel if unsure.

---

## Roadmap

- [x] Android app
- [x] Automated frequency scanning
- [x] ATAK / TAK CoT integration
- [x] Kujhad Fleet Console (multi-operator spectrum sharing)
- [x] RTL-433 / ADS-B / DSD-FME / M17 decoder integration
- [ ] Linux desktop build
- [ ] Windows desktop build
- [ ] ADS-B map overlay in the Map tab
- [ ] Scheduled scan plans (time-gated search bands)
- [ ] Signal fingerprinting / signature matching

---

## Contributing

Issues, ideas, and pull requests are welcome. This is a passion project — reviews may take time, but all constructive input is appreciated.

- **Bug reports:** include Android version, device model, SDR hardware, and steps to reproduce
- **Feature requests:** open an issue describing the use case and the problem it solves
- **Pull requests:** one logical change per PR; target the `main` branch

---

*Predator RF is an independent project. It is not affiliated with SDR++, Athena Technology, or any government or military organisation.*
