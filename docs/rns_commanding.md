# RNS commanding wrapper (roadmap #6)

Brings Kujhad-style `{class, action, args}` tasking commands over a
new `predatorrf/cmd.v1` Reticulum aspect so a Controller-mode peer
can task a Device-mode peer when IP is unreachable. Sits beside the
existing `cot.v1` outbound path — same Identity, same announce
handler, same per-peer fan-out.

## Files

| File | Responsibility |
|---|---|
| `backend/rns/cmd.py` | CBOR envelope wrap/unwrap; class allowlist; `tx.*` hard-reject. |
| `backend/rns/cmd_handler.py` | `RNSCmdBridge` — Controller-side publish + Device-side dispatch (mirrors `RNSCotBridge`). |
| `backend/coordination/kujhad_rns_client.py` | Controller-side sender with `send_{tune,scan,mission}_command(peer_h16, ...)` shape mirroring `KujhadClient`. |
| `backend/rns/daemon.py` | Optional second IN Destination on `predatorrf/cmd.v1`; `_publish_envelope_cmd` outbound; `_on_cmd_packet` inbound. Gated on `config["cmd_v1_enabled"]`. |
| `backend/tests/test_rns_cmd.py` | 29 tests / envelope, bridge, parity, daemon wire-up. |

## Wire format

CBOR envelope, identical six-key shape to `cot.v1`:

```
{ v: 1, ts: <ms>, src: <16-hex>, uid: <str>, ct: "cmd/json",
  z: 0|1, p: <bytes> }
```

`p` is JSON bytes of `{"class","action","args"}` — **byte-identical**
to the body the Kujhad HTTP `/v1/command` endpoint receives. That
parity is the load-bearing guarantee: the Device-side dispatcher can
route both transports through the same execution path, so an audit
trail / rate-limit / quota that exists for HTTP automatically covers
RNS.

`z=1` toggles zlib compression when the JSON payload exceeds 256 B
(matches `envelope.py`).

## Auth & access control

Three layers, all mandatory:

1. **Packet-source authority** — the receiver derives the sender's
   16-hex prefix from `RNS.Packet.source_hash` (the
   transport-authenticated Identity), NOT from the envelope's
   self-declared `src` field. Envelope `src` is informational; if it
   disagrees with the packet source the envelope is dropped and
   `envelope_errors` is bumped (defense against an attacker who
   replays an envelope from a different Identity). The unauthenticated
   envelope-only fallback path is reachable in unit tests that feed
   bytes directly to `handle_inbound`, but never across an RNS hop.
2. **Peer allowlist** — same 16-hex set as cot.v1 (read from
   `config["peer_allowlist"]`). Empty = open mode (any peer
   accepted); populated = strict allowlist.
3. **Loop suppression** — `src == own_hash16` envelopes are dropped
   without dispatch; protects the daemon's own IN destination from
   re-executing its own published commands when RNS routes them
   locally for opportunistic flooding.

## RX-only enforcement

Two-sided gate against `tx.*` command classes:

* **wrap-time** (`backend/rns/cmd.py:_validate_cmd_dict`) — refuses
  to encode any class whose lowercase name starts with `tx`.
  `RNSCmdBridge.publish` translates this into a `False` return and
  bumps the `envelope_errors` counter.
* **unwrap-time** — same check on the receiver. A malicious or buggy
  sender that hand-rolled a CBOR envelope bypassing the wrap-side
  guard still cannot get a `tx.*` past the recipient.

Both gates also enforce the `ALLOWED_COMMAND_CLASSES` allowlist
(`tune, scan, mission, decoder, marker, hold, vfo, source, audio,
ping`) — anything outside fails closed. New classes added to the C++
Kujhad dispatcher MUST be added to that frozenset before they can
ride the RNS transport.

## Dedupe window

Per-peer LRU keyed by `(uid, ts_ms // 1000)`. Sub-second retransmits
are suppressed; deliberate operator re-issues 2 s later go through.
LRU cap = 4096 entries per peer (matches `RNSCotBridge`). Bucket
evicts the oldest entry past the cap.

The `uid` is generated per call by `KujhadRNSClient` via `uuid.uuid4`,
giving ample collision resistance. Callers that want at-most-once
semantics across long RNS retransmit windows should reuse the same
uid for the retry.

## Routing — aspects are not aliases

Reticulum Destination hashes are computed over the full aspect path
(`appname.aspect_path`), so `predatorrf.cot.v1` and
`predatorrf.cmd.v1` resolve to **different** delivery targets even
when they share an Identity. Reusing the cot.v1 OUT destination for
cmd publish would land the envelope on the peer's cot.v1 packet
callback, which silently drops anything that isn't a CoT envelope.

