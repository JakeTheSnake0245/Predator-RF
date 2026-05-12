"""Controller-side sender that mirrors KujhadClient command shape but
ships over RNS instead of HTTP.

Roadmap #6 — RNS commanding wrapper. The four `send_*_command`
methods on `KujhadClient` (`send_tune_command`, `send_scan_command`,
`send_mission_command`) are recreated here with identical signatures
and identical wire-payload shapes so callers can swap transports
without re-thinking arg conventions:

    # IP path:
    await ip_client.send_tune_command(433_920_000)
    # RNS path (same args, same wire-body):
    rns_client.send_tune_command(peer_h16, 433_920_000)

The wire body is byte-identical to the JSON Kujhad HTTP receives, so
on the Device side the same dispatcher can handle both transports
(`backend/rns/cmd_handler.py` calls into the same execution path the
HTTP server already uses).

Differences from `KujhadClient`:

  * Sync, not async. `RNSCmdBridge.publish` ultimately calls into the
    daemon's `_publish_envelope` analog which is itself sync (RNS does
    its own background scheduling). Wrapping that in `await
    asyncio.to_thread(...)` is the caller's choice — most TOC code
    paths that send commands are already inside an async context but a
    thin sync sender keeps this module trivially testable.
  * No persistent session — every `send_*` is a fresh wrap+publish.
  * Per-peer addressing is delegated to the daemon's peer registry; we
    record the target `peer_h16` only so the dispatched command can be
    tagged in the audit trail. The actual RNS Destination lookup
    happens inside `_publish_fn`.

The bridge instance is supplied at construction; production wire-up
in `backend/main.py` will create a single `RNSCmdBridge` shared with
the `RNSDaemon`, and pass it into both this client and the daemon.
"""
from __future__ import annotations

import logging
import time
import uuid
from typing import Any, Dict, Optional

from backend.rns.cmd_handler import RNSCmdBridge

logger = logging.getLogger(__name__)


def _new_uid() -> str:
    """Per-call unique-id used by the Device-side dedupe LRU. uuid4
    gives ample collision resistance vs the (uid, sec) bucket key
    used by `RNSCmdBridge._dedupe_seen`."""
    return uuid.uuid4().hex


class KujhadRNSClient:
    """Send Kujhad-shape tasking commands over the cmd.v1 RNS aspect."""

    def __init__(self, bridge: RNSCmdBridge) -> None:
        self._bridge = bridge

    # ── identical signatures to KujhadClient.send_*_command ────────────

    def send_tune_command(self, peer_h16: str, frequency_hz: float,
                          vfo: str = "VFO A") -> bool:
        """Task a peer to tune to a frequency.

        Wire body matches `KujhadClient.send_tune_command` exactly:
            {"class":"tune","action":"set",
             "args":{"frequencyHz":Hz,"vfo":vfo}}
        """
        payload = {
            "class": "tune",
            "action": "set",
            "args": {"frequencyHz": float(frequency_hz), "vfo": vfo},
        }
        return self._publish(peer_h16, payload)

    def send_scan_command(self, peer_h16: str, freq_start_hz: float,
                          freq_end_hz: float, dwell_ms: int = 500,
                          start: bool = True) -> bool:
        """Task a peer to start (or stop) a frequency scan.

        Wire body matches `KujhadClient.send_scan_command` exactly:
            {"class":"scan","action":"start"|"stop",
             "args":{"startHz":...,"endHz":...,"dwellMs":...}}
        """
        payload = {
            "class": "scan",
            "action": "start" if start else "stop",
            "args": {
                "startHz": float(freq_start_hz),
                "endHz":   float(freq_end_hz),
                "dwellMs": int(dwell_ms),
            },
        }
        return self._publish(peer_h16, payload)

    def send_mission_command(self, peer_h16: str, action: str,
                             args: Dict[str, Any]) -> bool:
        """Task a peer with a mission.* command.

        action ∈ {setMode, setSearchBands, setTargets, setExcludes,
                  setSettings} — see main_window.cpp:1941-1967.
        """
        payload = {"class": "mission", "action": action,
                   "args": dict(args or {})}
        return self._publish(peer_h16, payload)

    # ── internals ──────────────────────────────────────────────────────

    def _publish(self, peer_h16: str, payload: Dict[str, Any],
                 *, reliable: Optional[bool] = None) -> bool:
        """Wrap + strict-unicast via the shared bridge. `peer_h16` is
        load-bearing: the daemon fails closed on unknown peers and
        there is no broadcast fall-back."""
        if not isinstance(peer_h16, str) or len(peer_h16) != 16:
            logger.error(
                "KujhadRNSClient: bad peer_h16=%r — expected 16 hex chars",
                peer_h16)
            return False
        uid = _new_uid()
        t0 = time.time()
        ok = self._bridge.publish(payload, uid=uid, peer_h16=peer_h16,
                                  reliable=reliable)
        logger.info(
            "KujhadRNSClient send class=%s action=%s peer=%s uid=%s "
            "ok=%s in %.1fms",
            payload.get("class"), payload.get("action"), peer_h16, uid,
            ok, (time.time() - t0) * 1000.0)
        return ok
