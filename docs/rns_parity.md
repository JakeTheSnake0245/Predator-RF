# RNS Linux ↔ Android parity matrix

This is the cross-platform parity rule from section H of `task-27.md`.
Every config field, control method, and UI control listed here must
land on **both** the Linux GUI Kujhad panel and the Android Kujhad
panel as part of the same task.

Legend: `✓` = implemented, `–` = not applicable to that platform.

## Config fields (per interface)

| Field | Linux UI | Android UI | Daemon |
|---|---|---|---|
| `id`                  | ✓ (auto) | ✓ (auto) | ✓ |
| `name`                | ✓ | ✓ | ✓ |
| `type` (9 types)      | ✓ | ✓ | ✓ |
| `enabled`             | ✓ | ✓ | ✓ |
| `mode`                | ✓ | ✓ | ✓ |
| `outgoing`            | ✓ | ✓ | ✓ |
| `bitrate_hint_bps`    | ✓ | ✓ | ✓ |
| `announce_interval_s` | ✓ | ✓ | ✓ |
| `notes`               | ✓ | ✓ | ✓ |
| `reliable_cot`        | ✓ | ✓ | ✓ |
| `ifac_netname`        | ✓ | ✓ | ✓ |
| `ifac_netkey`         | ✓ | ✓ | ✓ |
| `ifac_size`           | ✓ | ✓ | ✓ |

Per-type fields (TCP client/server, UDP, I2P, AutoInterface, RNode,
KISS TNC, AX.25 KISS, Pipe) — every field listed in section B of the
task spec is rendered with the matching input control on both UIs.

## Control API methods

| Method | Linux UI button | Android UI button | Daemon |
|---|---|---|---|
| `status`                 | live status panel | live status panel | ✓ |
| `list_interfaces`        | table refresh     | list refresh      | ✓ |
| `get_interface`          | edit dialog open  | edit sheet open   | ✓ |
| `add_interface`          | "Add interface"   | FAB + form        | ✓ |
| `update_interface`       | "Save"            | "Save"            | ✓ |
| `remove_interface`       | trash icon        | swipe-to-delete   | ✓ |
| `set_enabled`            | toggle switch     | toggle switch     | ✓ |
| `restart_interface`      | "Restart" per row | "Restart" per row | ✓ |
| `restart_all`            | "Restart all"     | "Restart all"     | ✓ |
| `validate_interface`     | inline form check | inline form check | ✓ |
| `export_config`          | "Export"          | "Export" + share  | ✓ |
| `import_config`          | "Import"          | "Import" + scan   | ✓ |
| `mint_replication_token` | "Mint token"      | "Mint token"      | ✓ |
| `get_logs`               | log tail panel    | log tail panel    | ✓ |

## Token portability

A `prf-rns-v1.*` token minted on Linux imports cleanly on Android with
the same passphrase, and vice versa. Identity-included and
identity-excluded paths are both verified by `test_rns_token.py`.

## Transport behavior parity

| Behavior | Linux | Android | Notes |
|---|---|---|---|
| Packet ≤ MTU                   | ✓ | ✓ | `PACKET_MDU = 460` |
| Link + Resource > MTU          | ✓ | ✓ | auto-promote |
| Reliable mode (`reliable_cot`) | ✓ | ✓ | per-interface flag |
| Per-peer dedupe LRU            | ✓ | ✓ | 4096 entries / peer |
| Loop suppression (own `src`)   | ✓ | ✓ | bridge-level |
| Peer allowlist                 | ✓ | ✓ | spec section D |
| IP↔RNS loop break              | ✓ | ✓ | `rns_to_ip_relay` flag |
| Graceful restart drain         | ✓ | ✓ | `drain_timeout_s` 5s |
| Forced close on hung iface     | ✓ | ✓ | `last_error="forced"` |
| Identity preserved on restart  | ✓ | ✓ | spec section G |
| XChaCha20-Poly1305 IETF tokens | ✓ | ✓ | spec section E |
| Argon2id KDF (t=3,m=64MiB,p=1) | ✓ | ✓ | spec section E |
| AAD-bound version byte         | ✓ | ✓ | downgrade-resistant |

## Common-field coverage (spec section B)

