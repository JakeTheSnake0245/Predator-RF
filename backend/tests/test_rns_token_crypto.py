"""Token AEAD crypto tests — verifies the spec-required XChaCha20-Poly1305
IETF construction (NOT XSalsa20/SecretBox) and AAD-binding of the version
byte. Per `.local/tasks/task-27.md` section E.
"""
from __future__ import annotations

import nacl.bindings as _nacl
import pytest

from backend.rns.token import (
    TOKEN_PREFIX,
    TOKEN_VERSION,
    TokenError,
    _b32decode,
    _derive_key,
    export_token,
    import_token,
)


def _cfg() -> dict:
    return {
        "interfaces": [
            {"name": "udp", "type": "udp", "listen_port": 4242},
        ],
        "peer_allowlist": [],
    }


def test_xchacha20_poly1305_ietf_decrypts_token_directly():
    """The implementation MUST be XChaCha20-Poly1305 IETF, not SecretBox.
    We decode a known token, derive the key the same way, and decrypt
    directly with the IETF AEAD — if the token used SecretBox, this
    would fail."""
    tok = export_token(_cfg(), "p")
    body = tok[len(TOKEN_PREFIX):]
    # Recover the raw blob — token.py's decoder already handles this.
    nbits = len(body) * 5
    expected = nbits // 8
    blob = None
    for trial in (expected, expected - 1, expected + 1):
        try:
            blob = _b32decode(body, trial)
            break
        except TokenError:
            continue
    assert blob is not None
    assert blob[0] == TOKEN_VERSION
    salt, nonce, ct = blob[1:17], blob[17:41], blob[41:]
    key = _derive_key("p", salt)
    aad = bytes([TOKEN_VERSION])
    pt = _nacl.crypto_aead_xchacha20poly1305_ietf_decrypt(ct, aad, nonce, key)
    assert len(pt) > 0


def test_aad_binding_detects_version_tamper():
    """Flipping the version byte MUST fail decryption because the AEAD
    AAD binds it. This blocks downgrade attacks."""
    tok = export_token(_cfg(), "p")
    body = tok[len(TOKEN_PREFIX):]
    nbits = len(body) * 5
    expected = nbits // 8
    blob = None
    for trial in (expected, expected - 1, expected + 1):
        try:
            blob = _b32decode(body, trial)
            break
        except TokenError:
            continue
    assert blob is not None
    # Tamper the version byte; importer should now refuse.
    tampered_blob = bytes([TOKEN_VERSION + 1]) + blob[1:]
    # Re-encode using token.py's base32 helper.
    from backend.rns.token import _b32encode
    bad = TOKEN_PREFIX + _b32encode(tampered_blob)
    with pytest.raises(TokenError):
        import_token(bad, "p")
