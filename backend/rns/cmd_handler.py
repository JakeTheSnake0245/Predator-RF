"""Device-side dispatcher for `predatorrf/cmd.v1` envelopes.

Mirrors `RNSCotBridge` (backend/rns/bridge.py) point-for-point — same
own_hash16 contract, same per-peer LRU dedupe, same allowlist
enforcement, same loop suppression — but for command envelopes instead
of CoT XML.

The dispatcher is deliberately import-light: it does NOT import RNS
itself. The daemon owns the `cmd.v1` Destination and calls into the
bridge via `publish` / `handle_inbound`. This keeps unit tests fast
and mirrors the bridge.py architecture.

Inbound command flow:

    daemon._on_cmd_packet(bytes, packet)
        → bridge.handle_inbound(env_bytes, src_hash16)
            → unwrap_cmd() validates RX-only + allowlist
            → loop / allowlist / dedupe checks
            → dispatch_fn({"class","action","args"}, src_hash16, uid)

`dispatch_fn` is supplied by the backend wire-up and SHOULD be the
same callable that the Kujhad HTTP `/v1/command` handler invokes, so
both transports converge on a single execution path (single audit
trail, single rate-limit / quota surface, single failure mode).
"""
from __future__ import annotations

import logging
import time
from collections import OrderedDict
from typing import Any, Callable, Dict, Iterable, Optional, Tuple

from .cmd import CmdEnvelopeError, unwrap_cmd, wrap_cmd

logger = logging.getLogger(__name__)

# Type alias for the backend-supplied dispatcher.
DispatchFn = Callable[[Dict[str, Any], str, str], bool]
"""dispatch_fn(cmd_dict, src_hash16, uid) → bool

Returns True when the command was accepted by the local execution path
(same semantics as Kujhad HTTP /v1/command's `ok=True`); False when
the command was rejected for non-transport reasons (unknown action,
invalid args, hardware busy, …). Transport-level rejects (bad
envelope, dedupe, allowlist) never reach the dispatcher.
"""


