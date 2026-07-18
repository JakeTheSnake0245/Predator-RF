# Predator RF — Linux web backend and browser UI

## Overview

`predator-rfd` is the headless Linux daemon mode for Predator RF. It runs the
full C++ SDR engine (signal path, decoder modules, Kujhad fleet hub) without
requiring a display. Operator access is through a browser pointed at
`http://localhost:5555` (configurable) or via the `predator-rfctl` CLI tool.

The browser UI (`preview.html` / `web/index.html`) is the same file served by
the Python intelligence backend. Both expose identical API endpoints under
`/api/v1/*` so the frontend works against either process without modification —
this is the Android↔Linux parity contract described in `replit.md`.

```
┌─────────────────────────────────────────────────────────────┐
│  Browser  ──HTTP──►  predator-rfd:5555                       │
│             ──WS──►  /ws  (spectrum stream)                  │
│             ──SSE──► /api/v1/events/stream                   │
├─────────────────────────────────────────────────────────────┤
│  predator-rfctl  ──Unix──►  /run/predator-rfd/control.sock  │
└─────────────────────────────────────────────────────────────┘
```

## Build

```bash
mkdir build && cd build
cmake .. \
  -DOPT_BACKEND_WEB=ON \
  -DOPT_BACKEND_GLFW=OFF \
  -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

`OPT_BACKEND_WEB=ON` and `OPT_BACKEND_GLFW=OFF` must not be combined (they
compile different `backend.cpp` TUs into `sdrpp_core`).

The web asset directory defaults to `${CMAKE_INSTALL_PREFIX}/share/predator-rf/web`.
Override at runtime with `PREDATOR_WEB_ROOT=/path/to/web`.

```bash
# CLI tool only (built automatically alongside the daemon)
cmake .. -DOPT_BACKEND_WEB=ON
make predator-rfctl
```

## Systemd service

```bash
sudo systemctl enable --now predator-rfd
sudo journalctl -u predator-rfd -f
```

Service unit: `deploy/predator-rfd.service`  
Installed at: `/lib/systemd/system/predator-rfd.service`

Configuration is read from `/etc/predator-rf/config.json` (same format as the
GLFW desktop build). Overrides via environment:

| Variable              | Default                              | Description              |
|-----------------------|--------------------------------------|--------------------------|
| `PREDATOR_WEB_ROOT`   | `/usr/share/predator-rf/web`         | Static asset directory   |
| `PREDATOR_CTRL_SOCK`  | `/run/predator-rfd/control.sock`     | rfctl Unix socket path   |

Relevant `config.json` keys:

```json
{
  "webBackendPort":  5555,
  "webRoot":         "/usr/share/predator-rf/web",
  "kujhadApiKey":    "<32-char-hex>",
  "webBindAll":      false
}
```

- **`webBindAll`** — defaults to `false` (loopback only, `127.0.0.1`). Set to
  `true` only when the operator explicitly needs the dashboard reachable from
  other hosts on the LAN. Must be paired with a strong `kujhadApiKey`.

## Kujhad plain-HTTP lockout on overlay networks (tailnet / ZeroTier)

The Kujhad fleet server (the device-side peer protocol listener, distinct
from the dashboard web server above) is **loopback-only when TLS is off**:
every non-loopback connection is closed at accept time so the API key never
crosses the network in the clear.

On an overlay deployment (Tailscale, ZeroTier, WireGuard) this means a node
started **without TLS enabled is invisible to its coordinator** — polls are
accepted then instantly dropped, which looks exactly like a dead node. This
is no longer silent:

- The node logs a rate-limited (30 s) warning to stderr/journal identifying
  the rejected peer IP:
  `[kujhad] WARNING: plain-HTTP mode: rejected remote peer <ip> …`
- The Kujhad tab in the node UI shows a persistent red **PLAIN-HTTP
  LOCKOUT** warning with the rejection count and the last rejected peer IP.

### Fixes

1. **Enable TLS** (preferred) — flip the TLS toggle in the Kujhad tab and
   regenerate the self-signed cert; controllers pin the fingerprint.
2. **Overlay CIDR allowlist** (opt-in, default empty) — when the overlay
   itself is the encryption/trust boundary, allowlist its CIDR range for
   plain HTTP:

   - UI: Kujhad tab → *Plain-HTTP overlay allowlist (CIDRs)*, e.g.
     `100.64.0.0/10` (tailnet CGNAT range) or your ZeroTier subnet.
   - Config key: `"kujhadPlainHttpAllowCidrs": "100.64.0.0/10"` (comma /
     space / semicolon separated IPv4 CIDRs; a bare address means `/32`).

   **The API key travels unencrypted to allowlisted peers.** This is only
   safe when the overlay encrypts the wire end-to-end. The allowlist is
   ignored while TLS is active, invalid entries are flagged in the UI, and
   edits apply live. RX-only posture (`tx.*` rejection) is unaffected.

## Authentication

All `/api/v1/*`, `/v1/*`, and `/ws` endpoints (except `/api/v1/identify`)
require the `X-Kujhad-Key` header or `Authorization: Bearer <key>` with the
value matching `kujhadApiKey` in config. Static files (the dashboard itself)
are exempt.

```bash
# With key
curl -H "X-Kujhad-Key: <your-key>" http://localhost:5555/api/v1/status

# No key (only works when kujhadApiKey is empty — loopback dev mode)
curl http://localhost:5555/api/v1/status
```

If no key is configured the server logs a warning and accepts all connections.
This is safe only because the default bind address is loopback.

## Browser UI

Open `http://localhost:5555` in any modern browser.

When `kujhadApiKey` is set, the dashboard sends `X-Kujhad-Key` in every API
request header. The browser will be prompted or the operator must supply the
key via the `?backend=http://localhost:5555` URL parameter alongside the
frontend auth mechanism.

The dashboard auto-detects whether a live backend is reachable. When reachable
it tears down the mock data ticker and switches to live feeds.

### Live endpoints (browser-facing)

All endpoints are available at both the short path and the `/api/v1/` prefix.
The dashboard calls the `/api/v1/` form; `curl` can use either.

| Method | Path                         | Description                                          |
|--------|------------------------------|------------------------------------------------------|
| GET    | `/`                          | Dashboard HTML (from `PREDATOR_WEB_ROOT`)            |
| GET    | `/api/v1/identify`           | Device identity (public — no auth required)          |
| GET    | `/api/v1/status`             | Daemon status (SDR state, scan, mission mode)        |
| GET    | `/api/v1/state`              | Combined nodes + tracks + status snapshot            |
| GET    | `/api/v1/nodes/`             | Kujhad fleet nodes (JSON array)                      |
| GET    | `/api/v1/nodes/df_capability`| Fleet DF capability summary (LOB / TDOA / RSSI-only) |
| GET    | `/api/v1/tracks/`            | Active emitter tracks (JSON array)                   |
| GET    | `/api/v1/events/stream`      | SSE live event stream (`text/event-stream`)          |
| GET    | `/api/v1/events`             | Paginated event log (`?since=N`)                     |
| GET    | `/api/v1/spectrum`           | Snapshot FFT bins (`?bins=256`)                      |
| POST   | `/api/v1/command`            | Typed command (`{class, action, args}`)              |
| GET    | `/api/v1/key`                | Key config status (length only — never returns key)  |
| GET    | `/api/v1/port`               | Current web port                                     |
| GET    | `/api/v1/role`               | Current node role (device / controller)              |
| GET    | `/api/v1/peers/`             | Fleet peer list (alias for nodes)                    |
| WS     | `/ws`                        | WebSocket spectrum stream (JSON frames)              |

`tx.*` class commands are rejected at the POST `/api/v1/command` endpoint —
same policy as Kujhad HTTP and the RNS commanding wrapper.

### WebSocket spectrum frame

```json
{
  "type":      "spectrum",
  "serial":    12345,
  "center":    433920000.0,
  "bandwidth": 2400000.0,
  "fft_min":   -120.0,
  "fft_max":   0.0,
  "bins": [/* 256 floats, resampled from raw FFT */]
}
```

### SSE event frame

```json
{"id": 42, "type": "hit", "freq": 433920000, "snr_db": 12.4, ...}
```

## predator-rfctl CLI

```
predator-rfctl status
predator-rfctl identify
predator-rfctl tune 433.92e6
predator-rfctl scan start|stop|pause
predator-rfctl mission set-mode manual|classify|scan|quickscan
predator-rfctl role show
predator-rfctl role set device|controller
predator-rfctl key show
predator-rfctl key regenerate
predator-rfctl port show
predator-rfctl peer list
predator-rfctl peer add <name> <host> <port> <key>
predator-rfctl peer remove <name>
predator-rfctl events [--since N]
predator-rfctl start
predator-rfctl stop
predator-rfctl raw '{"class":"tune","action":"set","args":{"freq":433920000}}'
```

Override socket path:
```bash
predator-rfctl --sock /tmp/my.sock status
PREDATOR_CTRL_SOCK=/tmp/my.sock predator-rfctl status
```

### TX commands

`tx.*` class commands are hard-rejected at the control socket — same policy as
the Kujhad HTTP server and the RNS commanding wrapper.

### Command dispatch model

Commands sent via `predator-rfctl` or `POST /api/v1/command` are enqueued in a
thread-safe queue and drained by the `renderLoop()` main thread every 50 ms.
This keeps all signal-path mutations on the main thread. Currently wired:

| class / action          | Effect                                           |
|-------------------------|--------------------------------------------------|
| `tune / set`            | Calls `sigpath::sourceManager.setFrequency()`   |
| `scan / start`          | Sets scan-running flag, status → "running"       |
| `scan / stop`           | Sets scan-running flag false, status → "idle"    |
| `scan / pause`          | Sets status → "paused"                          |
| `mission / set-mode`    | Updates mission mode integer                    |
| `role / set`            | Updates role string                             |
| Everything else         | Logged; emits `command_applied` event with `applied:false` |

Full signal-path wire-up (spectrum push, track/node snapshots) is the subject
of the follow-up task (see `docs/linux_web_frontend.md` wire-up section below).

## Live-data wire-up (follow-up task)

The hooks are defined in `core/src/backend.h` and implemented in
`core/backends/web/backend.cpp`. No-op stubs exist in the GLFW and Android
backends so the call sites compile unconditionally.

Add these calls in `core/src/gui/main_window.cpp`:

```cpp
#include <backend.h>

