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

A green `● Connected` indicator confirms the WebSocket link is live.

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
- Installs `numpy` and `scipy` for the N-LOB weighted least-squares path.
- Installs `rtl-sdr`, `libatlas-base-dev`, and `usbutils` via apt.
- Writes `/etc/udev/rules.d/99-krakensdr.rules` granting USB access.
- Adds the service user to the `plugdev` group.
- Writes `/etc/krakensdr/predator.env` with integration hints.

Without `--kraken` the triangulator falls back to the closed-form 2-node
solver (Python stdlib only).  `krakensdr_doa` itself must be installed
separately — see https://github.com/krakenrf/krakensdr_doa.

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
| 3+ | Weighted least-squares (`numpy`), weight = `confidence²`; falls back to 2-LOB if `numpy` unavailable |

**Crossing-angle veto:**  When two bearing lines are nearly parallel
(`|sin(b1 − b2)| < sin(15°)`) the intersection is geometrically
unstable.  The fix is suppressed; the operator should wait for a third
node or a better-separated pair.

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
- Calls `LOBTriangulator.triangulate()` after each measurement.
- When a crosscut fix is produced, updates `track.lob_crosscut_*` fields
  **and** promotes the crosscut to `track.estimated_lat/lon` unless a TDOA
  fix is already present.
- LOB crosscut only overwrites RSSI proximity or an older, lower-confidence
  LOB fix — it never overwrites a `location_method == "tdoa"` fix.

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

14 test cases covering: well-formed message → event populated, non-`doa_result`
type suppression, missing field suppression, invalid JSON suppression,
confidence clamping, bearing range validation, node ID from JSON,
config override node ID, frequency/power/SNR storage.

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
