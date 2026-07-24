# KrakenSDR LOB Integration

Predator RF integrates KrakenSDR direction-finding hardware as a
first-class location source via LOB (Line of Bearing) triangulation.

## Architecture

```
KrakenSDR hardware (5-channel coherent SDR)
  └─ daq_fw daemon            (port 5000, on the KrakenSDR RPi)
       └─ krakensdr_doa       (port 8082, WebSocket /ws)
            └─ kraken_lob_decoder SDRPP module
                 └─ DecoderIngestEvent {decoder="KRAKEN_LOB"}
                      └─ Predator bridge → backend/fusion/lob_triangulator.py
                           └─ TrackManager.ingest_lob()
                                └─ EmitterTrack.lob_crosscut_* fields
                                     ├─ /tracks/ API (LOB fields in JSON)
                                     ├─ Map: bearing wedges + crosscut circles
                                     └─ CoT: <sensor bearing="…"> element
```

Single-node operation produces a bearing wedge on the map.  Two or more
nodes produce a crosscut fix that promotes to the primary track location.

## Hardware Setup

### Required companion processes (on the KrakenSDR RPi)

| Process | Port | Role |
|---|---|---|
| `daq_fw` | 5000 | Firmware bridge — coherent IQ from the 5 ADCs |
| `krakensdr_doa` | 8082 | DOA solver — streams JSON `doa_result` messages |

Start them per the KrakenSDR official documentation:
```
# On the KrakenSDR RPi
cd ~/krakensdr_doa
./kraken_doa_start.sh
```

### krakensdr_doa WebSocket output format

krakensdr_doa streams JSON text frames on `ws://host:8082/ws`:

```json
{
  "type": "doa_result",
  "freq_hz": 433920000,
  "bearing_deg": 127.5,
  "bearing_std_deg": 5.2,
  "confidence": 0.83,
  "power_dbfs": -42.1,
  "snr_db": 12.3,
  "gps_lat": 37.4,
  "gps_lon": -122.1,
  "heading_deg": 0.0,
  "timestamp_unix": 1718035200.123,
  "node_id": "kraken-0"
}
```

Messages with any `type` other than `"doa_result"` are silently ignored.

**Suppression rules** — a measurement is accepted only when:
- `type == "doa_result"`
- `bearing_deg` is present and in `[0, 360)`
- `gps_lat` and `gps_lon` are both present and non-zero
- `confidence` is clamped to `[0, 1]`

## SDRPP Configuration

### kraken_lob_decoder module

Enable in **Module Manager**.  Configure in the module panel:

| Setting | Default | Description |
|---|---|---|
| Host | `127.0.0.1` | IP of the machine running krakensdr_doa |
| Port | `8082` | krakensdr_doa WebSocket port |
| Path | `/ws` | WebSocket path |
| Node ID | `kraken-0` | Device ID sent when JSON `node_id` is empty |
| Ctl Port | `8081` | krakensdr_doa DoA web root (miniserve, `en_remote_control=true`; same host) |

A green `● Connected` indicator confirms the WebSocket link is live.

### Remote frequency control (Kraken Control)

The module panel has a **Kraken Control (RX retune)** section that drives
the krakensdr_doa DoA web root the "DF Kracked" way
(`core/src/predator/kraken_ctl_client.h`). This matches how the field
Kraken Pi is set up: with **Remote Control** enabled
(`en_remote_control=true` in the DoA `_share/settings.json`), the DoA
`gui_run.sh` serves the web root via an upload-capable **miniserve** on
port **8081** (verify with `sudo ss -ltnp | grep ':8081'`). No stock
`POST /settings` API is required.

```
GET  http://<host>:8081/settings.json  → full settings JSON ("center_freq" **MHz** + VFO-0 in Hz)
POST http://<host>:8081/upload?path=/   → multipart upload of patched settings.json
```

Contract:

1. GET the full `settings.json` from the web root.
2. Patch `center_freq`, set `ext_upd_flag: true`, and retune **VFO-0**
   (`vfo_freq[0]` and/or `vfo_freq_0`, whichever the file uses).
