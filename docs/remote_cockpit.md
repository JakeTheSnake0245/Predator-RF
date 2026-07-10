# Predator RF — Remote Cockpit (Roadmap D)

## Overview

The remote-cockpit path lets an Android phone (or any WiFi-connected device)
act as the **Controller** while a Raspberry Pi / mini-PC running
`predator-rfd` acts as the **Device**. The phone renders the full native
ImGui UI and all SDR functions are forwarded over the network — no SDR
hardware is connected to the phone.

---

## Architecture

```
┌──────────────────────────────────┐        WiFi / Hotspot / LAN
│          Android Phone           │ ◄─────────────────────────────
│   Predator RF (Controller mode)  │
│                                  │
│  source: predator_node_source    │  HTTP polling  ◄─── spectrum/events
│  ─────────────────────────────── │  HTTP POST     ───► commands
│  ImGui UI — full native          │  link auto-detect every poll cycle
└──────────────────────────────────┘

        192.168.43.1  ← Android hotspot (phone is gateway)
        192.168.4.1   ← RPi soft-AP (RPi is gateway)
        env/config    ← operator-configured static IP
```

**Source module:** `source_modules/predator_node_source/`

The module registers as a standard SDR++ source. When active it:
1. Auto-detects the Device IP by probing the three well-known addresses in
   order: `192.168.43.1` (Android hotspot), `192.168.4.1` (RPi AP), then
   the operator-configured static IP.
2. Polls `/api/spectrum` for FFT bins and feeds them into the DSP stream.
3. Polls `/api/v1/events/stream` for decoded hits/tracks.
4. Forwards every VFO tune/scan/mission command via HTTP POST to
   `/api/v1/command` on the Device.

---

## tx.* lock — removal for remote-cockpit path

The standard Predator RF build **hard-rejects** `tx.*` class commands at
two points in `core/backends/web/backend.cpp` (compile-time guards via
`#ifndef OPT_ALLOW_TX_COMMANDS`).

The `predator_node_source` CMakeLists defines `OPT_ALLOW_TX_COMMANDS` so
the **Controller** binary can forward any command class. The **Device**
node's own tx.* guard (in its `predator-rfd` build, which does NOT define
`OPT_ALLOW_TX_COMMANDS`) enforces the RX-only policy at the point of
execution. This two-tier model means the phone cannot cause actual RF
transmission — the Device simply rejects any tx.* it receives.

Python RNS commanding (`backend/rns/cmd.py`) retains its own independent
tx.* rejection and is NOT affected by `OPT_ALLOW_TX_COMMANDS`.

---

## Link auto-detection

`PredatorNodeClient::detectLink()` probes in order:

| Priority | Address | Use case |
|----------|---------|---------|
| 1 | `192.168.43.1` | Android USB/WiFi hotspot (phone is gateway) |
| 2 | `192.168.4.1` | RPi soft-AP (RPi creates the AP) |
| 3 | Operator static IP | Field-configured address |

Probe timeout: 1 second per candidate. Detection runs on every failed poll
so link type is re-detected automatically after a network change.

---

## Build flags

| CMake flag | Default | Effect |
|-----------|---------|--------|
| `OPT_BUILD_PREDATOR_NODE_SOURCE` | `ON` | Build the remote-cockpit source module |
| `OPT_ALLOW_TX_COMMANDS` | undefined | Set by predator_node_source CMakeLists; do NOT set globally |

---

## Operator runbook

1. Start `predator-rfd` on the RPi/mini-PC (normal systemd unit, no extra
   flags needed).
2. On the phone: open Predator RF, select **Predator Node** as the source.
3. The module auto-detects the Device IP and shows link type + latency in
   the source panel.
4. All SDR functions (tune, scan, mission modes, decoder control) operate
   normally through the forwarded command channel.
5. Spectrum and decoded hits stream back at the polling interval (default
   250 ms); reduce to 100 ms on a local hotspot for near-real-time response.

---

## Security posture

- Commands flow over the existing `X-Kujhad-Key` authenticated HTTP
  channel — same auth as all other Kujhad API calls.
- The Device's tx.* guard is the authoritative enforcement point.
- The Controller (`predator_node_source`) only opens outbound connections;
  it does not expose a listening port.
- On an Android hotspot, the RPi is behind NAT and is not reachable from
  the internet.
