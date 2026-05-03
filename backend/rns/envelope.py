"""CBOR envelope used to carry CoT XML over RNS.

See section C of the task spec. Envelope shape:
  {v, ts, src, uid, ct, z, p}
zlib compression applied when payload > 256 bytes.
"""
from __future__ import annotations

import time
import zlib
from typing import Any, Dict

import cbor2

ENVELOPE_VERSION = 1
COMPRESS_THRESHOLD = 256


class EnvelopeError(ValueError):
    pass


def wrap_cot(xml: bytes, *, src_hash16: str, uid: str,
             ts_ms: int | None = None,
             ct: str = "cot/xml") -> bytes:
    """Wrap CoT XML into the CBOR envelope. Returns CBOR bytes."""
    if not isinstance(xml, (bytes, bytearray)):
        raise EnvelopeError("xml must be bytes")
    if len(src_hash16) != 16:
        raise EnvelopeError("src_hash16 must be 16 hex chars")
    payload = bytes(xml)
    z = 0
    if len(payload) > COMPRESS_THRESHOLD:
        compressed = zlib.compress(payload, 6)
        if len(compressed) < len(payload):
            payload = compressed
            z = 1
    env: Dict[str, Any] = {
        "v": ENVELOPE_VERSION,
        "ts": int(ts_ms) if ts_ms is not None else int(time.time() * 1000),
        "src": src_hash16,
        "uid": uid,
        "ct": ct,
        "z": z,
        "p": payload,
    }
    return cbor2.dumps(env)


def unwrap_cot(buf: bytes) -> Dict[str, Any]:
    """Decode and validate a CBOR envelope. Returns a dict with at least
    {v, ts, src, uid, ct, xml}. The original `p`/`z` fields are dropped
    in favor of a decoded `xml` bytes field."""
    try:
        env = cbor2.loads(buf)
    except Exception as exc:  # pragma: no cover - cbor2 raises various
        raise EnvelopeError(f"cbor decode failed: {exc}") from exc
    if not isinstance(env, dict):
        raise EnvelopeError("envelope must be a map")
    if env.get("v") != ENVELOPE_VERSION:
        raise EnvelopeError(f"unsupported envelope version {env.get('v')!r}")
    for k in ("ts", "src", "uid", "ct", "z", "p"):
        if k not in env:
            raise EnvelopeError(f"missing field {k!r}")
    payload = env["p"]
    if not isinstance(payload, (bytes, bytearray)):
        raise EnvelopeError("payload must be bytes")
    if env["z"]:
        try:
            payload = zlib.decompress(bytes(payload))
        except zlib.error as exc:
            raise EnvelopeError(f"zlib decompress failed: {exc}") from exc
    return {
        "v": env["v"],
        "ts": int(env["ts"]),
        "src": env["src"],
        "uid": env["uid"],
        "ct": env["ct"],
        "xml": bytes(payload),
    }