3. Upload the complete patched `settings.json` back via the multipart
   `POST /upload?path=/` (miniserve returns `303 See Other` on success —
   field-verified — or occasionally another 2xx; both are accepted).
4. The upload applies **live** — the DoA software watches
   `settings.json` and retunes with no restart. No DAQ stop/start and no
   sweep agent are involved: this is retune/cue only.
5. Poll GET `/settings.json` until the read-back `center_freq` matches
   (±100 Hz — the file stores MHz floats) or a 30 s timeout expires.

**Unit gotcha (field-verified):** the DoA settings file stores `center_freq`
in **MHz** (e.g. `97.9`) while `vfo_freq` entries are in **Hz**. The client
converts on both read and write; never write Hz into `center_freq`.

**Legacy fallback:** older remote-control installs that only expose
`GET /settings` (e.g. on port `8042`) still work — the client detects the
`/settings.json` 404, falls back to `GET /settings`, and writes back with
the legacy `POST /settings` full-blob path.

Retune takes **several seconds** — changing `center_freq` triggers an
automatic coherent-calibration cycle. The panel shows the full
lifecycle: `Sending… → Calibrating… → Tune confirmed / Tune failed`,
plus the current Kraken frequency read back every 3 s while the control
link is on. Success is only reported after readback verification —
never assume instant success.

Panel controls:

- **Start/Stop Control** — toggles the background control client
  (config keys `krakenCtlEnabled`, `krakenCtlPort`; host is shared
  with the WebSocket ingest setting).
- Frequency field (MHz, IME-safe) + **Tune Kraken** — retune to the
  entered frequency.
- **Tune to VFO** — one-click retune to the current SDR++ VFO
  frequency.

The tune inputs are disabled while a tune is in flight. This is an
RX-only retune of a passive DF array — no transmit path exists.

### One-click DF tasking (Hits list + Android map)

Beyond the module panel, two operator surfaces can task the Kraken via
the process-global **tune bus** (`core/src/predator/kraken_tune_bus.h`,
implementation in sdrpp_core so every plugin shares one instance; the
kraken_lob_decoder registers a tune handler + status provider on
construct, mirroring `native_decoder_registry`):

- **Hits list** (`main_window.cpp`, "Detected Hits"): every hit row
  grows a **Task Kraken DF** button when a kraken module is loaded.
  Pressing it retunes the array to the hit frequency; the row shows the
  live lifecycle line (`sending… → calibrating… → tuned & confirmed /
  failed`, matched by requested frequency ±1 Hz).
- **Android map** (`root/res/maps/index.html` + `MapActivity.kt`):
  tapping an emitter dot opens a popup with a **Tune Kraken** button
  (only rendered when the `window.PredatorNative` WebView bridge
  exists, i.e. inside the APK — the dashboard iframe never shows it).
  The request flows WebView JS → `MainActivity.pendingKrakenTuneHz`
  static → `backend::pollKrakenTuneRequest()` (JNI, drained once per
  render frame in `main_window.cpp`) → tune bus. Lifecycle status flows
  back `backend::setKrakenTuneStatus()` → `MainActivity.
  krakenTuneStatusJson` → MapActivity's 1 s pusher (JSONObject.quote
  escaping) → `PredatorRFMap.updateKrakenStatus()` → popup status line.

Tune-bus footguns:

- The bus tune handler **auto-starts the control client** (and persists
  `krakenCtlEnabled`) if it isn't running, using the saved host/port —
  the operator does not need to pre-arm "Start Control".
- `trackDotGeoJSON` carries a raw `freq_hz` property specifically for
  tasking; the `freq_mhz` label string is display-only (3 decimals →
  kHz precision loss).
- Desktop (GLFW) and web backends stub `pollKrakenTuneRequest()` /
  `setKrakenTuneStatus()` to no-ops; map tasking is Android-only. The
  Hits-list button works on every backend.

### krakensdr_source module

