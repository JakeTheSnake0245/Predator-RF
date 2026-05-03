"""CoT ↔ RNS bridge.

This is the in-process glue between `backend/output/cot_emitter.py` and
the RNS daemon. It has two halves:

  * `RNSCotBridge.publish(xml, uid)` — called by the CoT emitter for
    every emitted XML beacon. Wraps in the CBOR envelope and ships out
    over the `predatorrf/cot.v1` Destination.
  * `RNSCotBridge.handle_inbound(envelope_bytes, src_hash16)` — called
    by the RNS receive path for every inbound CBOR envelope. Performs
    dedupe + loop suppression and feeds the XML to the local CoC
    aggregator with `source_transport="rns"`.

The bridge is deliberately import-light: it does NOT import RNS itself.
The daemon owns the RNS Destination/Identity and calls into the bridge
via the two methods above. This keeps unit tests fast and lets the
backend run with `cot_enabled=True` even when the operator hasn't
installed the `rns` Python package yet.
"""
from __future__ import annotations

import logging
import time
from collections import OrderedDict
from typing import Callable, Iterable, Optional, Tuple

from .envelope import EnvelopeError, unwrap_cot, wrap_cot

logger = logging.getLogger(__name__)


class RNSCotBridge:
    """Single-direction RNS bridge for CoT XML.

    `publish_fn(envelope_bytes, reliable: bool)` is supplied by the
    daemon and actually pushes bytes over RNS. When None, `publish` is a
    no-op (used by tests and by the backend when RNS isn't running).
    The `reliable` flag tells the daemon whether to use opportunistic
    Packet (False) or open a short-lived Link / Resource (True). The
    daemon also auto-promotes to Link when the envelope exceeds the
    path MTU regardless of this flag — see section C of the task spec.

    `inbound_fn(xml, src_hash16)` is supplied by the backend (typically
    the CoC aggregator's `feed_event` hook) and is invoked for every
    inbound envelope that survived dedupe + loop + allowlist checks.

    Per-peer dedupe LRU (4096 entries each) per section C.
    Allowlist enforcement per section D.
    """

    DEDUPE_LIMIT = 4096
    PUBLISH_TYPE = Callable[[bytes, bool], None]

    def __init__(self,
                 *,
                 own_hash16: str,
                 publish_fn: Optional[Callable[..., None]] = None,
                 inbound_fn: Optional[Callable[[bytes, str], None]] = None,
                 peer_allowlist: Optional[Iterable[str]] = None,
                 reliable_default: bool = False):
        if len(own_hash16) != 16:
            raise ValueError("own_hash16 must be 16 hex chars")
        self.own_hash16 = own_hash16
        self._publish_fn = publish_fn
        self._inbound_fn = inbound_fn
        self.reliable_default = bool(reliable_default)
        # Per-peer LRU. Key: src_hash16 → OrderedDict[(uid, sec) → ts].
        self._dedupe: "dict[str, OrderedDict[Tuple[str, int], float]]" = {}
        self._allowlist: set[str] = set(
            (h or "").lower() for h in (peer_allowlist or []))
        self.published = 0
        self.received = 0
        self.deduped = 0
        self.loop_suppressed = 0
        self.allowlist_rejected = 0

    def set_publish_fn(self, fn: Optional[Callable[..., None]]) -> None:
        self._publish_fn = fn

    def set_inbound_fn(self, fn: Optional[Callable[[bytes, str], None]]) -> None:
        self._inbound_fn = fn

    def set_allowlist(self, peers: Iterable[str]) -> None:
        self._allowlist = set((h or "").lower() for h in peers)

    # ── outbound ───────────────────────────────────────────────────────

    def publish(self, xml: bytes, uid: str,
                reliable: Optional[bool] = None) -> bool:
        """Wrap and push one CoT XML beacon. Returns True when bytes
        actually went to the publish_fn, False when no publisher was
        bound. `reliable` overrides the bridge default; the daemon may
        further promote opportunistic Packet → Link based on path MTU.
        """
        if self._publish_fn is None:
            return False
        env = wrap_cot(xml, src_hash16=self.own_hash16, uid=uid)
        rel = self.reliable_default if reliable is None else bool(reliable)
        try:
            # Tolerate older single-arg publishers used by some unit tests.
            try:
                self._publish_fn(env, rel)
            except TypeError:
                self._publish_fn(env)
        except Exception as exc:
            logger.warning("RNSCotBridge.publish failed for uid=%s: %s",
                           uid, exc)
            return False
        self.published += 1
        return True

    # ── inbound ────────────────────────────────────────────────────────

    def _dedupe_seen(self, src: str, uid: str, ts_ms: int) -> bool:
        """Per-peer LRU. Returns True if (uid, sec) was already seen
        from this peer (caller should drop)."""
        peer_key = (src or "").lower() or "_unknown"
        bucket = self._dedupe.get(peer_key)
        if bucket is None:
            bucket = OrderedDict()
            self._dedupe[peer_key] = bucket
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
        """Decode an inbound envelope and hand the XML to the local
        bridge. Returns True if the message was forwarded, False if it
        was dropped (loop, dupe, allowlist reject, decode error, no
        inbound handler)."""
        try:
            env = unwrap_cot(env_bytes)
        except EnvelopeError as exc:
            logger.debug("RNSCotBridge: drop bad envelope: %s", exc)
            return False
        src = (env.get("src") or src_hash16 or "").lower()
        # Loop suppression — never re-deliver our own published XML.
        if src and src == self.own_hash16.lower():
            self.loop_suppressed += 1
            return False
        # Allowlist enforcement (empty = open mode, accept any peer).
        if self._allowlist and src not in self._allowlist:
            self.allowlist_rejected += 1
            logger.debug("RNSCotBridge: peer %s not in allowlist", src)
            return False
        if self._dedupe_seen(src, env["uid"], int(env["ts"])):
            self.deduped += 1
            return False
        self.received += 1
        if self._inbound_fn is not None:
            try:
                self._inbound_fn(env["xml"], src)
            except Exception as exc:
                logger.warning(
                    "RNSCotBridge inbound handler raised on uid=%s: %s",
                    env["uid"], exc)
                return False
        return True

    def stats(self) -> dict:
        return {
            "published": self.published,
            "received": self.received,
            "deduped": self.deduped,
            "loop_suppressed": self.loop_suppressed,
            "allowlist_rejected": self.allowlist_rejected,
            "peers_seen": len(self._dedupe),
            "dedupe_table_size": sum(len(b) for b in self._dedupe.values()),
            "own_hash16": self.own_hash16,
            "allowlist_size": len(self._allowlist),
            "reliable_default": self.reliable_default,
        }