class RNSCmdBridge:
    """Single-direction RNS bridge for tasking commands."""

    DEDUPE_LIMIT = 4096

    def __init__(
        self,
        *,
        own_hash16: str,
        publish_fn: Optional[Callable[..., None]] = None,
        dispatch_fn: Optional[DispatchFn] = None,
        peer_allowlist: Optional[Iterable[str]] = None,
        reliable_default: bool = True,
    ) -> None:
        if len(own_hash16) != 16:
            raise ValueError("own_hash16 must be 16 hex chars")
        self.own_hash16 = own_hash16
        self._publish_fn = publish_fn
        self._dispatch_fn = dispatch_fn
        # Commands default to reliable (Link/Resource) — they're tiny
        # and operator-initiated, so the cost of a Link round-trip is
        # paid willingly in exchange for delivery confirmation.
        self.reliable_default = bool(reliable_default)
        self._dedupe: "Dict[str, OrderedDict[Tuple[str, int], float]]" = {}
        self._allowlist: set = set(
            (h or "").lower() for h in (peer_allowlist or []))
        # Telemetry counters (mirrors RNSCotBridge.stats() shape).
        self.published = 0
        self.received = 0
        self.dispatched = 0
        self.dispatch_rejected = 0
        self.deduped = 0
        self.loop_suppressed = 0
        self.allowlist_rejected = 0
        self.envelope_errors = 0

    # ── wiring setters (used by daemon) ────────────────────────────────

    def set_publish_fn(self, fn: Optional[Callable[..., None]]) -> None:
        self._publish_fn = fn

    def set_dispatch_fn(self, fn: Optional[DispatchFn]) -> None:
        self._dispatch_fn = fn

    def set_allowlist(self, peers: Iterable[str]) -> None:
        self._allowlist = set((h or "").lower() for h in peers)

    # ── outbound (Controller side) ─────────────────────────────────────

    def publish(self, cmd: Dict[str, Any], uid: str,
                peer_h16: Optional[str] = None,
                reliable: Optional[bool] = None) -> bool:
        """Wrap and strict-unicast one command to `peer_h16`.

        Returns True when the publish_fn accepted the envelope; False
        on tx.*/schema rejection at wrap, no publish_fn bound, or
        publish_fn rejection (e.g. unknown peer).
        """
        if self._publish_fn is None:
            return False
        try:
            env = wrap_cmd(cmd, src_hash16=self.own_hash16, uid=uid)
        except CmdEnvelopeError as exc:
            logger.error("RNSCmdBridge.publish refused %r: %s", cmd, exc)
            self.envelope_errors += 1
            return False
        rel = self.reliable_default if reliable is None else bool(reliable)
        try:
            # Try (env, rel, peer) signature first; fall back for legacy
            # / test publish_fns. Production daemon uses the new shape.
            try:
                ok = self._publish_fn(env, rel, peer_h16)
            except TypeError:
                try:
                    ok = self._publish_fn(env, rel)
                except TypeError:
                    ok = self._publish_fn(env)
        except Exception as exc:
            logger.warning("RNSCmdBridge.publish failed for uid=%s: %s",
                           uid, exc)
            return False
        if ok is False:
            return False
        self.published += 1
        return True

    # ── inbound (Device side) ──────────────────────────────────────────

    def _dedupe_seen(self, src: str, uid: str, ts_ms: int) -> bool:
        peer_key = (src or "").lower() or "_unknown"
        bucket = self._dedupe.get(peer_key)
        if bucket is None:
            bucket = OrderedDict()
            self._dedupe[peer_key] = bucket
        # Bucket key uses 1-second resolution mirror cot.v1 semantics:
        # a retransmitted command within the same wall-clock second is
        # dropped, but a deliberate operator re-issue 2 s later goes
        # through. Combined with a unique `uid` per Controller-side
        # call, dupes are the only thing that ever collides.
        key = (uid, ts_ms // 1000)
        if key in bucket:
            bucket.move_to_end(key)
            return True
        bucket[key] = time.time()
        if len(bucket) > self.DEDUPE_LIMIT:
            bucket.popitem(last=False)
        return False

    def handle_inbound(self, env_bytes: bytes,
                       src_hash16: Optional[str] = None) -> bool:
        """Decode an inbound envelope and (if it survives dedupe +
        loop + allowlist) dispatch it to the local execution path.

        Returns True only when the dispatcher accepted the command.
        Returns False on every transport-layer drop AND on dispatcher
        rejection — callers can distinguish via the `dispatched` /
        `dispatch_rejected` / etc counters in `stats()`.
        """
        try:
            env = unwrap_cmd(env_bytes)
        except CmdEnvelopeError as exc:
            logger.debug("RNSCmdBridge: drop bad envelope: %s", exc)
            self.envelope_errors += 1
            return False
        # Packet src (transport-authenticated Identity) is authoritative;
        # envelope src is informational and must agree or the envelope
        # is dropped. Envelope-only fallback is unit-test path.
        env_src = (env.get("src") or "").lower()
        pkt_src = (src_hash16 or "").lower()
        if pkt_src:
            if env_src and env_src != pkt_src:
                self.envelope_errors += 1
                logger.warning(
                    "RNSCmdBridge: src mismatch packet=%s envelope=%s "
                    "uid=%s — dropping (possible spoof)",
                    pkt_src, env_src, env.get("uid"))
                return False
            src = pkt_src
        else:
            src = env_src
        if src and src == self.own_hash16.lower():
            self.loop_suppressed += 1
            return False
        if self._allowlist and src not in self._allowlist:
            self.allowlist_rejected += 1
            logger.info("RNSCmdBridge: peer %s not in allowlist; cmd dropped",
                        src)
            return False
        if self._dedupe_seen(src, env["uid"], int(env["ts"])):
            self.deduped += 1
            return False
        self.received += 1
        if self._dispatch_fn is None:
            # No dispatcher wired yet — count as received but not
            # dispatched. (Daemon may still be coming up.)
            return False
        try:
            ok = bool(self._dispatch_fn(env["cmd"], src, env["uid"]))
        except Exception as exc:
            logger.warning(
                "RNSCmdBridge dispatcher raised on uid=%s cmd=%r: %s",
                env["uid"], env["cmd"], exc)
            self.dispatch_rejected += 1
            return False
        if ok:
            self.dispatched += 1
        else:
            self.dispatch_rejected += 1
        return ok

    def stats(self) -> Dict[str, Any]:
        return {
            "published": self.published,
            "received": self.received,
            "dispatched": self.dispatched,
            "dispatch_rejected": self.dispatch_rejected,
            "deduped": self.deduped,
            "loop_suppressed": self.loop_suppressed,
            "allowlist_rejected": self.allowlist_rejected,
            "envelope_errors": self.envelope_errors,
            "peers_seen": len(self._dedupe),
            "dedupe_table_size": sum(len(b) for b in self._dedupe.values()),
            "own_hash16": self.own_hash16,
            "allowlist_size": len(self._allowlist),
            "reliable_default": self.reliable_default,
        }
