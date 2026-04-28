# RF Predator Guide Alignment

This app is being built as a Predator RF operator experience on top of the working SDR++ Android runtime. The production guide also describes a Raspberry Pi service appliance; those service concepts are mapped here to the current Android-native shape so future work stays intentional.

## Wired Now

- Android app identity: `Predator RF` app label, log tag, loading screen, credits, module descriptions, and map label.
- Runtime assets: `root/res` is copied into `android/app/assets/res` and extracted on-device at startup.
- Native SDR core: `libsdrpp_core.so` is loaded by `NativeActivity`, with SDR++ module contracts preserved.
- ARM64 Android package: `arm64-v8a` native libraries are packaged for phone deployment.
- Phone GPS map: `MapActivity` loads `res/maps/index.html`, follows phone GPS, supports recenter/follow controls, and exposes `window.PredatorRFMap`.
- Spectrum-first shell: persistent Predator RF status bar, live/ready badge, selected SDR badge, mission mode selector, GPS badge, spectrum/waterfall viewport, right-side tab rail, and overlay panels.
- Simple mission controls: Manual, Classify, Scan, and QuickScan modes are persisted.
- Mission configuration: search bands, targets, excludes, threshold, dwell, QuickScan delay/duration, and record-audio preference are persisted.
- Hits/events workflow: filter tabs, event persistence, manual event logging, scan target logging, FFT peak hit clustering, marker assignment state, and target/exclude promotion are present.
- Operator hit review: marker pool status, selected-hit detail, unread counts, per-hit event history, tune/mark/view/promote actions, duplicate event suppression, hold-on-new-hit, strong-hit dwell extension, and clear-hit controls are present.
- Spectrum markers: scan-created markers are passive `M#` overlays derived from refined FFT peaks, while manually routed hits can create decoder-ready Predator VFO routes without stealing the receiver VFO during scanning.
- Pre-demod usability: hit sorting, event filtering, hit rename/notes, scan progress, session notes, session JSON export, and basic Classify auto-marker assignment are present.
- Decoder workflow shell: DSD-FME bridge settings, per-hit decoder selection, and separate voice/data extraction folders are persisted while the original SDR++ recorder folder flow remains unchanged.
- Network shell: hierarchical Protocol → Network → Talkgroup tree (Diablo Network Tree style) with radio ID and frequency aggregation, search filter, alias persistence, bulk Target/Exclude/Marker actions on a selected node, and Topology CSV export to `root/exports/`.
- Decoder Bridges shell: receive-only configuration scaffolding for P25 (Phase 1+2), RTL433 ISM, POCSAG/FLEX paging, ADS-B aircraft, and AIS marine. Host/port/mode/notes persisted to `predatorDecoderBridges`; live status indicators in the Network tree mark which protocols have an active bridge feeding them. Native ingestion threads for each bridge are the next scaffolded-but-not-complete step.
- DF gate: DF is explicitly shown as unavailable instead of being implied or faked.
- Safe receive scope: transmit, jamming, effects, and offensive countermeasure workflows are not implemented.

## Android Module Set

The Android build currently packages and registers:

- `airspy_source`
- `airspyhf_source`
- `file_source`
- `hackrf_source`
- `hermes_source`
- `plutosdr_source`
- `rfspace_source`
- `rtl_sdr_source`
- `rtl_tcp_source`
- `sdrpp_server_source`
- `spyserver_source`
- `audio_sink`
- `network_sink`
- `m17_decoder`
- `meteor_demodulator`
- `radio`
- `frequency_manager`
- `recorder`
- `rigctl_server`
- `scanner`

The Android default config intentionally does not instantiate unsupported modules such as `audio_source`, `bladerf_source`, `limesdr_source`, `perseus_source`, `sdrplay_source`, or `soapy_source`.

## Scaffolded But Not Complete

- Decoder-confirmed hit clustering beyond FFT energy hits.
- Audio clip lifecycle tied to events.
- Live audio routing into DSD-FME and decoder-normalized event ingestion.
- Decoder-confirmed network node actions beyond event-derived topology actions.
- Automatic demodulator routing for assigned markers.
- Session export archive with database/audio records.
- External integration paths such as CoT/ATAK publishing.
- FHOP waveform library, match/identify workflow, and emitter cards.

## Service Topology Mapping

The production guide's Pi services are not literal Android services in this branch:

- `rfpredator-backend.service`: currently represented by the in-process SDR++ native core and Android activity lifecycle.
- `rfpredator-worker.service`: currently represented by SDR++ source modules, DSP path, waterfall, and module threads inside the native process.
- `rfpredator-watchdog.service`: not present yet.
- `rfpredator-maptiles.service`: currently represented by the bundled WebView map asset and online tile URL; offline tiles are not present yet.
- `rfpredator.target`: not applicable to Android packaging.

If the Pi appliance build resumes later, these should be implemented as separate systemd-backed services rather than pushed into the Android app.

## Safety Boundary

Predator RF remains receive, analyze, log, map, and export oriented. Diablo manual sections about transmit modes, effects, countermeasure workflows, jamming, or offensive radio behavior are intentionally out of scope for this app.
