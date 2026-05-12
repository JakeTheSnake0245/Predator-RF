"""CBOR envelope for Kujhad-style tasking commands carried over RNS.

Roadmap #6 — RNS commanding wrapper. Mirrors the cot.v1 envelope shape
(`backend/rns/envelope.py`) so the daemon's per-aspect plumbing stays
uniform, but the payload carries a JSON-encoded
`{class, action, args}` body — the EXACT same shape that the Kujhad
HTTP `/v1/command` endpoint accepts. Identical-on-the-wire bodies mean
the Device-side dispatcher in `cmd_handler.py` can route to the same
internal command bus that Kujhad HTTP already uses, so a `tune`
command produces byte-identical effects whether it arrived over IP or
RNS — single execution path = single audit trail.

Envelope shape (CBOR map):
  {v, ts, src, uid, ct, z, p}     — same six keys as cot.v1 envelope
where:
  v   = CMD_ENVELOPE_VERSION (1)
  ts  = wall-clock ms (int)
  src = 16-hex sender identity prefix (matches RNSCotBridge.own_hash16)
  uid = caller-supplied unique-id (used by dedupe)
  ct  = "cmd/json"
  z   = 0|1 (zlib compression flag — only set when payload > 256 B)
  p   = JSON bytes of {class, action, args}

`tx.*` enforcement: classes whose lowercase name starts with `tx` are
hard-rejected at BOTH wrap-time and unwrap-time. The C++ build already
hard-rejects `tx.*` at the Kujhad HTTP wire layer
(`core/src/predator/kujhad_fleet.h` line 23 banner) for the RX-only
posture; adding the same gate here means a malicious sender that
bypassed the wrap-side guard still cannot get a `tx.*` past the
recipient.
"""
from __future__ import annotations

import json
import time
import zlib
from typing import Any, Dict, Tuple

import cbor2

CMD_ENVELOPE_VERSION = 1
CMD_ASPECT_PRIMARY = "predatorrf"
CMD_ASPECT_SECONDARY = "cmd.v1"
CMD_CONTENT_TYPE = "cmd/json"
CMD_COMPRESS_THRESHOLD = 256

# Conservative MDU shared with the cot.v1 publish path so the daemon's
# Packet-vs-Link decision logic can stay identical for both aspects.
PACKET_MDU = 460

# Class prefixes (lowercased) that are hard-banned. RX-only fleet
# posture means TX is never permitted regardless of source.
FORBIDDEN_CLASS_PREFIXES: Tuple[str, ...] = ("tx",)

# Whitelisted command classes mirror the Kujhad HTTP dispatcher in
# `core/src/predator/kujhad_fleet.h` (L1062-1194). Anything outside
# this set is rejected at unwrap-time as well — defense in depth: a
# new class added in C++ without a corresponding RNS allowlist update
# fails closed rather than silently routing.
ALLOWED_COMMAND_CLASSES: frozenset = frozenset({
    "tune", "scan", "mission", "decoder", "marker", "hold",
    "vfo", "source", "audio", "ping",
})


class CmdEnvelopeError(ValueError):
    """Raised on any wrap/unwrap validation failure."""


def _validate_cmd_dict(cmd: Dict[str, Any]) -> Tuple[str, str, Dict[str, Any]]:
    """Pull (class, action, args) out of a command dict and enforce
    the RX-only / allowlist gates. Used by both wrap and unwrap so the
    rules cannot drift between sender and receiver."""
    if not isinstance(cmd, dict):
        raise CmdEnvelopeError("command must be a dict")
    klass = cmd.get("class")
    action = cmd.get("action")
    args = cmd.get("args", {})
    if not isinstance(klass, str) or not klass:
        raise CmdEnvelopeError("command.class must be a non-empty string")
    if not isinstance(action, str) or not action:
        raise CmdEnvelopeError("command.action must be a non-empty string")
    if not isinstance(args, dict):
        raise CmdEnvelopeError("command.args must be a dict")
    klass_lc = klass.lower()
    for forbidden in FORBIDDEN_CLASS_PREFIXES:
        if klass_lc.startswith(forbidden):
            raise CmdEnvelopeError(
                f"command class {klass!r} is forbidden (RX-only fleet)")
    if klass_lc not in ALLOWED_COMMAND_CLASSES:
        raise CmdEnvelopeError(
            f"command class {klass!r} not in ALLOWED_COMMAND_CLASSES")
    return klass, action, args