The cot.v1 announce handler therefore builds **both** an OUT
destination on cot.v1 and an OUT destination on cmd.v1 per learned
peer (`_peers[h16]["destination"]` vs `["cmd_destination"]`).
`_publish_envelope_cmd` uses the cmd one and skips peers where the
cmd OUT couldn't be built. We piggyback on the cot.v1 announce stream
because every fielded peer already announces cot.v1; a future cmd.v1
announce handler will be needed if cmd.v1-only peers ever appear.

## Transport selection

Mirrors cot.v1: opportunistic Packet for envelopes ≤ `PACKET_MDU`
(460 B), Link/Resource for everything bigger OR when the
per-interface `reliable_cot` config flag is set. `KujhadRNSClient`
defaults `reliable=True` — commands are tiny and operator-initiated,
so paying the Link round-trip cost for delivery confirmation is the
right trade-off; cot.v1 keeps its opportunistic Packet default.

## Config flag

`config["cmd_v1_enabled"]` (default `False`). When false:

* No `predatorrf/cmd.v1` IN destination is created.
* `RNSCmdBridge._publish_fn` is never bound, so any
  `KujhadRNSClient.send_*_command` call returns `False` (no transport
  bound).
* Non-upgraded peers that announce cmd.v1 are silently ignored —
  the announce handler runs on the cot.v1 aspect only.

When flipped on at daemon `start()`:

* Second IN destination registered + announced.
* `cmd_bridge.set_publish_fn(self._publish_envelope_cmd)` binds the
  Controller-side path.
* `stop()` unbinds the publish_fn so a re-start with the flag flipped
  off cleanly disables tasking.

## Diagnostics

`RNSCmdBridge.stats()` mirrors `RNSCotBridge.stats()`:

```python
{ "published": int, "received": int,
  "dispatched": int,           # dispatcher returned True
  "dispatch_rejected": int,    # dispatcher returned False or raised
  "deduped": int, "loop_suppressed": int,
  "allowlist_rejected": int, "envelope_errors": int,
  "peers_seen": int, "dedupe_table_size": int,
  "own_hash16": str, "allowlist_size": int,
  "reliable_default": bool }
```

Healthy steady state: `published` ≈ Controller send count;
`dispatched` ≈ Device executed-ok count; `envelope_errors == 0`
(non-zero indicates a Controller-side `tx.*` attempt — programming
bug); `loop_suppressed` should be 0 when peers are correctly
addressed (non-zero indicates the local daemon is receiving its own
broadcast back, usually means peer registry is empty and the
fall-back local-IN-destination path is being used).

## Test surface

`python -m unittest backend.tests.test_rns_cmd -v` — 29 tests:

* **CmdEnvelopeTests** (10) — round-trip, tx.* on wrap and unwrap,
  unknown class, missing fields, bad src/uid, version mismatch, wrong
  content-type, oversized→Link prediction.
* **RNSCmdBridgeTests** (11) — round-trip, loop suppression, dedupe
  window, allowlist (block + pass), dispatcher accept / reject /
  exception, no-publisher, no-dispatcher, tx.* refusal at publish,
  bad envelope.
* **KujhadRNSClientParityTests** (5) — wire-body byte-identity to
  `KujhadClient` for tune/scan(start)/scan(stop)/mission, plus bad
  peer-hash refusal.
* **DaemonWireUpTests** (3) — flag-off does NOT bind cmd publish;
  flag-on DOES bind; `stop()` unbinds.
* **Source-authority tests** (2) — packet src wins on mismatch;
  envelope-only path works when daemon couldn't extract a packet src
  (unit-test backdoor only).

## Production wire-up (not yet integrated in `backend/main.py`)

The bridge, sender, and daemon hooks all exist; what remains for
field deployment is creating one shared `RNSCmdBridge` in
`backend/main.py` next to the existing `RNSCotBridge`, passing it
into `RNSDaemon(..., cmd_bridge=...)`, calling
`bridge.set_dispatch_fn(<callable that invokes the same handler the
Kujhad HTTP /v1/command endpoint uses>)`, calling
`bridge.set_allowlist(daemon.config["peer_allowlist"])`, and
constructing `KujhadRNSClient(bridge)` for the TOC tasking path.
Until that wire-up lands, `cmd_v1_enabled=True` is harmless: the
bridge will count `received` envelopes but report `dispatched=0`
because no dispatcher is bound — cmd.v1 is opt-in for exactly this
reason.

### Per-peer addressing — strict unicast

`KujhadRNSClient.send_*_command(peer_h16, …)` is strict-unicast
end-to-end. The bridge forwards `peer_h16` to
`RNSDaemon._publish_envelope_cmd`, which addresses the peer's
cmd.v1 OUT destination directly and **fails closed** if the peer
is unknown or has no cmd OUT (returns False all the way back up
to the caller). There is **no broadcast fall-back** — a misaddressed
`tune 433.92 MHz` cannot retune peers that weren't supposed to be
tasked. The `RNSDaemon.send_to_peer_cmd(h16, env, reliable)` helper
exposes the same gate for code paths that already have an envelope
(e.g. retransmit on Link failure).
