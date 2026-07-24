# DF-Kracked LOB Sensor

A standalone, sensor-only peer for the **Predator RF "Kujhad" fleet**. It runs
on a KrakenSDR Raspberry Pi, reads bearings (LOBs) from the local
`krakensdr_doa` DoA feed, and serves them over the Kujhad v1 protocol so any
controller — including the Predator RF phone app — can pair and mirror the
sensor's bearings onto its map.

It is **RX-only** and accepts no commands. It is a thin, single-file service
(`sensor.py`) plus a small persisted config; it does **not** import the
Predator RF `backend/`.

## What it does

```
krakensdr_doa  ──ws://127.0.0.1:8082/ws──▶  sensor.py  ──HTTP+JSON (Kujhad v1)──▶  controller / phone
   (DoA engine)      doa_result frames       KRAKEN_LOB events        X-Kujhad-Key auth
```

- Connects **read-only** to the Kraken DoA WebSocket. It does not upload,
  retune, or otherwise touch the Kraken — it just listens, so it does **not
  conflict** with the decoder module or the controller's own Kraken control.
- Converts each usable `doa_result` frame into a `KRAKEN_LOB` event row whose
  `raw.{bearing_deg, gps_lat, gps_lon, timestamp_unix, …}` keys match exactly
  what the controller's LOB aggregator reads.
- Throttles to ~2 events/s and coalesces identical bearings within 0.5 s so
  the Kraken can't flood the fleet.

## Prerequisites

- `krakensdr_doa` running on the Pi with:
  - **`en_remote_control = true`** (so the DoA engine exposes its data feed),
  - the DoA data server / miniserve reachable at **`ws://127.0.0.1:8082/ws`**
    (this is the default; override with `--ws` if your build differs).
- Python 3.8+ (`python3`), `pip3`.
- Overlay network up (ZeroTier or Headscale/Tailscale) so peers can reach the Pi.

## Install (one command)

```bash
sudo ./install.sh
```

The installer:
1. checks `python3`,
2. installs `aiohttp` + `websockets` (falls back to `--break-system-packages`
   on PEP 668 "externally managed" Raspberry Pi OS),
3. copies files to `/opt/df_kracked_sensor`,
4. installs, enables, and starts the `df-kracked-sensor.service` systemd unit,
5. prints the **peer code**.

Set the service account with `SERVICE_USER=youruser sudo ./install.sh` (default `pi`).

To customise arguments (fixed site position, name, etc.) without editing the
unit, create `/etc/default/df-kracked-sensor`:

```sh
ARGS="--name df-north --lat 37.4219 --lon -122.0841 --heading 0"
# or, for a moving/gpsd site:  ARGS="--name df-mobile --gpsd"
```

Then `sudo systemctl restart df-kracked-sensor`.

## Run manually (no systemd)

```bash
python3 sensor.py --bind 0.0.0.0 --port 9151 --name df-north --lat 37.42 --lon -122.08
```

Key CLI flags:

| Flag | Meaning |
|------|---------|
| `--bind` | HTTP bind address (default `0.0.0.0` — needed for overlay peers) |
| `--port` | HTTP port (default `9151`) |
| `--key`  | API key; if omitted, one is generated and persisted to `df_kracked_sensor.json` next to the script and reused |
| `--name` | device name on `/v1/identify` (default hostname) |
| `--ws`   | Kraken DoA websocket URL (default `ws://127.0.0.1:8082/ws`) |
| `--lat` / `--lon` / `--heading` | fixed-site position (heading default 0) |
| `--gpsd` | poll `gpsd` on `localhost:2947` for live position (degrades gracefully) |

On startup it prints a pairing block, e.g.:

```
============================================================
  DF-KRACKED LOB SENSOR — PAIRING
============================================================
  PEER CODE:  100.88.4.12:9151  key=8f3c…d19a
  ...
```

Each non-loopback interface (ZeroTier/Headscale first, then LAN) gets its own
`PEER CODE:` line.

## Pair the phone app

In **Predator RF → Kujhad → Add Peer**, enter:

- **IP / host**: the overlay IP from the `PEER CODE:` line (the ZeroTier /
  Headscale address, not the LAN one, when connecting across the overlay),
- **Port**: `9151` (or your `--port`),
- **API key**: the `key=` value. The app sends it as the `X-Kujhad-Key`
  header on every request.
- **TLS**: leave off (this service serves plain HTTP over the private overlay).

Once paired, the controller polls `/v1/identify`, `/v1/state`, `/v1/gps`, and
`/v1/events?since=`. The sensor advertises `role: "sensor"` and a minimal-but-
valid mission state (empty bands/targets/hits) so it never breaks the Mission
UI.

## ZeroTier / Headscale note

- Bind to `0.0.0.0` (the default) so the service is reachable on the overlay
  interface. The overlay itself is the security boundary; the API key is the
  shared secret.
- Use the **overlay IP** printed in the peer code (e.g. `100.x.x.x` for
  Headscale/Tailscale, `10.147.x.x` / `zt*` for ZeroTier), not `127.0.0.1` or
  the plain LAN address, when the controller is on the overlay.

## How bearings appear on peers' maps

Each emitted LOB becomes a `KRAKEN_LOB` decoder event. The controller merges
these events verbatim and its LOB aggregator reads
`raw.bearing_deg` + `raw.gps_lat` / `raw.gps_lon` (the sensor's position at the
time of the measurement) + `raw.timestamp_unix`, then draws the line of bearing
from the sensor's location. Multiple sensors' LOBs crosscut into a fix.

## Files

| File | Purpose |
|------|---------|
| `sensor.py` | the asyncio service (Kraken WS client + Kujhad v1 HTTP server) |
| `df-kracked-sensor.service` | systemd unit |
| `install.sh` | idempotent Pi installer |
| `test_sensor.py` | unit tests (`python3 -m pytest test_sensor.py`) |
| `df_kracked_sensor.json` | auto-generated; stores the persisted API key |

## Tests

```bash
pip3 install aiohttp websockets pytest
python3 -m pytest test_sensor.py -q
```