Displays an information panel in the source selector explaining the
KrakenSDR architecture.  Does not produce IQ data — KrakenSDR raw
IQ is not accessible to SDR++.

## Build Flags

### Linux (`build_linux.sh`)

KrakenSDR modules are **ON by default**.  Pass `--no-kraken` to exclude them:

```bash
./build_linux.sh              # includes KrakenSDR (default)
./build_linux.sh --no-kraken  # excludes KrakenSDR modules
```

This controls:
```
-DOPT_BUILD_KRAKENSDR_LOB_DECODER=ON   (default)
-DOPT_BUILD_KRAKENSDR_SOURCE=ON        (default)
```

Both CMake options also default to `ON` in `CMakeLists.txt`.

### Android (`android/app/build.gradle`)

`kraken_lob_decoder` and `krakensdr_source` are listed in the module
array unconditionally so the JNI build always includes them.  The
modules are enabled at runtime via Module Manager.

### Raspberry Pi install script

```bash
sudo bash deploy/install_rpi.sh --kraken
```

`--kraken` additionally:
- Installs `numpy` and `scipy` for the N-LOB solver path.
- Installs `rtl-sdr`, `libatlas-base-dev`, `usbutils`, and `git` via apt.
- Writes `/etc/udev/rules.d/99-krakensdr.rules` granting USB access.
- Adds the service user to the `plugdev` group.
- **Clones `krakensdr_doa` into `/opt/krakensdr_doa`** (shallow clone of
  `https://github.com/krakenrf/krakensdr_doa`) and pip-installs its
  `requirements.txt`.  On re-runs it `git pull --ff-only` instead of
  re-cloning.
- Writes `/etc/systemd/system/krakensdr-doa.service` (enabled, not started)
  that launches `krakensdr_doa`'s web interface under `/opt/krakensdr_doa`.
- Writes `/etc/krakensdr/predator.env` with tunable `KRAKEN_DOA_HOST/PORT`
  environment hints used by the systemd unit.

**First-start sequence after `--kraken`:**
```bash
# 1. Confirm the array is plugged in and drivers are loaded
lsusb | grep 0bda

# 2. Review / adjust the env file
sudoedit /etc/krakensdr/predator.env

# 3. Start the DOA engine
sudo systemctl start krakensdr-doa

# 4. (optionally) watch its log
journalctl -u krakensdr-doa -f
```

Without `--kraken` the triangulator falls back to the closed-form 2-node
solver (Python stdlib only).

## Python Backend

### backend/models/lob_measurement.py

`LOBMeasurement` — one bearing observation from one node.  Key fields:

| Field | Type | Description |
|---|---|---|
| `node_id` | str | KrakenSDR device identifier |
| `node_lat/lon` | float | Array position (WGS-84) |
| `bearing_deg` | float | True bearing, 0-360 (0 = north) |
| `bearing_uncert_deg` | float | 1-sigma uncertainty (degrees) |
| `confidence` | float | DOA confidence 0..1 |
| `frequency_hz` | float | Centre frequency (Hz) |
| `heading_deg` | float | Platform heading — set to 0 for static arrays |

### backend/fusion/lob_triangulator.py

`LOBTriangulator` — stateless solver, shared across all tracks.

| Input count | Algorithm |
|---|---|
| 0 or 1 | Returns `None` |
| 2 | Closed-form 2-line intersection with crossing-angle veto (`< 15°` → `None`) |
| 3+ | `scipy.optimize.least_squares` (Levenberg-Marquardt) preferred; numpy normal-equations WLS fallback; 2-LOB last resort |

**Solver preference (3+ nodes):**
1. `scipy.optimize.least_squares` with LM damping — most robust on
   near-degenerate geometry (parallel-ish bearings, one weak node).
2. NumPy normal equations (`A^T W A x = A^T W b`) — faster but can
   diverge when `A^T W A` is near-singular.
3. Closed-form 2-LOB on the first two measurements — used only when
   neither `scipy` nor `numpy` is available.

