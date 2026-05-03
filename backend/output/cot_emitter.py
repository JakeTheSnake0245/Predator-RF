"""
Cursor-on-Target (CoT) UDP transmitter for TAK / ATAK / WinTAK / iTAK.

Builds a standards-compliant CoT 2.0 XML beacon for each persisted emitter
track that the operator chooses to escalate, and sends it via UDP to the
configured destination (multicast 239.2.3.1:6969 by default, the TAK
standard "SA" feed).

Operator gating
---------------
Two-key gate. A beacon is sent only when **both** are true:
1. `config.cot_enabled` is set (operator-level kill switch).
2. The track's most recent AssessmentReport has `escalate_to_atak=True`.

This keeps the platform RX-by-default — nothing leaves the box without an
explicit operator decision via the config flag, even when the intelligence
layer thinks a track is high-threat.

CoT type selection
------------------
* Geolocated emitter (we have a TDOA fix) → `a-u-G` (unknown ground unit).
* Un-geolocated emitter (frequency-only contact) → `b-m-p-s-p-loc` (point
  of interest at the most-trustworthy node's location).
"""
from __future__ import annotations

import asyncio
import logging
import socket
import struct
import time
from datetime import datetime, timedelta, timezone
from typing import Callable, List, Optional, Tuple
from xml.sax.saxutils import escape as _xmlesc

logger = logging.getLogger(__name__)


# ── XML builder ──────────────────────────────────────────────────────────

def _iso(ts: datetime) -> str:
    """CoT time format: ISO-8601 with millisecond precision and 'Z' tz."""
    return ts.strftime("%Y-%m-%dT%H:%M:%S.") + f"{ts.microsecond // 1000:03d}Z"


def build_cot_xml(*,
                  uid: str,
                  lat: float,
                  lon: float,
                  cot_type: str = "a-u-G",
                  callsign: str = "EMITTER",
                  ce_meters: float = 9_999_999.0,
                  hae_meters: float = 9_999_999.0,
                  le_meters: float = 9_999_999.0,
                  stale_seconds: float = 300.0,
                  remarks: str = "",
                  how: str = "m-g") -> bytes:
    """Construct a CoT 2.0 event XML datagram. Returns UTF-8 bytes ready
    for `socket.sendto`. All string inputs are XML-escaped."""
    now = datetime.now(timezone.utc)
    stale = now + timedelta(seconds=max(1.0, stale_seconds))
    parts = [
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
        '<event version="2.0"'
        f' uid="{_xmlesc(uid)}"'
        f' type="{_xmlesc(cot_type)}"'
        f' time="{_iso(now)}"'
        f' start="{_iso(now)}"'
        f' stale="{_iso(stale)}"'
        f' how="{_xmlesc(how)}">',
        f'<point lat="{lat:.7f}" lon="{lon:.7f}"'
        f' hae="{hae_meters:.1f}" ce="{ce_meters:.1f}" le="{le_meters:.1f}"/>',
        '<detail>',
        f'<contact callsign="{_xmlesc(callsign)}"/>',
        f'<remarks>{_xmlesc(remarks)}</remarks>',
        '<__group name="Cyan" role="Team Member"/>',
        '<precisionlocation altsrc="???" geopointsrc="GPS"/>',
        '</detail>',
        '</event>',
    ]
    return "".join(parts).encode("utf-8")


# ── UDP transport ────────────────────────────────────────────────────────

