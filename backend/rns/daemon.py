"""RNS daemon — owns the Reticulum stack for Predator RF.

Implements the control API in section F of the task spec:
  status / list_interfaces / get_interface / add_interface / update_interface
  remove_interface / set_enabled / restart_interface / restart_all
  validate_interface / export_config / import_config /
  mint_replication_token / get_logs

Talks to RNS via the upstream `rns` Python package. RNS itself owns
interface lifecycle once we hand it the right config dict, so this
module is mostly a translator between our schema (section B) and the
RNS interface kwargs.

When the `rns` package isn't importable, the daemon still serves the
control API and reports `daemon=stub` from `status()` — this is what
runs under CI / unit tests.
"""
from __future__ import annotations

import json
import logging
import os
import socket
import threading
import time
import uuid
from collections import deque
from typing import Any, Callable, Dict, List, Optional

from .schema import (
    DEVICE_LOCAL_FIELDS,
    SchemaError,
    validate_config,
    validate_interface,
)
from .token import (
    TokenError,
    export_token,
    import_token,
    mint_replication_token,
)
from .bridge import RNSCotBridge

try:  # The daemon must run even when rns isn't installed (CI / tests).
    import RNS  # type: ignore
    _HAVE_RNS = True
except Exception:  # pragma: no cover
    RNS = None  # type: ignore
    _HAVE_RNS = False

logger = logging.getLogger(__name__)


def _default_state_dir() -> str:
    if os.environ.get("PREDATOR_RNS_STATE_DIR"):
        return os.environ["PREDATOR_RNS_STATE_DIR"]
    home = os.path.expanduser("~")
    return os.path.join(home, ".config", "predator-rns")


class _LogTail:
    def __init__(self, max_entries: int = 2048) -> None:
        self._buf: deque = deque(maxlen=max_entries)
        self._lock = threading.Lock()

    def write(self, level: str, msg: str) -> None:
        with self._lock:
            self._buf.append({
                "ts": int(time.time() * 1000),
                "level": level,
                "msg": msg,
            })

    def tail(self, level: str = "INFO",
             since_ms: int = 0,
             limit: int = 200) -> List[Dict[str, Any]]:
        order = {"DEBUG": 0, "INFO": 1, "WARN": 2, "WARNING": 2, "ERROR": 3}
        threshold = order.get(level.upper(), 1)
        with self._lock:
            rows = [r for r in self._buf
                    if r["ts"] >= since_ms
                    and order.get(r["level"], 1) >= threshold]
        return rows[-limit:]