def wrap_cmd(cmd: Dict[str, Any], *, src_hash16: str, uid: str,
             ts_ms: int | None = None) -> bytes:
    """Wrap a `{class, action, args}` dict into the CBOR cmd envelope.

    Raises:
        CmdEnvelopeError: if the command violates RX-only or allowlist
            rules, or if `src_hash16` is malformed.
    """
    if len(src_hash16) != 16:
        raise CmdEnvelopeError("src_hash16 must be 16 hex chars")
    if not isinstance(uid, str) or not uid:
        raise CmdEnvelopeError("uid must be a non-empty string")
    klass, action, args = _validate_cmd_dict(cmd)
    payload = json.dumps(
        {"class": klass, "action": action, "args": args},
        separators=(",", ":"), sort_keys=True,
    ).encode("utf-8")
    z = 0
    if len(payload) > CMD_COMPRESS_THRESHOLD:
        compressed = zlib.compress(payload, 6)
        if len(compressed) < len(payload):
            payload = compressed
            z = 1
    env: Dict[str, Any] = {
        "v": CMD_ENVELOPE_VERSION,
        "ts": int(ts_ms) if ts_ms is not None else int(time.time() * 1000),
        "src": src_hash16,
        "uid": uid,
        "ct": CMD_CONTENT_TYPE,
        "z": z,
        "p": payload,
    }
    return cbor2.dumps(env)


def unwrap_cmd(buf: bytes) -> Dict[str, Any]:
    """Decode and validate a cmd envelope. Returns
    `{v, ts, src, uid, ct, cmd:{class,action,args}}`.

    Raises:
        CmdEnvelopeError: on any decode failure, missing field, version
            mismatch, content-type mismatch, RX-only violation, or
            class outside the allowlist.
    """
    try:
        env = cbor2.loads(buf)
    except Exception as exc:
        raise CmdEnvelopeError(f"cbor decode failed: {exc}") from exc
    if not isinstance(env, dict):
        raise CmdEnvelopeError("envelope must be a map")
    if env.get("v") != CMD_ENVELOPE_VERSION:
        raise CmdEnvelopeError(
            f"unsupported envelope version {env.get('v')!r}")
    for k in ("ts", "src", "uid", "ct", "z", "p"):
        if k not in env:
            raise CmdEnvelopeError(f"missing field {k!r}")
    if env["ct"] != CMD_CONTENT_TYPE:
        raise CmdEnvelopeError(
            f"unexpected content-type {env['ct']!r}; "
            f"expected {CMD_CONTENT_TYPE!r}")
    payload = env["p"]
    if not isinstance(payload, (bytes, bytearray)):
        raise CmdEnvelopeError("payload must be bytes")
    if env["z"]:
        try:
            payload = zlib.decompress(bytes(payload))
        except zlib.error as exc:
            raise CmdEnvelopeError(
                f"zlib decompress failed: {exc}") from exc
    try:
        cmd_obj = json.loads(bytes(payload).decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise CmdEnvelopeError(f"command json decode failed: {exc}") from exc
    klass, action, args = _validate_cmd_dict(cmd_obj)
    return {
        "v": env["v"],
        "ts": int(env["ts"]),
        "src": env["src"],
        "uid": env["uid"],
        "ct": env["ct"],
        "cmd": {"class": klass, "action": action, "args": args},
    }


def will_use_link(env_bytes: bytes, *, reliable: bool = False) -> bool:
    """Return True iff the daemon will promote this envelope to a Link
    (Resource) instead of an opportunistic Packet. Mirrors the rule in
    `daemon._send_one` so callers and tests can predict transport
    selection without instantiating the daemon."""
    return bool(reliable) or len(env_bytes) > PACKET_MDU
