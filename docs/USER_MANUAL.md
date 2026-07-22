# Predator RF — Operator's Manual (Android App)

This manual covers the Predator RF Android application only: what works today,
how to use it, and its current limits. For field procedures see
`docs/OPERATOR_RUNBOOK.md`; for pre-departure checks see
`docs/MISSION_READY_CHECKLIST.md`.

---

## 1. Quick Start

1. **Connect an SDR** (or a remote node — see §9). Select the source in the
   **SYS** tab and press Play.
2. Pick a **mission mode** in the MIS tray: MANUAL, SCAN, QUICKSCAN, or
   CLASSIFY.
3. Configure **Search Bands**, **Targets**, and **Exclude Bands** in MIS.
4. Watch the spectrum. Double-tap to place markers (Manual/Classify);
   Scan/Classify place them automatically on strong hits.
5. Review detections in **HITS**; hold interesting frequencies for persistent
   monitoring/decoding.

---

## 2. Screen Layout & Tabs (right rail)

| Tab | Purpose |
|---|---|
| **SPEC** | Main FFT + waterfall. Display controls, band plan, mission status, A1/A2 arrow readouts. |
| **HITS** | All detections: Quick Filter (All/Target/Exclude/Unknown), Held Frequencies (decoders), Marker Pool, Detected Hits list, Events log. |
| **NET** | Peer node list + network topology view. |
| **MAP** | Tactical map: own GPS position, peer positions, DF lines of bearing. |
| **MIS** | Mission config: search bands, targets, excludes, scan dwell/threshold. Opens as a left slide-out tray. |
| **KUJ** | Kujhad fleet: drive a remote peer's radio, mirror its spectrum and mission. |
| **SYS** | SDR source/device, audio sinks, appearance, global settings. |
| **BASE** | RF baseline recorder + baseline comparison filter. |
| **TX** | Fox Hunt transmit controls. Only appears in the Fox Hunt mission. |

**Status banner colors:** green = local radio, amber = driving a peer,
red = peer link stale. GPS shows `GPS READY` / `GPS WAIT`.

---

## 3. Mission Modes

- **MANUAL** — you tune, you place markers. Nothing automated.
- **SCAN** — steps across every enabled search band, dwelling per step and
  logging hits above the SNR threshold. Markers auto-assign from the pool.
- **QUICKSCAN** — rapid single-marker sweep for a fast look.
- **CLASSIFY** — you keep manual control of tuning while the app watches the
  visible band and auto-places markers on detections (toggleable).

---

## 4. Spectrum Gestures

| Gesture | Action |
|---|---|
| One-finger drag | Pan / tune |
| Pinch | Zoom frequency span |
| **Double-tap** | Place a marker at that frequency (Manual/Classify) |
| **Long-press on a marker** | Marker menu: Name…, Target, Exclude, Adjust, Remove Marker |
| Long-press on empty spectrum | Recenter/tune there |
| Drag FFT/waterfall divider (short hold) | Resize FFT vs waterfall |

**Adjust:** after choosing Adjust from the marker menu, drag left/right to
fine-place the marker; lift to commit.

---

## 5. Markers & Hits

- Marker pool size is configurable (1–16 slots, "Marker Slots" setting); slots
  are labelled M1, M2, …. A marker pins a frequency,
  draws a labelled vertical line on FFT + waterfall, and (in Manual/Classify)
  opens a lightweight channel usable for per-marker IQ recording.
- **Color legend:**
  - **Yellow** — ordinary hit / unknown signal.
  - **Green** — matches a configured Target.
  - **Red wash** — Exclude zone (hits suppressed there).
  - **Cyan, dashed** — a peer's markers mirrored over the network.
- Hits cluster by frequency; each row tracks hit count, max RSSI, and state.
  Row actions: hold (+ Hold), marker assign/release, gear menu for recording
  options, Target/Exclude promotion.
- **Events log** (HITS tab): automated threshold hits, manual "Log Event"
  entries, and decoder output (IDs, talkgroups, sensor data).

---

## 6. Held Frequencies & Decoders

- **+ Hold** on a hit creates a persistent held entry (survives restarts, up
  to 8 concurrent).
- Decoder kinds selectable per hold: Analog voice (AM/NBFM/WBFM/SSB), DSD-FME
  digital voice, ADS-B, RTL-433.
- **Auto-decode today:** only **RTL-433** spawns automatically from a hold.
  DSD-FME (P25), ADS-B, and the analog radio kinds are manual-load for now
  (multi-P25 concurrent decode is on the roadmap).