class RNSDaemon:
    """In-process RNS stack manager + control API."""

    def __init__(self, state_dir: Optional[str] = None,
                 cot_bridge: Optional[RNSCotBridge] = None) -> None:
        self.state_dir = state_dir or _default_state_dir()
        os.makedirs(self.state_dir, exist_ok=True)
        self.config_path = os.path.join(self.state_dir, "config.json")
        self.identity_path = os.path.join(self.state_dir, "identity.prv")
        self.cot_bridge = cot_bridge
        self._lock = threading.RLock()
        self._iface_runtime: Dict[str, Dict[str, Any]] = {}
        self._log = _LogTail()
        self._reticulum: Any = None
        self._identity: Any = None
        self._destination: Any = None
        # Known remote peers keyed by 16-hex identity prefix. Each value
        # is {"identity": RNS.Identity, "destination": OUT-Destination,
        #  "iface_id": str | None, "first_seen": float}. Populated by
        # the announce handler registered in start(); consulted by
        # _publish_envelope to fan envelopes out to every known peer.
        self._peers: Dict[str, Dict[str, Any]] = {}
        self._announce_handler: Any = None
        self._running = False
        self.config: Dict[str, Any] = self._load_or_init_config()
        try:
            os.chmod(self.state_dir, 0o700)
        except OSError:
            pass

    # ── config persistence ─────────────────────────────────────────────

    def _load_or_init_config(self) -> Dict[str, Any]:
        if os.path.exists(self.config_path):
            try:
                with open(self.config_path, "r", encoding="utf-8") as f:
                    cfg = json.load(f)
                return validate_config(cfg)
            except (OSError, json.JSONDecodeError, SchemaError) as exc:
                # Per spec: corrupted config rejected with clear error,
                # previous good config kept. Move broken file aside so
                # the operator can inspect; start with empty config.
                broken = self.config_path + ".broken"
                try:
                    os.replace(self.config_path, broken)
                except OSError:
                    pass
                self._log.write("ERROR",
                    f"config load failed ({exc}); broken file moved to {broken}")
        return {"schema_version": 1, "interfaces": [],
                "cot_bridge": {}, "peer_allowlist": []}

    def _save_config(self) -> None:
        tmp = self.config_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.config, f, indent=2, sort_keys=True)
        os.replace(tmp, self.config_path)
        try:
            os.chmod(self.config_path, 0o600)
        except OSError:
            pass

    # ── identity ───────────────────────────────────────────────────────

    def _ensure_identity(self) -> None:
        if not _HAVE_RNS:
            self._identity = None
            return
        if self._identity is not None:
            return
        if os.path.exists(self.identity_path):
            self._identity = RNS.Identity.from_file(self.identity_path)
        if self._identity is None:
            self._identity = RNS.Identity()
            self._identity.to_file(self.identity_path)
            try:
                os.chmod(self.identity_path, 0o600)
            except OSError:
                pass

    def identity_hash16(self) -> str:
        if self._identity is not None and _HAVE_RNS:
            try:
                return RNS.hexrep(self._identity.hash, delimit=False)[:16]
            except Exception:
                pass
        # Stable per-state-dir fallback so the bridge always has a 16-hex
        # source tag even when RNS is absent.
        seed = os.path.join(self.state_dir, "identity.prv")
        h = abs(hash(seed))
        return f"{h:016x}"[:16]

    # ── lifecycle ──────────────────────────────────────────────────────

    def start(self) -> Dict[str, Any]:
        with self._lock:
            if self._running:
                return self.status()
            self._ensure_identity()
            if _HAVE_RNS:
                try:
                    self._reticulum = RNS.Reticulum(
                        configdir=os.path.join(self.state_dir, "reticulum"),
                        loglevel=RNS.LOG_INFO)
                except Exception as exc:
                    self._log.write("ERROR", f"RNS init failed: {exc}")
                    self._reticulum = None
                # Build a SINGLE destination for the cot.v1 aspect.
                if self._reticulum is not None:
                    try:
                        self._destination = RNS.Destination(
                            self._identity, RNS.Destination.IN,
                            RNS.Destination.SINGLE,
                            "predatorrf", "cot.v1")
                        self._destination.set_packet_callback(
                            self._on_packet)
                        self._destination.announce()
                    except Exception as exc:
                        self._log.write(
                            "ERROR", f"destination create failed: {exc}")
                    # Register an announce handler on the same aspect so
                    # we learn remote peer identities and can build OUT
                    # destinations to send envelopes back to them. RNS
                    # delivery is per-destination — sending to our own IN
                    # destination only loops locally; peer delivery
                    # requires an OUT/SINGLE destination per remote
                    # identity. Ref: RNS docs §Identity / §Destination.
                    try:
                        self._announce_handler = self._make_announce_handler()
                        RNS.Transport.register_announce_handler(
                            self._announce_handler)
                    except Exception as exc:
                        self._log.write(
                            "WARN", f"announce handler registration failed: {exc}")
            for entry in self.config.get("interfaces", []):
                if entry.get("enabled", True):
                    self._spawn_interface(entry)
            if self.cot_bridge is not None:
                self.cot_bridge.set_publish_fn(self._publish_envelope)
            self._running = True
            self._log.write("INFO", f"daemon started (rns={_HAVE_RNS})")
        return self.status()

    def stop(self) -> None:
        with self._lock:
            if not self._running:
                return
            for iid in list(self._iface_runtime):
                self._teardown_interface(iid, drain_s=2.0)
            if self.cot_bridge is not None:
                self.cot_bridge.set_publish_fn(None)
            self._running = False
            self._log.write("INFO", "daemon stopped")

    # ── RNS plumbing ───────────────────────────────────────────────────

    def _make_announce_handler(self) -> Any:
        """Construct an RNS announce handler that learns remote peer
        identities advertising the same `predatorrf.cot.v1` aspect.

        For each newly-seen identity we build an OUT/SINGLE destination
        and stash it in `self._peers` keyed by the 16-hex hash prefix
        used elsewhere in the bridge. Peer-allowlist enforcement is
        applied here too — peers outside the allowlist are dropped at
        learn time so we never build an OUT destination to them.
        """
        daemon = self

        class _Handler:
            aspect_filter = "predatorrf.cot.v1"

            def received_announce(self, destination_hash, announced_identity,
                                  app_data=None):
                try:
                    h16 = RNS.hexrep(announced_identity.hash,
                                     delimit=False)[:16].lower()
                except Exception:
                    return
                if h16 == daemon.identity_hash16().lower():
                    return  # our own announce loop
                allow = set(
                    (h or "").lower()
                    for h in daemon.config.get("peer_allowlist", []))
                if allow and h16 not in allow:
                    daemon._log.write(
                        "INFO",
                        f"peer announce ignored (not in allowlist): {h16}")
                    return
                if h16 in daemon._peers:
                    return
                try:
                    out = RNS.Destination(
                        announced_identity, RNS.Destination.OUT,
                        RNS.Destination.SINGLE, "predatorrf", "cot.v1")
                except Exception as exc:
                    daemon._log.write(
                        "WARN", f"could not build OUT dest for {h16}: {exc}")
                    return
                # Best-effort: tag with the interface that delivered the
                # announce so per-interface `reliable_cot` decisions can
                # use it during publish.
                iface_id = None
                try:
                    if hasattr(announced_identity, "_announce_interface"):
                        ifc = announced_identity._announce_interface
                        for iid, rt in daemon._iface_runtime.items():
                            if rt.get("iface") is ifc:
                                iface_id = iid
                                break
                except Exception:
                    pass
                daemon._peers[h16] = {
                    "identity": announced_identity,
                    "destination": out,
                    "iface_id": iface_id,
                    "first_seen": time.time(),
                }
                daemon._log.write("INFO",
                                  f"learned peer {h16} via iface={iface_id}")

        return _Handler()

    def _peer_reliable(self, peer_meta: Dict[str, Any],
                       requested: bool) -> bool:
        """Resolve the reliable flag for one outbound delivery. Per spec
        section C the `reliable_cot` toggle is per-interface, so the
        interface that learned the peer wins; bridge-level / call-level
        `requested` flag is the fallback when no per-interface override
        applies."""
        iid = peer_meta.get("iface_id")
        if iid:
            for entry in self.config.get("interfaces", []):
                if entry.get("id") == iid and "reliable_cot" in entry:
                    return bool(entry["reliable_cot"])
        return bool(requested)

    def _poll_iface_runtime(self, rt: Dict[str, Any]) -> None:
        """Refresh a runtime entry from its live RNS interface object.

        RNS interfaces don't share a uniform stats API, so we probe a
        handful of well-known attributes and fall back to whatever was
        last cached. This makes the Kujhad UI's status panel reflect
        what the stack is actually doing rather than what we recorded
        once at spawn time.
        """
        iface = rt.get("iface")
        if iface is None:
            return
        # `online` (or `OUT`) is the de-facto up/down flag in RNS.
        for attr in ("online", "online_state", "is_online"):
            if hasattr(iface, attr):
                try:
                    rt["up"] = bool(getattr(iface, attr))
                    break
                except Exception:
                    pass
        else:
            for attr in ("OUT", "outgoing"):
                if hasattr(iface, attr):
                    try:
                        rt["up"] = bool(getattr(iface, attr))
                        break
                    except Exception:
                        pass
        for src, dst in (("rxb", "bytes_in"), ("txb", "bytes_out"),
                         ("ifac_size", None), ("clients", "peers")):
            if hasattr(iface, src) and dst is not None:
                try:
                    rt[dst] = int(getattr(iface, src) or 0)
                except Exception:
                    pass
        for attr in ("bitrate", "bitrate_kbps"):
            if hasattr(iface, attr):
                try:
                    v = int(getattr(iface, attr) or 0)
                    rt["bitrate_bps"] = v * (1000 if attr.endswith("kbps")
                                              else 1)
                    break
                except Exception:
                    pass
        last_err = getattr(iface, "last_error", None) or \
                   getattr(iface, "error", None)
        if last_err:
            rt["last_error"] = str(last_err)

    def _on_packet(self, data: bytes, packet: Any) -> None:
        if self.cot_bridge is None:
            return
        try:
            src = ""
            try:
                src = RNS.hexrep(packet.source_hash, delimit=False)[:16]
            except Exception:
                pass
            self.cot_bridge.handle_inbound(bytes(data), src)
        except Exception as exc:
            self._log.write("WARN", f"inbound packet drop: {exc}")

    # Conservative MTU. RNS Packet payload is bounded by MDU (~ 460B
    # in current upstream); above that we open a short-lived Link and
    # stream the envelope as a Resource. Operators can also force Link
    # mode per-interface via the `reliable_cot` flag (spec section C).
    PACKET_MDU = 460

    def _publish_envelope(self, env_bytes: bytes,
                           reliable: bool = False) -> None:
        # When RNS is up, every known peer (learned from announces on the
        # `predatorrf.cot.v1` aspect) receives a copy of the envelope via
        # its own OUT destination. Each per-peer delivery picks Packet
        # (opportunistic) vs Link/Resource (reliable or oversize) per
        # spec section C, with per-interface `reliable_cot` overriding
        # the call-level `reliable` argument.
        #
        # When no peers have been learned yet (e.g. the local daemon has
        # only just started), we still send one Packet to our own IN
        # destination. RNS will route it via Transport — useful for
        # opportunistic flooding on AutoInterface segments and required
        # by the daemon-link unit tests.
        if not _HAVE_RNS:
            return
        targets = []
        for h16, meta in self._peers.items():
            targets.append((h16, meta["destination"],
                            self._peer_reliable(meta, reliable)))
        if not targets and self._destination is not None:
            targets.append((self.identity_hash16(), self._destination,
                            bool(reliable)))
        for h16, dest, rel in targets:
            self._send_one(env_bytes, dest, rel, h16)

    def _send_one(self, env_bytes: bytes, dest: Any, reliable: bool,
                  peer_tag: str) -> None:
        use_link = bool(reliable) or len(env_bytes) > self.PACKET_MDU
        if not use_link:
            try:
                packet = RNS.Packet(dest, env_bytes)
                packet.send()
                return
            except Exception as exc:
                self._log.write(
                    "WARN", f"publish (packet) failed for {peer_tag}: {exc}")
                return
        # Reliable / oversize path: open a Link, stream the envelope as
        # one Resource, tear down. Failures are logged and swallowed —
        # the parallel TAK UDP feed remains the operator's hard guarantee.
        try:
            link = RNS.Link(dest)
            try:
                RNS.Resource(env_bytes, link)
            except Exception as exc:
                self._log.write(
                    "WARN", f"resource send failed for {peer_tag}: {exc}")
            try:
                link.teardown()
            except Exception:
                pass
        except Exception as exc:
            self._log.write(
                "WARN", f"publish (link) failed for {peer_tag}: {exc}")

    def _spawn_interface(self, entry: Dict[str, Any]) -> None:
        iid = entry["id"]
        runtime = {
            "id": iid,
            "name": entry["name"],
            "type": entry["type"],
            "enabled": entry.get("enabled", True),
            "up": False,
            "peers": 0,
            "bytes_in": 0,
            "bytes_out": 0,
            "last_error": "",
            "started_at": time.time(),
        }
        if _HAVE_RNS and self._reticulum is not None:
            try:
                runtime["up"] = self._build_rns_interface(entry)
            except Exception as exc:
                runtime["last_error"] = str(exc)
                self._log.write(
                    "ERROR", f"interface {entry['name']} failed: {exc}")
        else:
            # Stub mode: mark up=False but record we tried.
            runtime["last_error"] = "rns module not available (stub mode)"
        self._iface_runtime[iid] = runtime

    def _build_rns_interface(self, entry: Dict[str, Any]) -> bool:
        """Map our schema entry to an RNS interface and add it to the
        running stack. Returns True on success.

        Each branch builds the interface directly via the RNS interfaces
        module and registers it with the running stack. RNS doesn't have
        a single uniform "create interface from kwargs" API across
        versions, so we localize the branching here and surface failures
        as `last_error` rather than raising past the daemon control API.
        """
        if not _HAVE_RNS:
            return False
        from RNS.Interfaces import (  # type: ignore
            TCPInterface, UDPInterface, AutoInterface, RNodeInterface,
            KISSInterface, AX25KISSInterface, PipeInterface, I2PInterface,
        )
        t = entry["type"]
        owner = self._reticulum
        name = entry["name"]
        iface = None
        if t == "tcp_client":
            iface = TCPInterface.TCPClientInterface(
                owner, name, entry["target_host"], entry["target_port"],
                kiss_framing=entry.get("kiss_framing", False),
                i2p_tunneled=entry.get("i2p_tunneled", False))
        elif t == "tcp_server":
            iface = TCPInterface.TCPServerInterface(
                owner, name,
                entry.get("listen_address", "0.0.0.0"),
                entry["listen_port"],
                prefer_ipv6=entry.get("prefer_ipv6", False),
                i2p_tunneled=entry.get("i2p_tunneled", False))
        elif t == "udp":
            iface = UDPInterface.UDPInterface(
                owner, name,
                entry.get("listen_address", "0.0.0.0"),
                entry["listen_port"],
                entry.get("forward_address"),
                entry.get("forward_port"))
        elif t == "auto_interface":
            iface = AutoInterface.AutoInterface(
                owner, name,
                group_id=entry.get("group_id"),
                discovery_scope=entry.get("discovery_scope", "link"),
                discovery_port=entry.get("discovery_port"),
                data_port=entry.get("data_port"),
                allowed_interfaces=entry.get("allowed_interfaces"),
                ignored_interfaces=entry.get("ignored_interfaces"))
        elif t == "rnode":
            iface = RNodeInterface.RNodeInterface(
                owner, name, entry["port"],
                entry["frequency_hz"], entry["bandwidth_hz"],
                entry["txpower_dbm"], entry["spreadingfactor"],
                entry["codingrate"],
                flow_control=entry.get("flow_control", False),
                id_callsign=entry.get("id_callsign"),
                id_interval=entry.get("id_interval_s"))
        elif t == "kiss_tnc":
            iface = KISSInterface.KISSInterface(
                owner, name, entry["port"], entry["speed_baud"],
                entry.get("databits", 8), entry.get("parity", "none"),
                entry.get("stopbits", 1),
                entry.get("preamble_ms", 150),
                entry.get("txtail_ms", 10),
                entry.get("persistence", 200),
                entry.get("slottime_ms", 20),
                flow_control=entry.get("flow_control", False),
                beacon_interval=entry.get("beacon_interval_s"),
                beacon_data=entry.get("beacon_data"))
        elif t == "ax25_kiss":
            iface = AX25KISSInterface.AX25KISSInterface(
                owner, name, entry["callsign"], entry["ssid"],
                entry["axint_port"], entry["speed_baud"],
                entry.get("databits", 8), entry.get("parity", "none"),
                entry.get("stopbits", 1),
                entry.get("preamble_ms", 150),
                entry.get("txtail_ms", 10),
                entry.get("persistence", 200),
                entry.get("slottime_ms", 20),
                flow_control=entry.get("flow_control", False))
        elif t == "pipe":
            iface = PipeInterface.PipeInterface(
                owner, name, entry["command"],
                respawn_delay=entry.get("respawn_delay_s", 5))
        elif t == "i2p":
            iface = I2PInterface.I2PInterface(
                owner, name,
                entry.get("peers", []),
                connectable=entry.get("connectable", False),
                i2p_sam_address=entry.get("i2p_sam_address",
                                          "127.0.0.1:7656"))
        if iface is None:
            return False
        # Apply COMMON_FIELDS (spec section B) that map onto attributes
        # exposed by every RNS interface subclass: `mode` selects the
        # interface mode (full / gateway / access_point / roaming /
        # boundary), `outgoing` (a.k.a. `OUT`) gates outbound packets,
        # `bitrate_hint_bps` overrides the auto-estimated link bitrate
        # used by Reticulum's RTT/MDU heuristics, and
        # `announce_interval_s` schedules periodic Identity announces.
        # Each setattr is best-effort because some interface subclasses
        # don't expose every attribute (e.g. PipeInterface has no
        # bitrate hint); a missing attribute is not an error per spec.
        if "mode" in entry:
            mode_map = {
                "full": getattr(RNS.Interfaces.Interface.Interface, "MODE_FULL", 0x01)
                        if hasattr(RNS, "Interfaces") else 0x01,
                "gateway": getattr(RNS.Interfaces.Interface.Interface, "MODE_GATEWAY", 0x02)
                        if hasattr(RNS, "Interfaces") else 0x02,
                "access_point": getattr(RNS.Interfaces.Interface.Interface,
                                          "MODE_ACCESS_POINT", 0x04)
                        if hasattr(RNS, "Interfaces") else 0x04,
                "roaming": getattr(RNS.Interfaces.Interface.Interface, "MODE_ROAMING", 0x08)
                        if hasattr(RNS, "Interfaces") else 0x08,
                "boundary": getattr(RNS.Interfaces.Interface.Interface, "MODE_BOUNDARY", 0x10)
                        if hasattr(RNS, "Interfaces") else 0x10,
            }
            try:
                iface.mode = mode_map.get(entry["mode"], mode_map["full"])
            except Exception:
                pass
        if "outgoing" in entry:
            for attr in ("OUT", "outgoing"):
                try:
                    setattr(iface, attr, bool(entry["outgoing"]))
                except Exception:
                    pass
        if "bitrate_hint_bps" in entry:
            for attr in ("bitrate", "bitrate_hint", "bitrate_hint_bps"):
                try:
                    setattr(iface, attr, int(entry["bitrate_hint_bps"]))
                except Exception:
                    pass
        if "announce_interval_s" in entry:
            for attr in ("announce_interval", "announce_rate_target"):
                try:
                    setattr(iface, attr, int(entry["announce_interval_s"]))
                except Exception:
                    pass
        # IFAC (Interface Access Code) — Reticulum's per-interface
        # pre-shared-key gate. When `ifac_netname` AND `ifac_netkey`
        # are both present the iface uses keyed framing so non-keyed
        # peers can't even parse link-layer packets. `ifac_size` (in
        # bytes, 8..512) controls the truncation length of the keyed
        # hash; defaults to RNS's own internal default when absent.
        # All three writes are best-effort because some interface
        # subclasses don't expose every attribute.
        netname = entry.get("ifac_netname")
        netkey = entry.get("ifac_netkey")
        if netname and netkey:
            if "ifac_size" in entry:
                try:
                    setattr(iface, "ifac_size", int(entry["ifac_size"]))
                except Exception:
                    pass
            try:
                setattr(iface, "ifac_netname", str(netname))
            except Exception:
                pass
            try:
                setattr(iface, "ifac_netkey", str(netkey))
            except Exception:
                pass
            # Some RNS versions only honour IFAC when the iface flag
            # is set; setting it is a no-op on versions that don't.
            try:
                setattr(iface, "ifac_signature", True)
            except Exception:
                pass
        try:
            RNS.Transport.interfaces.append(iface)
        except Exception:
            pass
        # Stash the live iface object so _teardown can drain/close it.
        rt = self._iface_runtime.get(entry["id"])
        if rt is not None:
            rt["iface"] = iface
        return True

    def _teardown_interface(self, iid: str, drain_s: float) -> bool:
        """Graceful per-spec section G: stop accepting new outbound
        packets, allow in-flight Links to finish for up to `drain_s`,
        then force-close. RNS doesn't expose a uniform `stop()` across
        interface types, so we look for the common `OUT`/`detach`/
        `close`/`teardown` hooks and best-effort each one. Identity
        and config are NOT touched here."""
        runtime = self._iface_runtime.pop(iid, None)
        if runtime is None:
            return False
        iface = runtime.get("iface")
        # Phase 1: mark outbound off so no fresh packets queue.
        for attr in ("OUT", "outgoing"):
            if iface is not None and hasattr(iface, attr):
                try:
                    setattr(iface, attr, False)
                except Exception:
                    pass
        # Phase 2: drain. Poll any `pending_outgoing`/`tx_queue` size
        # if exposed; otherwise just sleep up to `drain_s`.
        deadline = time.time() + max(0.0, drain_s)
        while time.time() < deadline:
            pending = 0
            for attr in ("pending_outgoing", "tx_queue", "txbuf"):
                if iface is not None and hasattr(iface, attr):
                    try:
                        v = getattr(iface, attr)
                        pending = max(pending, len(v) if v is not None else 0)
                    except Exception:
                        pass
            if pending == 0:
                break
            time.sleep(0.05)
        # Phase 3: force close. Per spec, if teardown still hangs we
        # mark `last_error="forced"`.
        forced = False
        if iface is not None:
            for fn_name in ("detach", "close", "teardown", "stop"):
                fn = getattr(iface, fn_name, None)
                if callable(fn):
                    try:
                        fn()
                        break
                    except Exception:
                        forced = True
            try:
                if _HAVE_RNS and iface in RNS.Transport.interfaces:
                    RNS.Transport.interfaces.remove(iface)
            except Exception:
                pass
        if forced:
            self._log.write("WARN",
                            f"interface {runtime.get('name', iid)}: forced close")
        # Drop any peer entries that came in via this interface so the
        # publish fan-out doesn't keep targeting them across a restart.
        for h16 in [k for k, v in self._peers.items()
                    if v.get("iface_id") == iid]:
            self._peers.pop(h16, None)
        # Sentinel so a subsequent restart_interface() can surface
        # forced-close status on the freshly spawned runtime.
        self._last_teardown_forced = {iid: forced}
        return forced

    # ── control API ────────────────────────────────────────────────────

    def status(self) -> Dict[str, Any]:
        with self._lock:
            ifaces = []
            for entry in self.config.get("interfaces", []):
                rt = self._iface_runtime.get(entry["id"], {})
                self._poll_iface_runtime(rt)
                # Per-peer count for this iface from the announce table.
                peers = sum(1 for v in self._peers.values()
                            if v.get("iface_id") == entry["id"])
                if peers:
                    rt["peers"] = peers
                ifaces.append({
                    "id": entry["id"],
                    "name": entry["name"],
                    "type": entry["type"],
                    "enabled": entry.get("enabled", True),
                    "up": rt.get("up", False),
                    "peers": rt.get("peers", 0),
                    "bytes_in": rt.get("bytes_in", 0),
                    "bytes_out": rt.get("bytes_out", 0),
                    "bitrate_bps": rt.get("bitrate_bps", 0),
                    "last_error": rt.get("last_error", ""),
                })
            cot_stats = (self.cot_bridge.stats()
                         if self.cot_bridge is not None else
                         {"published": 0, "received": 0, "deduped": 0})
            return {
                "daemon": "running" if self._running else (
                    "stub" if not _HAVE_RNS else "stopped"),
                "rns_available": _HAVE_RNS,
                "identity_hash": self.identity_hash16(),
                "interfaces": ifaces,
                "cot_bridge": cot_stats,
                "config_path": self.config_path,
            }

    def list_interfaces(self) -> List[Dict[str, Any]]:
        with self._lock:
            return list(self.config.get("interfaces", []))

    def get_interface(self, iid: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            for e in self.config.get("interfaces", []):
                if e["id"] == iid:
                    return dict(e)
        return None

    def add_interface(self, cfg: Dict[str, Any]) -> Dict[str, Any]:
        with self._lock:
            entry = validate_interface(cfg)
            entry.setdefault("id", str(uuid.uuid4()))
            for existing in self.config["interfaces"]:
                if existing["id"] == entry["id"]:
                    raise SchemaError(f"duplicate id {entry['id']}")
                if existing["name"] == entry["name"]:
                    raise SchemaError(
                        f"duplicate interface name {entry['name']!r}")
            self.config["interfaces"].append(entry)
            self._save_config()
            if self._running and entry.get("enabled", True):
                self._spawn_interface(entry)
            return entry

    def update_interface(self, iid: str,
                          cfg: Dict[str, Any]) -> Dict[str, Any]:
        with self._lock:
            for i, existing in enumerate(self.config["interfaces"]):
                if existing["id"] == iid:
                    merged = dict(existing)
                    merged.update(cfg)
                    merged["id"] = iid
                    entry = validate_interface(merged)
                    self.config["interfaces"][i] = entry
                    self._save_config()
                    if self._running:
                        self._teardown_interface(iid, drain_s=2.0)
                        if entry.get("enabled", True):
                            self._spawn_interface(entry)
                    return entry
            raise SchemaError(f"unknown interface id {iid}")

    def remove_interface(self, iid: str) -> bool:
        with self._lock:
            for i, existing in enumerate(self.config["interfaces"]):
                if existing["id"] == iid:
                    self.config["interfaces"].pop(i)
                    self._save_config()
                    if self._running:
                        self._teardown_interface(iid, drain_s=2.0)
                    return True
        return False

    def set_enabled(self, iid: str, enabled: bool) -> Dict[str, Any]:
        with self._lock:
            for i, existing in enumerate(self.config["interfaces"]):
                if existing["id"] == iid:
                    existing["enabled"] = bool(enabled)
                    self._save_config()
                    if self._running:
                        self._teardown_interface(iid, drain_s=2.0)
                        if enabled:
                            self._spawn_interface(existing)
                    return existing
            raise SchemaError(f"unknown interface id {iid}")

    def restart_interface(self, iid: str,
                           drain_timeout_s: float = 5.0,
                           start_timeout_s: float = 10.0) -> Dict[str, Any]:
        # Spec section G: graceful drain → spawn → wait for `up` (or
        # `start_timeout_s`). If the prior teardown had to force-close,
        # the new runtime carries `last_error="forced"` so operators
        # can see it on the status panel.
        with self._lock:
            for existing in self.config["interfaces"]:
                if existing["id"] == iid:
                    if not self._running:
                        return {"id": iid, "restarted": False,
                                "reason": "daemon not running"}
                    forced = self._teardown_interface(
                        iid, drain_s=drain_timeout_s)
                    if existing.get("enabled", True):
                        self._spawn_interface(existing)
                    # Wait for the interface to come up or the start
                    # timeout to elapse (releasing the lock briefly so
                    # background RNS callbacks can flip the flag).
                    deadline = time.time() + max(0.0, start_timeout_s)
                    while time.time() < deadline:
                        rt = self._iface_runtime.get(iid, {})
                        if rt.get("up"):
                            break
                        self._lock.release()
                        try:
                            time.sleep(0.05)
                        finally:
                            self._lock.acquire()
                    rt = self._iface_runtime.get(iid, {})
                    if forced and not rt.get("last_error"):
                        rt["last_error"] = "forced"
                    timed_out = (existing.get("enabled", True)
                                 and not rt.get("up"))
                    return {"id": iid, "restarted": True,
                            "up": rt.get("up", False),
                            "timed_out": timed_out,
                            "forced_close": forced,
                            "last_error": rt.get("last_error", "")}
        raise SchemaError(f"unknown interface id {iid}")

    def restart_all(self,
                    drain_timeout_s: float = 5.0) -> List[Dict[str, Any]]:
        results = []
        with self._lock:
            for existing in list(self.config["interfaces"]):
                try:
                    results.append(self.restart_interface(
                        existing["id"], drain_timeout_s=drain_timeout_s))
                except Exception as exc:
                    results.append({"id": existing["id"],
                                     "restarted": False,
                                     "error": str(exc)})
        return results

    def validate_interface(self, cfg: Dict[str, Any]) -> Dict[str, Any]:
        # Pure validation — no side effects.
        return validate_interface(cfg)

    def export_config(self, passphrase: str,
                       include_identity: bool = True) -> Dict[str, str]:
        with self._lock:
            cfg = dict(self.config)
            id_pub = id_prv = None
            if include_identity and _HAVE_RNS and self._identity is not None:
                try:
                    id_pub = self._identity.get_public_key()
                    id_prv = self._identity.get_private_key()
                except Exception:
                    id_pub = id_prv = None
            token = export_token(cfg, passphrase,
                                  include_identity=include_identity,
                                  identity_pub=id_pub, identity_prv=id_prv,
                                  node_label=cfg.get("node_label", ""))
        return {"token": token}

    def import_config(self, token: str, passphrase: str,
                       placeholders: Optional[Dict[str, Any]] = None,
                       ) -> Dict[str, Any]:
        cfg, missing = import_token(token, passphrase,
                                     placeholders=placeholders)
        if missing:
            return {"applied": False, "missing_placeholders": missing}
        # Identity bytes ride alongside the schema in the token but are
        # NOT part of validate_config(); peel them off so the validator
        # doesn't reject them.
        id_prv_hex = cfg.pop("identity_prv", None)
        cfg.pop("identity_pub", None)
        cfg.pop("exported_at", None)
        identity_replaced = False
        with self._lock:
            self.config = validate_config(cfg)
            self._save_config()
            # Apply identity import end-to-end: write identity.prv
            # (0600) and reload the in-memory Identity so the node hash
            # persists across the import. RNS.Identity.to_file/from_file
            # uses the raw private-key bytes, which is exactly what the
            # token carries (token.py: identity_prv.hex()).
            if id_prv_hex:
                try:
                    raw = bytes.fromhex(id_prv_hex)
                    with open(self.identity_path, "wb") as f:
                        f.write(raw)
                    try:
                        os.chmod(self.identity_path, 0o600)
                    except OSError:
                        pass
                    if _HAVE_RNS:
                        self._identity = RNS.Identity.from_file(
                            self.identity_path)
                    identity_replaced = True
                except Exception as exc:
                    self._log.write(
                        "ERROR", f"identity import failed: {exc}")
            if self._running:
                # Tear everything down and re-spawn. If the identity was
                # replaced we also rebuild the local IN destination so
                # subsequent announces advertise the imported hash.
                for iid in list(self._iface_runtime):
                    self._teardown_interface(iid, drain_s=2.0)
                if identity_replaced and _HAVE_RNS \
                        and self._reticulum is not None:
                    try:
                        self._destination = RNS.Destination(
                            self._identity, RNS.Destination.IN,
                            RNS.Destination.SINGLE,
                            "predatorrf", "cot.v1")
                        self._destination.set_packet_callback(
                            self._on_packet)
                        self._destination.announce()
                    except Exception as exc:
                        self._log.write(
                            "ERROR",
                            f"destination rebuild after identity import "
                            f"failed: {exc}")
                    if self.cot_bridge is not None:
                        self.cot_bridge.own_hash16 = self.identity_hash16()
                for entry in self.config.get("interfaces", []):
                    if entry.get("enabled", True):
                        self._spawn_interface(entry)
        return {"applied": True, "missing_placeholders": [],
                "identity_replaced": identity_replaced,
                "identity_hash": self.identity_hash16()}

    def mint_replication_token(self, new_passphrase: str,
                                include_identity: bool = False
                                ) -> Dict[str, str]:
        with self._lock:
            cfg = dict(self.config)
            id_pub = id_prv = None
            if include_identity and _HAVE_RNS and self._identity is not None:
                try:
                    id_pub = self._identity.get_public_key()
                    id_prv = self._identity.get_private_key()
                except Exception:
                    id_pub = id_prv = None
            token = mint_replication_token(
                cfg, new_passphrase, include_identity=include_identity,
                identity_pub=id_pub, identity_prv=id_prv,
                node_label=cfg.get("node_label", ""))
        return {"token": token}

    def get_logs(self, level: str = "INFO",
                 since_ms: int = 0,
                 limit: int = 200) -> List[Dict[str, Any]]:
        return self._log.tail(level, since_ms, limit)


# ── line-delimited JSON control socket ──────────────────────────────────

class ControlServer:
    """Local-only control socket. Linux Unix socket with peer-uid check.

    Wire format: line-delimited JSON, one request per line.
    Request : {"id":<int>, "method":<str>, "params":{...}}
    Response: {"id":<int>, "ok":bool, "result":..., "error":<str>?}
    """

    def __init__(self, daemon: RNSDaemon,
                 sock_path: Optional[str] = None) -> None:
        self.daemon = daemon
        self.sock_path = sock_path or self._default_sock_path()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._listener: Optional[socket.socket] = None

    def _default_sock_path(self) -> str:
        if os.geteuid() == 0:
            return "/run/predator-rns.sock"
        state = os.path.expanduser("~/.local/state/predator-rns")
        os.makedirs(state, exist_ok=True)
        return os.path.join(state, "control.sock")

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass
        os.makedirs(os.path.dirname(self.sock_path) or ".", exist_ok=True)
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._listener.bind(self.sock_path)
        os.chmod(self.sock_path, 0o600)
        self._listener.listen(8)
        self._listener.settimeout(0.5)
        self._stop.clear()
        self._thread = threading.Thread(target=self._serve, daemon=True,
                                         name="rns-control")
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass
            self._listener = None
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass

    def _serve(self) -> None:
        own_uid = os.geteuid()
        while not self._stop.is_set():
            try:
                conn, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                # Linux SO_PEERCRED uid match
                try:
                    import struct
                    SO_PEERCRED = 17
                    creds = conn.getsockopt(socket.SOL_SOCKET, SO_PEERCRED,
                                             struct.calcsize("3i"))
                    _pid, peer_uid, _gid = struct.unpack("3i", creds)
                    if peer_uid != own_uid:
                        conn.close()
                        continue
                except OSError:
                    pass
                threading.Thread(target=self._handle, args=(conn,),
                                  daemon=True).start()
            except Exception:
                try:
                    conn.close()
                except OSError:
                    pass

    def _handle(self, conn: socket.socket) -> None:
        buf = b""
        try:
            conn.settimeout(60.0)
            while not self._stop.is_set():
                chunk = conn.recv(4096)
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    resp = self._dispatch_line(line)
                    conn.sendall(json.dumps(resp).encode("utf-8") + b"\n")
        except Exception:
            return
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _dispatch_line(self, line: bytes) -> Dict[str, Any]:
        try:
            req = json.loads(line.decode("utf-8"))
        except Exception as exc:
            return {"id": 0, "ok": False, "error": f"bad json: {exc}"}
        rid = req.get("id", 0)
        method = req.get("method") or ""
        params = req.get("params") or {}
        try:
            result = self._call(method, params)
            return {"id": rid, "ok": True, "result": result}
        except Exception as exc:
            return {"id": rid, "ok": False, "error": str(exc)}

    _METHODS: Dict[str, str] = {
        "status": "status",
        "list_interfaces": "list_interfaces",
        "get_interface": "get_interface",
        "add_interface": "add_interface",
        "update_interface": "update_interface",
        "remove_interface": "remove_interface",
        "set_enabled": "set_enabled",
        "restart_interface": "restart_interface",
        "restart_all": "restart_all",
        "validate_interface": "validate_interface",
        "export_config": "export_config",
        "import_config": "import_config",
        "mint_replication_token": "mint_replication_token",
        "get_logs": "get_logs",
    }

    def _call(self, method: str, params: Dict[str, Any]) -> Any:
        if method not in self._METHODS:
            raise SchemaError(f"unknown method {method!r}")
        attr = getattr(self.daemon, self._METHODS[method])
        return attr(**params)


def run_daemon(state_dir: Optional[str] = None,
               sock_path: Optional[str] = None) -> None:
    """Module entry point — start the daemon and the control socket
    and block forever (until SIGINT/SIGTERM)."""
    import signal
    bridge = RNSCotBridge(own_hash16=("0" * 16))
    daemon = RNSDaemon(state_dir=state_dir, cot_bridge=bridge)
    bridge.own_hash16 = daemon.identity_hash16()
    daemon.start()
    server = ControlServer(daemon, sock_path=sock_path)
    server.start()
    stop = threading.Event()

    def _sig(_signum, _frame):
        stop.set()

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)
    print(f"predator-rns daemon up; control socket: {server.sock_path}",
          flush=True)
    try:
        while not stop.is_set():
            time.sleep(0.5)
    finally:
        server.stop()
        daemon.stop()
