# Fleet Polling Correctness

This document describes the verified contract for each endpoint in the
Kujhad fleet polling chain, the fields each one carries, and where each
field is consumed in the UI. It serves as the reference for future
contributors to know what must stay in sync when either the server
(`kujhad_fleet.h`) or client (`KujhadControllerClient`) changes.

## Polling architecture

`KujhadControllerClient` (`core/src/predator/kujhad_fleet.h`) runs one
worker thread per peer. The thread loop:

1. Polls `/v1/identify` once at connect, then every 30 s.
2. Polls `/v1/state` and `/v1/gps` each cycle (~1 s interval).
3. Polls `/v1/events?since=<lastId>` each cycle to drain hit events.
4. Spectrum stream (`/v1/spectrum`) runs on a dedicated thread started
   separately when `kujhadMirrorPeerSpectrum` is true and this peer is
   the active peer.

Results are stored in `KujhadPeerSnapshot` under a mutex. The ImGui
render thread reads via `snapshot()` (lock-protected copy).

---

## Endpoint contract

### `GET /v1/identify`

**Purpose:** Device identity and capabilities. Polled once on connect
and every 30 s.

| Field | Type | Server source | UI consumed |
|---|---|---|---|
| `device` | string | `kujhadDeviceName` | Peers list: "device=…" |
| `version` | string | `SDRPP_VERSION` | Peers list (debug) |
| `role` | string | `"device"` | Peers list: "role=…"; Fleet Peers map panel |
| `hwProfile` | string | detected hardware | (available in snapshot) |
| `advertise` | string | `kujhadAdvertiseAddress` | (available in snapshot) |

**Snapshot field:** `snap_.identify`

**Reachability:** The identify poll result sets `snap_.reachable` and
`snap_.linkLatencyMs`. A failed identify marks the peer as unreachable
and the row turns grey in the UI. All subsequent polls still run each
cycle even when reachable is false so the client recovers automatically
without a restart.

---

### `GET /v1/state`

**Purpose:** Live SDR state — VFOs, mission mode, decoders, hits, scan
status. Polled every cycle (~1 s).

| Field | Type | Server source | UI consumed |
|---|---|---|---|
| `vfos[]` | array | `sigpath::vfoManager` | Mission tab peer VFO overlay |
| `vfos[].name` | string | VFO name | |
| `vfos[].freq` | number | VFO centre freq Hz | |
| `vfos[].bw` | number | VFO bandwidth Hz | |
| `vfos[].decoder` | string | active decoder module | |
| `missionMode` | int | `predatorMissionMode` | Mission tab: mode indicator |
| `searchBands[]` | array | `searchBands` | Mission tab peer bands overlay |
| `targets[]` | array | `targets` | Mission tab peer targets overlay |
| `excludes[]` | array | `excludes` | Mission tab peer excludes overlay |
| `dwellMs` | int | `dwellMs` | Mission tab peer dwell indicator |
| `thresholdDb` | float | `missionThreshold` | Mission tab peer threshold |
| `recordAudio` | bool | `recordAudio` | Mission tab |
| `hits[]` | array | hit markers | Kujhad cached hits overlay |
| `scanning` | bool | scan running | (available in snapshot) |

**Snapshot field:** `snap_.state`

**Gaps fixed:** None — all listed fields were already present in the
server's `/v1/state` response and consumed by the client.

---

### `GET /v1/gps`

**Purpose:** Live GPS fix for the peer. Polled every cycle (~1 s).

| Field | Type | Server source | UI consumed |
|---|---|---|---|
| `hasFix` | bool | `gpsHasFix` | Fleet Peers: "no GPS fix" vs location dot |
| `lat` | float | `gpsLat` | Fleet Peers map marker |
| `lon` | float | `gpsLon` | Fleet Peers map marker |
| `accuracy` | float | `gpsAccuracyMeters` | Fleet Peers: "+/-…m" |

**Snapshot field:** `snap_.gps`

**Note:** The peer location dot on the map panel uses `hasFix` to gate
rendering. When `hasFix=false` the peer is listed as "no GPS fix" in
the Fleet Peers panel and no marker is plotted.