// --- In the FFT releaseFFTBuffer path (same spot as kujhadSpectrumRaw capture):
backend::webBackendPushSpectrumSnapshot(
    fftBins.data(), (int)fftBins.size(),
    centerFreq, bandwidth, fftMin, fftMax);

// --- In the Kujhad snapshot refresh tick (once per render tick):
backend::webBackendUpdateNodes(kujhadNodesJson);
backend::webBackendUpdateTracks(kujhadTracksJson);
backend::webBackendUpdateStatus(
    sigpath::sourceManager.isRunning(),
    sigpath::sourceManager.getFrequency(),
    sigpath::vfoManager.getBandwidth(""),
    sigpath::sourceManager.getSelectedSourceName(),
    missionMode, scanRunning, scanStatus);

// --- When appending to predatorEvents:
nlohmann::json ev; ev["type"]="hit"; /* populate */ ;
backend::webBackendPushEvent(ev);
```

## Android↔Linux parity contract

The browser dashboard (`preview.html`) is the canonical cross-platform UI. It
must work against both:

1. **Python intelligence backend** (`backend/main.py`, FastAPI, port 8073)
2. **C++ web backend** (`predator-rfd`, this module, port 5555)

Both expose identical endpoints under `/api/v1/*`:
`GET /api/v1/tracks/`, `GET /api/v1/nodes/`, `GET /api/v1/events/stream`.

Any change to the dashboard that adds a new endpoint must add it to **both**
backends before merging. Python side: `backend/main.py`; C++ side:
`core/backends/web/backend.cpp`.

Spectrum data (FFT bins + waterfall) is C++ only — the Python backend has no
direct SDR access. That tab degrades gracefully to a placeholder when hitting
the Python backend.

## Debian package

The `make_debian_package.sh` script produces `predator-rf_<ver>_amd64.deb`:

- `/usr/bin/predator-rfd` — headless daemon
- `/usr/bin/predator-rfctl` — CLI tool
- `/usr/share/predator-rf/web/` — dashboard assets
- `/lib/systemd/system/predator-rfd.service` — service unit

```bash
./make_debian_package.sh /path/to/build "libfftw3-single3,libvolk2.5,librtlsdr0"
```