**TTL window:**  Measurements older than `measurement_ttl_s` (default
30 s) are excluded before the solver runs.  This prevents stale bearings
from a previous sweep from distorting the current crosscut.  If _all_
measurements fall outside the TTL window (e.g. in unit tests where
timestamps are synthetic) the full history is used as a fallback.

**Crossing-angle veto (2-node only):**  When `|sin(b1 − b2)| < sin(15°)`
the intersection is geometrically unstable; the fix is suppressed.

**Error radius:**  Estimated as `range × tan(mean_uncert) × 2`, clamped
to `[20 m, 50 km]`.  Treat as a rough DF scatter cone, not a hard CEP.

**Confidence ceiling:**  `MAX_LOB_CONFIDENCE = 0.70`.  DOA phase
ambiguity limits confidence even for a 5-node perfect crossing; TDOA
always wins if it produces a fix first.

### TrackManager.ingest_lob()

```python
track = track_manager.ingest_lob(measurement)
```

- Associates measurement with an existing track by frequency (`±25 kHz`)
  or creates a new one.
- Stores `measurement` in `track.lob_measurement_history` (capped at 20).
- Calls `LOBTriangulator.triangulate()` — TTL filtering (30 s) runs inside
  the triangulator before the solver sees any measurements.
- When a crosscut fix is produced, updates `track.lob_crosscut_*` fields and:
  1. **Stationarity gate** (if `stationarity_gate=` was passed to
     `TrackManager.__init__`): sanity-checks the fix against the track's
     accepted location history.  Rejected fixes update `lob_crosscut_*`
     but are _not_ promoted to the primary location.
  2. **LOB+TDOA hybrid merge** (if `track.location_method == "tdoa"`):
     blends the LOB crosscut with the existing TDOA fix using
     inverse-variance weighting (`w = 1/r²`).  The result is stored as
     `location_method = "lob_tdoa_hybrid"` with a confidence ceiling of
     `MAX_LOB_TDOA_HYBRID_CONF = 0.85`.
  3. **LOB-only promotion** (if no TDOA fix present): promotes crosscut
     to primary only when the current method is `None`,
     `"rssi_proximity"`, or `"lob_crosscut"` with _lower_ confidence —
     never overwrites a better LOB fix with a worse one.
- Calls `custody_elector.assess(track, nodes)` if a `custody_elector`
  was wired in, so custody re-scores immediately on LOB updates.

### Track wire format (GET /tracks/)

New fields added to every track dict:

```json
{
  "lob_bearing_deg": 127.5,
  "lob_bearing_uncert_deg": 5.2,
  "lob_confidence": 0.83,
  "lob_node_ids": ["kraken-0", "kraken-1"],
  "lob_crosscut_lat": 37.401,
  "lob_crosscut_lon": -122.098,
  "lob_crosscut_radius_m": 450.0,
  "lob_crosscut_confidence": 0.41,
  "lob_node_lat": 37.41,
  "lob_node_lon": -122.12
}
```

All fields are `null` when no KrakenSDR data is present for the track.

## Map Visualization

Two new GeoJSON layers are added to the dashboard map:

| Layer | Toggle | Geometry | Description |
|---|---|---|---|
| `_lob_wedges` | DF / LOB → LOB Wedges | Polygon fan | Bearing wedge from node, ±uncertainty, 25 km range |
| `_lob_crosscut` | DF / LOB → LOB Crosscut | Polygon circle | Error radius at crosscut point |

Both layers have their own toggles in the **DF / LOB** control panel section.

**Wedge colour** matches threat level (same palette as emitter dots).

**Crosscut circles** use a magenta fill distinct from TDOA ellipses
(cyan) and RSSI proximity (yellow) so the operator can immediately
distinguish the geolocation method.

## CoT LOB Export

When `lob_bearing_deg` is set, `build_cot_xml_lob()` injects a
`<sensor>` element into the CoT detail section:

```xml
<sensor bearing="127.50" fov="10.4" range="25000"
        type="LOB" model="KrakenSDR" nodes="2"/>
```

