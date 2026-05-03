# Predator RF — Reticulum Network Stack (RNS) transport bridge

This package adds Reticulum (RNS) as a **parallel** transport for the same
CoT/XML traffic Predator RF already pushes to TAK. CoT continues to flow
over the existing UDP/TCP TAK path *and* over RNS simultaneously — RNS
handles its own path selection across whatever interfaces are up.

## Pinned upstream

- **Package**: `rns` (Reticulum Network Stack), pinned in
  [`requirements.txt`](./requirements.txt) at version **`1.2.0`**.
- **Why pinned**: RNS interface kwargs evolve between minor releases
  (TCPInterface argument names changed between 0.5 and 0.7, AutoInterface
  scope keys between 0.7 and 1.0). The daemon's interface translator in
  `daemon.py::_build_rns_interface` targets 1.2.0 exactly.
- **No fork, no patches**. Where we need behavior the upstream doesn't
  expose, we wrap it; we never edit vendored RNS sources.

Crypto deps (token format):

- `cbor2` for the CoT envelope
- `argon2-cffi` for the Argon2id KDF
- `pynacl` for XChaCha20-Poly1305 IETF
  (`crypto_aead_xchacha20poly1305_ietf_*`); the version byte is bound
  as AAD to block downgrade attacks

## Architecture

```
+----------------------+        +-------------------------+
|  CoT emitter (UDP)   |        |  CoC aggregator (local) |
|  239.2.3.1:6969      |        |                         |
+----------+-----------+        +------------+------------+
           ^                                 ^
           | xml                             | xml + src tag
           |                                 |
+----------+---------------------------------+------------+
|             backend/rns/bridge.py (RNSCotBridge)         |
|  publish(xml,uid)                          handle_inbound|
+----------+---------------------------------+------------+
           |                                 ^
           v                                 |
+--------------------------------------------+------------+
|             backend/rns/daemon.py (RNSDaemon)            |
|  RNS Identity, Destination(predatorrf/cot.v1),           |
|  9 interfaces, control API, dedupe + loop suppression    |
+----------+----------+----------+----------+-------------+
           |          |          |          |
        TCP/UDP    AutoIfc   RNode      KISS/AX.25/Pipe/I2P
```

The bridge does **not** import RNS itself — that lives in the daemon.
This means unit tests run with `cot_enabled=True` even when the `rns`
package isn't installed: `RNSCotBridge.publish` returns False, and
inbound is exercised directly via `handle_inbound(env_bytes, src)`.

## CoT bridge contract

- Outbound: `cot_emitter.CoTEmitter.attach_fanout(hook)` registers the
  bridge as a fan-out hook. Every successful UDP send also calls
  `bridge.publish(xml, uid)`. The bridge then calls
  `publish_fn(env, reliable)` — the daemon picks Packet (opportunistic)
  vs. Link/Resource (reliable or oversize) per spec section C
  (`PACKET_MDU = 460`).
- Inbound: the daemon's destination callback hands raw envelope bytes
  to `bridge.handle_inbound`. After per-peer dedupe + loop-suppress +
  allowlist enforcement, the inbound XML is given to the registered
  `inbound_fn` tagged with `source_transport="rns"`. The TAK UDP path
  refuses to re-emit RNS-sourced tracks unless the operator opts in
  via `BackendConfig.rns_to_ip_relay` (default off — IP↔RNS loop break).
- Per-peer dedupe LRU: 4096 entries per peer, keyed on `(uid, ts_sec)`
  per spec section C; `bridge.stats()["peers_seen"]` tracks active
  buckets.
- Allowlist (spec section D): empty list = open mode (warning logged
  at startup). Non-empty list rejects any peer not in the set; rejects
  counted in `bridge.stats()["allowlist_rejected"]`.

The envelope format is described in
[`envelope.py`](./envelope.py) (CBOR `{v,ts,src,uid,ct,z,p}` with zlib
compression beyond 256 bytes).

## Graceful restart (spec section G)

`RNSDaemon._teardown_interface(iid, drain_s)` runs three phases:
1. **Stop new outbound** — flips `iface.OUT = False` if exposed.
2. **Drain** — polls `pending_outgoing` / `tx_queue` for up to
   `drain_s` seconds (default 5s on per-interface restart, 2s on
   add/update/remove churn).