class CoTEmitter:
    """Async, gated CoT UDP emitter. Holds a single non-blocking UDP
    socket and writes datagrams from the asyncio loop via `to_thread`
    (sendto is sync but typically buffered by the kernel)."""

    def __init__(self, *,
                 dest_host: str = "239.2.3.1",
                 dest_port: int = 6969,
                 enabled: bool = False,
                 uid_prefix: str = "PREDATOR",
                 multicast_ttl: int = 1,
                 stale_seconds: float = 300.0):
        self.dest_host = dest_host
        self.dest_port = int(dest_port)
        self.enabled = bool(enabled)
        self.uid_prefix = uid_prefix
        self.stale_seconds = float(stale_seconds)
        self._sock: Optional[socket.socket] = None
        # Per-emitter rate limit so a chatty source doesn't flood the TAK
        # bus. Maps emitter_id → last-emitted unix seconds.
        self._last_emit: dict[str, float] = {}
        self._min_interval_s = 5.0
        self._sent_count = 0
        self._error_count = 0
        # Optional fan-out hooks. Each hook is called with `(xml_bytes, uid)`
        # for every successfully transmitted CoT datagram. The RNS bridge
        # (backend/rns/bridge.py::RNSCotBridge.publish) is the canonical
        # consumer; tests register a list-appending closure. Hook
        # exceptions are logged and swallowed — never break the TAK feed.
        self._fanout_hooks: List[Callable[[bytes, str], None]] = []
        self._fanout_count = 0

        if self.enabled:
            try:
                self._sock = self._open_socket(multicast_ttl)
                logger.info("CoTEmitter active → %s:%d", dest_host, dest_port)
            except Exception as exc:
                logger.error("CoTEmitter socket open failed: %s — disabling", exc)
                self.enabled = False
                self._sock = None
        else:
            logger.info("CoTEmitter disabled (cot_enabled=false)")

    def _open_socket(self, multicast_ttl: int) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Set multicast TTL only if the destination is a multicast addr
        if self._is_multicast(self.dest_host):
            ttl_bin = struct.pack('b', max(1, min(255, int(multicast_ttl))))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, ttl_bin)
        return sock

    @staticmethod
    def _is_multicast(host: str) -> bool:
        try:
            first = int(host.split('.', 1)[0])
            return 224 <= first <= 239
        except (ValueError, IndexError):
            return False

    # ── Public API ──────────────────────────────────────────────────────

    async def emit_track(self, track_dict: dict, assessment_dict: dict,
                          fallback_location: Optional[Tuple[float, float]] = None
                          ) -> bool:
        """Build + send a CoT for `track_dict`. Returns True if a datagram
        was actually written, False if any gate suppressed it.

        Loop prevention (spec section C): tracks ingested from RNS are
        not re-broadcast over IP unless the operator explicitly opts
        in via `set_rns_to_ip_relay(True)`.

        `fallback_location` is the lat/lon to use when the track has no
        TDOA fix yet — typically the most-trustworthy detecting node's
        position, so the operator at least sees a "somewhere near node X"
        marker in TAK.
        """
        # Gate 0: IP↔RNS loop break (spec section C, default off)
        if not self._emit_allowed_for(track_dict):
            return False

        # Gate 1: operator kill switch
        if not self.enabled or self._sock is None:
            return False

        # Gate 2: assessment must approve escalation
        if not assessment_dict.get("escalate_to_atak"):
            return False

        emitter_id = track_dict.get("emitter_id") or "unknown"

        # Gate 3: per-emitter rate limit
        now_s = time.time()
        last = self._last_emit.get(emitter_id, 0.0)
        if now_s - last < self._min_interval_s:
            return False

        # Resolve location
        lat = track_dict.get("estimated_lat")
        lon = track_dict.get("estimated_lon")
        cot_type = "a-u-G"
        if lat is None or lon is None:
            if fallback_location is None:
                # No TDOA fix and no fallback — suppress (TAK requires a point)
                return False
            lat, lon = fallback_location
            cot_type = "b-m-p-s-p-loc"

        # CE (circular error) ~ 50m at high TDOA confidence, scaling up
        # linearly down to 5km at zero confidence
        loc_conf = float(track_dict.get("location_confidence") or 0.0)
        ce = 50.0 + (1.0 - max(0.0, min(1.0, loc_conf))) * 4_950.0

        freq_mhz = float(track_dict.get("primary_frequency") or 0.0) / 1e6
        threat = assessment_dict.get("threat_level", "unknown").upper()
        callsign = f"{self.uid_prefix}-{emitter_id[:8]}"
        remarks = (
            f"PREDATOR-RF {threat} | "
            f"{freq_mhz:.4f} MHz | "
            f"obs={track_dict.get('observation_count', 0)} | "
            f"conf={track_dict.get('confidence', 0):.2f} | "
            f"{assessment_dict.get('summary', '')}"
        ).strip()

        xml = build_cot_xml(
            uid=f"{self.uid_prefix}.{emitter_id}",
            lat=lat, lon=lon,
            cot_type=cot_type,
            callsign=callsign,
            ce_meters=ce,
            stale_seconds=self.stale_seconds,
            remarks=remarks,
        )

        try:
            await asyncio.to_thread(
                self._sock.sendto, xml, (self.dest_host, self.dest_port))
        except Exception as exc:
            self._error_count += 1
            logger.warning("CoT send failed for %s: %s", emitter_id, exc)
            return False

        self._last_emit[emitter_id] = now_s
        self._sent_count += 1
        logger.info("CoT sent: %s @ (%.5f, %.5f) → %s:%d",
                    callsign, lat, lon, self.dest_host, self.dest_port)

        # Parallel-transport fan-out (RNS bridge, etc). Each hook is
        # best-effort; failures must not break the TAK UDP feed.
        cot_uid = f"{self.uid_prefix}.{emitter_id}"
        for hook in list(self._fanout_hooks):
            try:
                hook(xml, cot_uid)
                self._fanout_count += 1
            except Exception as exc:  # pragma: no cover - defensive
                logger.warning("CoT fan-out hook raised: %s", exc)
        return True

    def set_rns_to_ip_relay(self, allow: bool) -> None:
        """When False (default), tracks tagged source_transport='rns'
        are not re-emitted on the TAK UDP feed — this is the IP↔RNS
        loop break point per spec section C."""
        self._rns_to_ip_relay = bool(allow)

    def _emit_allowed_for(self, track_dict: dict) -> bool:
        if getattr(self, "_rns_to_ip_relay", False):
            return True
        st = track_dict.get("source_transport") or ""
        if str(st).lower() == "rns":
            return False
        return True

    def attach_fanout(self, hook: Callable[[bytes, str], None]) -> None:
        """Register a `(xml_bytes, uid)` callback invoked after every
        successful CoT UDP send. Used to publish the same XML over
        parallel transports (RNS in particular)."""
        self._fanout_hooks.append(hook)

    def detach_fanout(self, hook: Callable[[bytes, str], None]) -> None:
        try:
            self._fanout_hooks.remove(hook)
        except ValueError:
            pass

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None

    def stats(self) -> dict:
        return {
            "enabled": self.enabled,
            "destination": f"{self.dest_host}:{self.dest_port}",
            "sent": self._sent_count,
            "errors": self._error_count,
            "tracked_emitters": len(self._last_emit),
        }