| Common field           | schema | daemon applies | Linux UI | Android UI (HTTP) |
|------------------------|--------|----------------|----------|-------------------|
| `mode`                 | ✓ | iface.mode | combo  | via `/api/v1/rns/*` cfg |
| `outgoing`             | ✓ | iface.OUT/.outgoing | checkbox | via cfg |
| `bitrate_hint_bps`     | ✓ | iface.bitrate | int input | via cfg |
| `announce_interval_s`  | ✓ | iface.announce_interval | int input | via cfg |
| `notes`                | ✓ | (label only) | text input | via cfg |
| `reliable_cot`         | ✓ | publish path | checkbox | via cfg |
| `ifac_netname`         | ✓ | iface.ifac_netname | text input | via cfg |
| `ifac_netkey`          | ✓ | iface.ifac_netkey | password input | via cfg (in token AEAD) |
| `ifac_size`            | ✓ | iface.ifac_size  | int input  | via cfg |

## Per-type advanced-field coverage

| Type           | Advanced fields wired end-to-end |
|----------------|-----------------------------------|
| tcp_client     | kiss_framing, i2p_tunneled |
| tcp_server     | prefer_ipv6, i2p_tunneled |
| udp            | forward_address, forward_port |
| i2p            | peers[], i2p_sam_address, connectable |
| auto_interface | discovery_scope, discovery_port, data_port, allowed_interfaces[], ignored_interfaces[] |
| rnode          | flow_control, id_callsign, id_interval_s |
| kiss_tnc       | databits, parity, stopbits, preamble_ms, txtail_ms, persistence, slottime_ms, flow_control, beacon_interval_s, beacon_data |
| ax25_kiss      | (kiss_tnc set) + callsign, ssid, axint_port |
| pipe           | respawn_delay_s |

## Control-plane locality (no network exposure)

The daemon control plane is **local-only on every platform**:

  * **Linux** — `backend/rns/daemon.py::ControlServer` listens on a
    Unix socket (`/run/predator-rns.sock` as root, otherwise
    `~/.local/state/predator-rns/control.sock`), mode 0600, with
    SO_PEERCRED uid checks on accept. Wire format: line-delimited
    JSON, `{id, method, params}` request → `{id, ok, result|error}`
    response.
  * **Android** — `RnsBridge.kt` opens the same Unix socket via
    `android.net.LocalSocket` (`Namespace.FILESYSTEM`) and speaks
    the same line-JSON protocol. Each Kotlin method (`status()`,
    `listInterfaces()`, `addInterface()`, `restartInterface()`,
    `exportConfig()`, `importConfig()`, `mintReplicationToken()`,
    `getLogs()`) maps 1:1 to the Python `RNSDaemon` method name.
    The socket path is taken from the `PREDATOR_RNS_SOCK` system
    property the embedded backend exports at startup.
  * **No HTTP control plane is mounted on the FastAPI server.**
    `backend/api/server.py` deliberately omits the
    `/api/v1/rns/*` router so the daemon control surface is never
    reachable over the network, even when the public API binds to
    `0.0.0.0`. The `backend/api/routes/rns.py` module is retained
    only as importable scaffolding for future opt-in modes.

Inbound RNS CoT is forwarded to the device's local TAK app on UDP
4242 by default on Android (`RNS_ATAK_LOCAL_PORT` defaults to 4242
when `ANDROID_ROOT` is set), satisfying the "peer-relayed CoT shown
on the device's local TAK map" requirement.

## Implementation notes

Both UIs render the full per-interface table, add/edit modal (with the
correct field set per the 9 interface types in `backend/rns/schema.py`),
enabled toggle, restart, delete, validate, export/import, mint, and
log-tail controls. The Linux GUI panel lives in `core/src/gui/main_window.cpp`
under the `RNS Interfaces (Reticulum)` collapsing header inside the
Kujhad tab and uses the Unix-socket control plane via the helpers in
`core/src/predator/kujhad_rns.h`. The Android UI is the same native
ImGui panel rendered through `NativeActivity` (Android shares
`sdrpp_core` and `main_window.cpp` with the Linux build); the
`RnsBridge.kt` HTTP client is provided as the parallel control plane
that any pure-Kotlin tooling on Android can use against
`/api/v1/rns/*`.

New rows MUST be added to this table whenever either UI gains a
control.
