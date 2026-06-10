# Predator RF — Linux web backend and browser UI

## Overview

`predator-rfd` is the headless Linux daemon mode for Predator RF. It runs the
full C++ SDR engine (signal path, decoder modules, Kujhad fleet hub) without
requiring a display. Operator access is through a browser pointed at
`http://<host>:5555` (configurable) or via the `predator-rfctl` CLI tool.

The browser UI (`preview.html` / `web/`) is the same file served by the
Python intelligence backend. Both present identical API endpoints so the
frontend works against either process without modification — this is the
Android↔Linux parity contract described in `replit.md`.

```
┌─────────────────────────────────────────────────┐
│  Browser  ──HTTP──►  predator-rfd:5555           │
│             ──WS──►  /ws  (spectrum stream)      │
│             ──SSE──► /events/stream              │
├─────────────────────────────────────────────────┤
│  predator-rfctl  ──Unix──►  /run/predator-rfd/   │
│                             control.sock         │
└─────────────────────────────────────────────────┘
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

The web asset directory defaults to `${CMAKE_INSTALL_PREFIX}/share/predator-rf/web`.
Override at runtime with `PREDATOR_WEB_ROOT=/path/to/web`.

To build the CLI tool (always built; does not require the web backend):
```bash
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

| Variable              | Default                              | Description            |
|-----------------------|--------------------------------------|------------------------|
| `PREDATOR_WEB_ROOT`   | `/usr/share/predator-rf/web`         | Static asset directory |
| `PREDATOR_CTRL_SOCK`  | `/run/predator-rfd/control.sock`     | rfctl Unix socket      |

Add port and root to `/etc/predator-rf/predator-rf.env`:
```
PREDATOR_WEB_ROOT=/usr/share/predator-rf/web
```

Or via `config.json`:
```json
{
  "webBackendPort": 5555,
  "webRoot": "/usr/share/predator-rf/web"
}
```

## Browser UI

Open `http://<host>:5555` in any modern browser.  
On the same machine: `http://localhost:5555`

The dashboard auto-detects whether a live backend is reachable. When
reachable it tears down the mock data ticker and switches to the live
`/tracks/`, `/nodes/`, `/events/stream` feeds.

### Live endpoints (browser-facing)

| Method | Path               | Description                                      |
|--------|--------------------|--------------------------------------------------|
| GET    | `/`                | Dashboard HTML (served from `PREDATOR_WEB_ROOT`) |
| GET    | `/tracks/`         | Active emitter tracks (JSON array)               |
| GET    | `/nodes/`          | Kujhad fleet nodes (JSON array)                  |
| GET    | `/events/stream`   | SSE event stream (`text/event-stream`)           |
| WS     | `/ws`              | WebSocket spectrum stream (JSON frames)          |
| GET    | `/api/spectrum`    | Snapshot FFT bins (`?bins=256`)                  |
| GET    | `/api/events`      | Paginated event log (`?since=N`)                 |
| POST   | `/api/command`     | Issue typed command (`{class,action,args}`)      |
| GET    | `/api/identify`    | Device identity                                  |
| GET    | `/v1/*`            | Kujhad fleet v1 aliases                          |

### WebSocket spectrum frame

```json
{
  "type": "spectrum",
  "serial": 12345,
  "center": 433920000.0,
  "bandwidth": 2400000.0,
  "fft_min": -120.0,
  "fft_max": 0.0,
  "bins": [/* 256 float values, normalised 0–1 from fft_min to fft_max */]
}
```

### SSE event frame

```json
{"id": 42, "type": "hit", "freq": 433920000, "snr_db": 12.4, ...}
```

## predator-rfctl CLI

```
predator-rfctl identify
predator-rfctl state
predator-rfctl tune 433.92e6
predator-rfctl scan start
predator-rfctl scan stop
predator-rfctl mission set-mode classify
predator-rfctl events --since 100
predator-rfctl raw '{"class":"tune","action":"set","args":{"freq":433920000}}'
```

Override socket: `predator-rfctl --sock /tmp/my.sock identify`  
Override via env: `PREDATOR_CTRL_SOCK=/tmp/my.sock predator-rfctl state`

### TX commands

`tx.*` class commands are hard-rejected at the control socket — same policy as
the Kujhad HTTP server and the RNS commanding wrapper.

## Android↔Linux parity

The browser dashboard (`preview.html`) is the canonical cross-platform UI. It
must compile and run against both:

1. **Python intelligence backend** (`backend/main.py`, FastAPI) on port 8073
2. **C++ web backend** (`predator-rfd`, this module) on port 5555

Both expose identical endpoints: `GET /tracks/`, `GET /nodes/`, `GET /events/stream`.

Any change to the dashboard that requires a new endpoint must add it to **both**
backends before merging. The Python side lives in `backend/main.py`; the C++
side lives in `core/backends/web/backend.cpp`.

Spectrum data (FFT bins + waterfall) is C++ only — the Python backend does not
have direct SDR access. That tab degrades gracefully to a placeholder when
hitting the Python backend.

## Debian package

The `make_debian_package.sh` script produces `predator-rf_<ver>_amd64.deb` that
installs:

- `/usr/bin/predator-rfd` — headless daemon
- `/usr/bin/predator-rfctl` — CLI tool
- `/usr/share/predator-rf/web/` — dashboard assets
- `/lib/systemd/system/predator-rfd.service` — service unit

```bash
./make_debian_package.sh /path/to/build "libfftw3-single3,libvolk2.5,librtlsdr0"
```
