"""CBOR envelope round-trip tests."""
from __future__ import annotations

import pytest

from backend.rns.envelope import (
    COMPRESS_THRESHOLD,
    EnvelopeError,
    unwrap_cot,
    wrap_cot,
)


def test_round_trip_short_payload_no_compression():
    xml = b"<event/>"
    env = wrap_cot(xml, src_hash16="0" * 16, uid="abc")
    out = unwrap_cot(env)
    assert out["xml"] == xml
    assert out["uid"] == "abc"
    assert out["ct"] == "cot/xml"
    assert out["src"] == "0" * 16


def test_round_trip_large_payload_compresses():
    xml = (b"<event x='1'>" + b"a" * (COMPRESS_THRESHOLD + 50)
           + b"</event>")
    env = wrap_cot(xml, src_hash16="1234567890abcdef", uid="big")
    # Should be smaller than the raw XML due to zlib.
    assert len(env) < len(xml) + 64
    out = unwrap_cot(env)
    assert out["xml"] == xml


def test_unwrap_rejects_garbage():
    with pytest.raises(EnvelopeError):
        unwrap_cot(b"not-cbor")


def test_src_hash_must_be_16_chars():
    with pytest.raises(EnvelopeError):
        wrap_cot(b"x", src_hash16="short", uid="u")