- DSD-FME is single-instance: one digital-voice decode at a time.

---

## 7. Recording

- **Per-marker raw IQ:** gear icon on a hit row → "Record raw IQ (.wav)";
  files land in auto-created subfolders. Alternatively use the SDR++ Recorder
  module in Baseband mode and select the marker's `Predator M#` channel.
- **Decoded product:** gear icon → save decoded voice/data per hold.
- **Global recorder:** SYS/sinks menu for whole-session audio or baseband.

---

## 8. Baseline (BASE tab)

- **Record** a baseline while moving through an area to profile its ambient
  RF.
- Enable **Baseline Comparison** so Scan/Classify only report signals that
  exceed the recorded environment by your threshold — new emitters stand out,
  static noise disappears.

---

## 9. Networking & Peer Control

### Kujhad fleet (KUJ tab)
- Any node can run a **Device Server** (toggle in KUJ) so peers can view or
  drive it; QR codes speed up pairing. Auth via shared key, optional TLS with
  certificate pinning.
- As **Controller** you mirror a peer's spectrum, markers, and decoders live,
  and can task tune/scan/mission changes. Banner turns amber while driving a
  peer, red if the link goes stale.
- **RX-only posture:** remote transmit commands are hard-rejected at every
  network surface. Nothing on the network can key a transmitter (see Fox Hunt
  exception below — separate opt-in class, still not `tx.*`).

### Remote cockpit (phone as console)
- The `Predator Node` source turns the app into a pure console for a remote
  Pi/PC node over hotspot/Wi-Fi — full native UI, no local SDR needed. Common
  link addresses are auto-probed.

### Off-grid & TAK
- **Reticulum (RNS):** CoT and command tasking over low-bandwidth/IP-denied
  links (identity-hash authenticated).
- **ATAK/TAK CoT:** operator-initiated export; UDP multicast push plus an
  HTTP pull endpoint for when multicast is blocked. Kraken bearings render as
  wedges in TAK.

### Multi-node sensing
- **Custody election:** the fleet auto-nominates the best-placed node to own
  an emitter (SNR, GPS health, geometry).
- **TDOA (Controller mode):** geolocates emitters from peer time-difference
  measurements; nodes without disciplined clocks are confidence-capped.
- **KrakenSDR:** 5-channel DF lines of bearing, one-tap retasking from the
  hits list or map, triangulated fixes on the MAP tab.
- **Correlation engine:** flags when multiple nodes hear the same band and
  cues geolocation on fingerprint matches.

---

## 10. Fox Hunt (TX tab)

- Local-hardware-only transmit for training/fox-hunt exercises: IQ file
  replay, steady tone, or CW beacon (callsign + WPM configurable), manual /
  marker-derived / sweep frequency modes.
- **Remote Fox Hunt** tasking exists but each node must explicitly tick
  "Accept Remote Fox Hunt tasking" — off by default, dead-man timer stops the
  beacon if the controller goes silent.
- Know your license and local regulations before transmitting. CW beacons
  should carry your callsign.

---

## 11. Arrow Markers (A1/A2)

Toggle A1/A2 above the spectrum, tap to drop. Each shows a precise frequency
readout; with both placed you get a Δ-frequency measurement — handy for
channel spacing and offset checks. They're display-only and never affect the
radio.

---

## 12. Troubleshooting

| Symptom | Meaning / fix |
|---|---|
| **STALLED badge** | Sample flow stopped. Check the SDR connection/USB; if it persists, restart the source from SYS. (Marker-induced stalls were fixed — update your build if you still see stall-on-marker.) |
| **GPS WAIT** | No fix yet — check location permission and sky view. Map/DF features need a fix. |
| **Red banner** | Peer link stale — check the network path to the node; control is suspended until it recovers. |
| Marker won't place | Double-tap placement works in Manual/Classify. In Scan, markers auto-assign from hits. |
| No decode from a hold | Only RTL-433 auto-spawns today; other decoders must be loaded manually (§6). |
| Keyboard covers a field | All editable fields use the pop-up editor above the keyboard; if one doesn't, report it — that's a bug. |

---

## 13. Current Limits (honest list)

- One DSD-FME (digital voice) decode at a time; multi-P25 is roadmap.
- Hold auto-decode wired for RTL-433 only.
- TDOA on-device UI is Controller-mode; timing-poor nodes are
  confidence-capped.
- The app is RX-only over the network by design; all remote TX except the
  opt-in Fox Hunt beacon class is rejected.