ATAK / WinTAK render this as a bearing spoke with a field-of-view wedge
overlay on the tactical picture.

- Geolocated crosscut track → CoT type `a-u-G` with `<sensor>`.
- Bearing-only track (no crosscut) → CoT type `b-m-p-s-p-loc` with
  `<sensor>`.  The ATAK unit marker is placed at the node (array) position.

## Unit Tests

### Python tests

```bash
python -m unittest backend.tests.test_lob_triangulator -v
```

34 test cases covering: coordinate helpers, zero-node/single-node returns,
2-node orthogonal fix, crossing-angle veto, same-node deduplication,
confidence clamping, error radius bounds, 3-node WLS recovery, low-confidence
node handling, and edge cases.

### C++ tests

```bash
g++ -std=c++17 -O2 -Icore/src tests/kraken_lob_test.cpp \
    -o /tmp/kraken_lob_test && /tmp/kraken_lob_test
```

20 test cases covering: well-formed message → event populated, non-`doa_result`
type suppression, missing field suppression, invalid JSON suppression,
confidence clamping, bearing range validation, node ID from JSON,
config override node ID, frequency/power/SNR storage, plus the Kraken
control client pure logic: settings patch (center_freq + ext_upd_flag,
full-blob passthrough), non-object rejection, center_freq extraction,
readback frequency match tolerance, HTTP response parsing, and the
GET→patch→serialize→readback round trip.

## Known Limitations and Footguns

1. **Bearing is magnetic-north relative only when `heading_deg = 0`.**
   For vehicle-mounted arrays, `heading_deg` must be set to the vehicle
   heading so `bearing_deg` is interpreted as platform-relative.
   Predator RF currently does NOT apply the heading correction — the Python
   backend assumes `bearing_deg` is already true-north.  Callers must add
   `heading_deg` to `bearing_deg` (mod 360) before constructing the
   `LOBMeasurement` if the array is on a moving platform.

2. **Flat-earth approximation limits baseline to ~50 km.**
   The `LOBTriangulator` uses a 1° ≈ 111 120 m projection.  For baselines
   exceeding 50 km (very rare for DF work) the crosscut error grows beyond
   the already-large bearing uncertainty; use a proper WGS-84 solver instead.

3. **25 kHz frequency association window.**
   `ingest_lob()` uses `tracks_near_frequency(±25 kHz)`.  If two emitters
   are within 25 kHz of each other and one has a KrakenSDR bearing, the LOB
   may attach to the wrong track.  Narrow the window by injecting a
   custom `LOBTriangulator` with a tighter `freq_tolerance_hz` in
   `TrackManager.__init__`.

4. **numpy is optional.**
   The 3+-LOB WLS path requires `numpy`.  Without it the triangulator logs
   a warning and falls back to the 2-LOB closed-form solver using only the
   first two (most recent, per-node) measurements.  Install `numpy` (and
   `scipy` for future Kalman-filter LOB+TDOA blending) via
   `pip install numpy scipy`.

5. **Android KrakenSDR over USB is not implemented.**
   The Android build includes the `kraken_lob_decoder` module (WebSocket
   bridge) but not direct USB KrakenSDR access.  The array must run
   `krakensdr_doa` on a companion RPi reachable over Wi-Fi/LAN from the
   Android device.

6. **Remote retune interrupts DOA output.**
   Uploading a new `center_freq` (with `ext_upd_flag: true`) restarts the
   coherent-calibration cycle — `doa_result` messages stop for several
   seconds and any in-progress crosscut goes stale.  The control client
   only reports success after readback verification; if the panel shows
   `Tune failed`, check that port 8081 is reachable (firewall on the
   KrakenSDR RPi) and that **Remote Control is enabled**
   (`en_remote_control=true`) so miniserve is serving the DoA web root
   with upload enabled (`sudo ss -ltnp | grep ':8081'` should show
   miniserve).  Only `center_freq`/VFO-0 are controlled remotely; gain
   and every other setting must be changed on the Kraken web UI.
