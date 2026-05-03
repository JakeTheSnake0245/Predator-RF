"""Predator RF RNS config token (`prf-rns-v1.<base32>`).

Token layout (post-base32-decode):
    [ver:1][salt:16][nonce:24][ciphertext+tag]

Cipher  : XChaCha20-Poly1305 IETF (PyNaCl `crypto_aead_xchacha20poly1305_ietf_*`)
KDF     : Argon2id(t=3, m=64MiB, p=1, len=32)
Encoding: Crockford base32, no padding, lowercase

Plaintext is zlib-compressed canonical JSON of:
    {schema_version, exported_at, node_label,
     identity_pub | None, identity_prv | None,
     interfaces[], cot_bridge{}, peer_allowlist[]}

Device-local fields are replaced by `{"$placeholder": "<field_path>"}`
markers that the importer must fill in.
"""
from __future__ import annotations

import json
import secrets
import time
import zlib
from typing import Any, Dict, List, Tuple

import argon2.low_level as _argon2
import nacl.bindings as _nacl
import nacl.exceptions

from .schema import (
    DEVICE_LOCAL_FIELDS,
    SchemaError,
    placeholder_paths,
    validate_config,
)

TOKEN_PREFIX = "prf-rns-v1."
TOKEN_VERSION = 1

_ARGON_T = 3            # iterations
_ARGON_M = 64 * 1024    # KiB → 64 MiB
_ARGON_P = 1            # parallelism
_ARGON_KEY_LEN = 32

_CROCKFORD = "0123456789abcdefghjkmnpqrstvwxyz"
_CROCKFORD_INV: Dict[str, int] = {c: i for i, c in enumerate(_CROCKFORD)}
# Map ambiguous characters operators tend to type back in.
for a, b in (("o", "0"), ("i", "1"), ("l", "1"), ("u", "v")):
    _CROCKFORD_INV[a] = _CROCKFORD_INV[b]


class TokenError(ValueError):
    """Raised when a token cannot be decoded / decrypted / parsed."""


def _b32encode(data: bytes) -> str:
    n = int.from_bytes(data, "big")
    nbits = len(data) * 8
    # number of base32 chars needed
    nchars = (nbits + 4) // 5
    out = []
    for i in range(nchars - 1, -1, -1):
        out.append(_CROCKFORD[(n >> (5 * i)) & 0x1F])
    return "".join(out)


def _b32decode(s: str, expected_len: int) -> bytes:
    s = s.strip().lower()
    n = 0
    for c in s:
        if c in ("-", " ", "_"):
            continue
        if c not in _CROCKFORD_INV:
            raise TokenError(f"invalid base32 char {c!r}")
        n = (n << 5) | _CROCKFORD_INV[c]
    return n.to_bytes(expected_len, "big")


def _derive_key(passphrase: str, salt: bytes) -> bytes:
    if not isinstance(passphrase, str) or not passphrase:
        raise TokenError("passphrase must be a non-empty string")
    return _argon2.hash_secret_raw(
        passphrase.encode("utf-8"), salt,
        time_cost=_ARGON_T, memory_cost=_ARGON_M, parallelism=_ARGON_P,
        hash_len=_ARGON_KEY_LEN, type=_argon2.Type.ID)


def _replace_placeholders(cfg: Dict[str, Any]) -> Dict[str, Any]:
    out = json.loads(json.dumps(cfg))
    for i, entry in enumerate(out.get("interfaces", [])):
        iftype = entry.get("type")
        for fname in DEVICE_LOCAL_FIELDS.get(iftype, ()):
            if fname in entry:
                entry[fname] = {
                    "$placeholder": f"interfaces.{i}.{fname}",
                }
    return out


def _apply_placeholder_values(cfg: Dict[str, Any],
                              values: Dict[str, Any]) -> Tuple[
                                  Dict[str, Any], List[str]]:
    """Walk the cfg and substitute every `{"$placeholder": path}` marker
    with the supplied value. Returns (new_cfg, missing_paths)."""
    missing: List[str] = []
    out = json.loads(json.dumps(cfg))
    for i, entry in enumerate(out.get("interfaces", [])):
        for fname, val in list(entry.items()):
            if isinstance(val, dict) and "$placeholder" in val:
                path = val["$placeholder"]
                if path in values:
                    entry[fname] = values[path]
                else:
                    missing.append(path)
    return out, missing