---

### `GET /v1/events?since=<lastId>`

**Purpose:** Incremental hit events stream. Polled every cycle. The
`since` cursor advances on each response so only new events are fetched.

| Field | Type | Description |
|---|---|---|
| `events[]` | array | new events since lastId |
| `events[].id` | uint64 | monotonic event id |
| `events[].type` | string | "hit", "decoder_payload", etc. |
| `events[].freq` | float | centre frequency Hz |
| `events[].data` | object | decoder-specific payload |
| `lastId` | uint64 | high-water mark for next poll |

**Snapshot field:** `events_` queue (bounded to 1024 events).

**Note:** The events queue drains through `popEvents()` in the main
window's per-frame waterfall overlay update. Hit markers from the peer
paint on the local waterfall when the active peer is selected and the
spectrum mirror is on. The drain loop runs regardless of whether the
spectrum stream is active — events are always polled.

---

### `GET /v1/spectrum` (chunked NDJSON stream)

**Purpose:** Live FFT bins and hit overlay for the spectrum mirror
waterfall. Runs on a dedicated thread (`spectrumWorker`), separate from
the identify/state/gps/events poll thread.

**Activation condition:** The spectrum thread starts when
`kujhadMirrorPeerSpectrum=true` AND this peer is the active peer
(`kujhadActivePeerIdx` matches). It stops cleanly via `stopFlag_` when
either condition changes.

**Stream frame fields:**

| Field | Type | Description |
|---|---|---|
| `bins[]` | array | FFT magnitudes (float, dB) |
| `serial` | uint64 | frame sequence number |
| `hits[]` | array | active hit markers |
| `searchBands[]` | array | current search bands |
| `targets[]` | array | target frequencies |
| `excludes[]` | array | exclude frequencies |

**Snapshot storage:** `kujhadPeerCachedBins`, `kujhadPeerCachedHits`,
etc. Written by the spectrum frame callback at line ~3521.

**Clean disconnect:** When the peer is unreachable the chunked HTTP
recv loop gets an empty chunk or a socket error and the spectrum thread
sets `spectrumStreaming_=false`. The UI shows "idle" for the stream
status until the thread is restarted on the next active-peer change.

---

## Python backend `KujhadClient` parity

`backend/coordination/kujhad_rns_client.py` is the **RNS commanding
sender** (not a polling client). The Python backend does not maintain a
live polling loop against Kujhad `/v1/*` endpoints — it relies on the
C++ controller-client for fleet state.

The Python backend consumes the Kujhad fleet state through the
`/tracks/` and `/events/stream` SSE endpoints published by the local
C++ web backend (`predator-rfd`), which in turn populates them from
`sigpath::*` singletons and the Kujhad snapshot. No Python-side gaps
exist in this chain.

`KujhadRNSClient` (RNS transport) and `KujhadClient` (HTTP transport)
share identical wire bodies for `tune`, `scan`, and `mission` command
classes, verified by `KujhadRNSClientParityTests` in
`backend/tests/test_rns_cmd.py`.

---

## Verified correctness summary

| Endpoint | Polled? | Fields complete? | UI rendered? | Notes |
|---|---|---|---|---|
| `/v1/identify` | ✓ 30s | ✓ | ✓ | Reachability gate |
| `/v1/state` | ✓ 1s | ✓ | ✓ | All overlay fields present |
| `/v1/gps` | ✓ 1s | ✓ | ✓ | GPS fix gated |
| `/v1/events` | ✓ 1s | ✓ | ✓ | Drain loop always runs |
| `/v1/spectrum` | ✓ on demand | ✓ | ✓ | Thread stops on peer change |
| `/v1/command` (POST) | — (write only) | ✓ | ✓ | TX rejected at wire |

No `/v1/timing` endpoint exists in the current server. TDOA trust
scoring in the Python backend uses GPS timestamps from `/v1/gps` rather
than a dedicated timing endpoint. If a `/v1/timing` endpoint is added
in the future for sub-second synchronisation, it must be wired into
both `KujhadServer` (server-side) and `KujhadControllerClient`
(client-side) to maintain parity.