3. **Force close** — calls `detach`/`close`/`teardown`/`stop`
   whichever the iface exposes; if all raise, logs `forced close` and
   removes from `RNS.Transport.interfaces` anyway.

Identity (`identity.prv`) is never touched by restart — only by
explicit re-import without `include_identity`.

## Identity

Each node has one persistent RNS identity at:
- Linux: `~/.config/predator-rns/identity.prv` (mode 0600)
- Android: app-private storage (decision below)

Generated on first daemon start if absent. The 16-hex prefix of the
identity hash is the `src` tag in every outbound envelope and is also
used by `RNSCotBridge` for loop suppression.

## Token format

`prf-rns-v1.<crockford-base32-payload>` — see
[`token.py`](./token.py). Payload =
`[ver:1][salt:16][nonce:24][XChaCha20-Poly1305(zlib(canonical_json))]`,
key derived with Argon2id (t=3, m=64MiB, p=1, len=32).

Device-local fields (NIC names, serial port paths, listen addresses,
SAM endpoint, etc.) are exported as `{"$placeholder": "<field_path>"}`
markers and re-prompted on import. The full set is enumerated in
[`schema.py::DEVICE_LOCAL_FIELDS`](./schema.py).

## Control API

`backend.rns.daemon.ControlServer` listens on a Unix socket
(`/run/predator-rns.sock` for root, `~/.local/state/predator-rns/control.sock`
otherwise) and accepts line-delimited JSON requests. Methods:

`status`, `list_interfaces`, `get_interface`, `add_interface`,
`update_interface`, `remove_interface`, `set_enabled`,
`restart_interface`, `restart_all`, `validate_interface`,
`export_config`, `import_config`, `mint_replication_token`, `get_logs`.

The same method names are exposed in-process on Android so the Kotlin
layer doesn't need a JSON-over-socket round-trip.

## Android approach (decision recorded per task spec)

The upstream `rns` Python package depends on `cryptography` (built C
extensions), `pyserial`, `cbor2`, etc. — which builds cleanly under
the existing Android backend's embedded CPython 3.11 runtime that
already ships `cryptography` for the FastAPI HTTPS path.

**Decision**: embed the upstream Python package on Android. Run
`backend.rns.daemon.RNSDaemon` and `RNSCotBridge` in-process inside
the same CPython worker that already hosts the Predator backend on
Android. The Kotlin layer (`android/app/src/main/java/RnsBridge.kt`)
talks to that in-process daemon over the daemon's local Unix control
socket (the same one the Linux GUI uses, see
`backend/rns/daemon.py::ControlServer`) via Android's
`android.net.LocalSocket` API — *not* over HTTP. The control plane
is local-only on every platform: no TCP listener, no FastAPI route,
no exposure beyond filesystem-permission-gated (0600) Unix sockets.
Outbound RNS-CoT goes through the normal in-process bridge fan-out
hook. Inbound RNS-CoT is forwarded to the local ATAK app on the
phone via the standard ATAK CoT input port (UDP 4242 by default,
auto-enabled when `ANDROID_ROOT` is set) by the inbound_fn closure
registered when the bridge is constructed.

The native ImGui Kujhad panel in `core/src/gui/main_window.cpp`
(which also renders on Android via the shared `sdrpp_core`
NativeActivity) uses the same Unix-socket control plane on both
platforms.

This keeps the schema, token format, validation rules, and tests
identical across Linux and Android — exactly one codebase, one
contract.

The Java/Kotlin port `reticulum-network-stack-android` was considered
and rejected: it lags upstream by 6+ months and the wire format is
identical anyway, so embedding the canonical Python implementation is
both lower risk and removes a long-term sync burden.

## Running the daemon

```
# Linux:
python -m backend.rns
# or via systemd:
sudo cp deploy/predator-rns.service /etc/systemd/system/
sudo systemctl enable --now predator-rns
```

State directory defaults to `~/.config/predator-rns/`. Override with
`PREDATOR_RNS_STATE_DIR=/var/lib/predator-rns python -m backend.rns`.

## Field verification

See [`docs/rns_field_log.md`](../../docs/rns_field_log.md) for
hardware verification entries. Hardware verification entries that
require physical hardware not available to the implementation
environment are recorded there as `BLOCKED:operator`.