def export_token(cfg: Dict[str, Any], passphrase: str, *,
                 include_identity: bool = True,
                 identity_pub: bytes | None = None,
                 identity_prv: bytes | None = None,
                 node_label: str = "") -> str:
    """Serialize and encrypt the config; return a `prf-rns-v1.*` token."""
    cfg = validate_config(cfg)
    payload: Dict[str, Any] = {
        "schema_version": cfg.get("schema_version", 1),
        "exported_at": int(time.time()),
        "node_label": node_label or cfg.get("node_label", ""),
        "interfaces": _replace_placeholders(cfg)["interfaces"],
        "cot_bridge": cfg.get("cot_bridge", {}),
        "peer_allowlist": cfg.get("peer_allowlist", []),
    }
    if include_identity and identity_pub is not None and identity_prv is not None:
        payload["identity_pub"] = identity_pub.hex()
        payload["identity_prv"] = identity_prv.hex()
    else:
        payload["identity_pub"] = None
        payload["identity_prv"] = None

    canonical = json.dumps(payload, sort_keys=True,
                           separators=(",", ":")).encode("utf-8")
    compressed = zlib.compress(canonical, 6)
    salt = secrets.token_bytes(16)
    nonce = secrets.token_bytes(24)
    key = _derive_key(passphrase, salt)
    # XChaCha20-Poly1305 IETF — 24-byte nonce, 16-byte tag, optional AAD.
    # AAD binds the version byte so a downgrade attacker can't strip it.
    aad = bytes([TOKEN_VERSION])
    ct = _nacl.crypto_aead_xchacha20poly1305_ietf_encrypt(
        compressed, aad, nonce, key)
    blob = bytes([TOKEN_VERSION]) + salt + nonce + ct
    return TOKEN_PREFIX + _b32encode(blob)


def import_token(token: str, passphrase: str, *,
                 placeholders: Dict[str, Any] | None = None,
                 ) -> Tuple[Dict[str, Any], List[str]]:
    """Decrypt a token. Returns (config, missing_placeholders).

    If `placeholders` supplies every device-local value, `missing` is []
    and the returned config is fully validated and ready to apply. If
    any placeholder is unfilled, `missing` lists the paths and the
    returned config still contains `$placeholder` markers.
    """
    if not isinstance(token, str) or not token.startswith(TOKEN_PREFIX):
        raise TokenError(f"token must start with {TOKEN_PREFIX!r}")
    body = token[len(TOKEN_PREFIX):]
    if not body:
        raise TokenError("token body is empty")
    # We don't know the exact length up front because base32 chars carry 5
    # bits each. Try a window of plausible byte lengths around the math.
    nbits = len(body) * 5
    expected = nbits // 8
    blob: bytes | None = None
    last_err: str = ""
    for trial in (expected, expected - 1, expected + 1):
        if trial < 1 + 16 + 24 + 16:
            continue
        try:
            blob = _b32decode(body, trial)
            break
        except TokenError as exc:
            last_err = str(exc)
            continue
    if blob is None:
        raise TokenError(f"base32 decode failed: {last_err}")
    if blob[0] != TOKEN_VERSION:
        raise TokenError(
            f"unsupported token version {blob[0]} (this build supports "
            f"v{TOKEN_VERSION})")
    salt = blob[1:17]
    nonce = blob[17:41]
    ct = blob[41:]
    key = _derive_key(passphrase, salt)
    aad = bytes([blob[0]])
    try:
        compressed = _nacl.crypto_aead_xchacha20poly1305_ietf_decrypt(
            ct, aad, nonce, key)
    except nacl.exceptions.CryptoError as exc:
        raise TokenError("decryption failed (wrong passphrase or "
                         "corrupted token)") from exc
    try:
        canonical = zlib.decompress(compressed)
        payload = json.loads(canonical.decode("utf-8"))
    except (zlib.error, ValueError) as exc:
        raise TokenError(f"payload decode failed: {exc}") from exc
    if not isinstance(payload, dict) or "interfaces" not in payload:
        raise TokenError("token payload missing 'interfaces'")

    cfg: Dict[str, Any] = {
        "schema_version": payload.get("schema_version", 1),
        "node_label": payload.get("node_label", ""),
        "interfaces": payload.get("interfaces", []),
        "cot_bridge": payload.get("cot_bridge", {}),
        "peer_allowlist": payload.get("peer_allowlist", []),
        "identity_pub": payload.get("identity_pub"),
        "identity_prv": payload.get("identity_prv"),
        "exported_at": payload.get("exported_at"),
    }
    cfg, missing = _apply_placeholder_values(cfg, placeholders or {})
    if not missing:
        # Validate only when fully resolved; otherwise the caller will
        # validate after gathering the remaining placeholder values.
        try:
            cfg = validate_config(cfg)
        except SchemaError as exc:
            raise TokenError(f"imported config is invalid: {exc}") from exc
    return cfg, missing


def mint_replication_token(cfg: Dict[str, Any], new_passphrase: str, *,
                           include_identity: bool = False,
                           identity_pub: bytes | None = None,
                           identity_prv: bytes | None = None,
                           node_label: str = "") -> str:
    """Convenience for `--replicate`: defaults to NOT shipping the
    identity (the next node mints its own), so accidental sharing of
    the same RNS hash is impossible unless explicitly opted in."""
    return export_token(
        cfg, new_passphrase,
        include_identity=include_identity,
        identity_pub=identity_pub, identity_prv=identity_prv,
        node_label=node_label,
    )


def list_placeholders(cfg: Dict[str, Any]) -> List[str]:
    """Return device-local field paths the importer must supply for
    `cfg` to be apply-able. Convenience wrapper over schema."""
    return placeholder_paths(cfg)
