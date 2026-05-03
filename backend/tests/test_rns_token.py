"""Config token export/import tests."""
from __future__ import annotations

import pytest

from backend.rns.token import (
    TOKEN_PREFIX,
    TokenError,
    export_token,
    import_token,
    mint_replication_token,
)


def _cfg() -> dict:
    return {
        "interfaces": [
            {"name": "lan", "type": "auto_interface", "group_id": "ops",
             "allowed_interfaces": ["eth0"]},
            {"name": "lora", "type": "rnode", "port": "/dev/ttyUSB0",
             "frequency_hz": 915_000_000, "bandwidth_hz": 125_000,
             "txpower_dbm": 17, "spreadingfactor": 8, "codingrate": 5},
            {"name": "udp", "type": "udp", "listen_port": 4242},
        ],
        "cot_bridge": {"reliable_default": True},
        "peer_allowlist": ["abcdef0123456789"],
    }


def test_round_trip_with_placeholders():
    tok = export_token(_cfg(), "correct horse")
    assert tok.startswith(TOKEN_PREFIX)
    cfg, missing = import_token(tok, "correct horse")
    assert set(missing) == {
        "interfaces.0.allowed_interfaces",
        "interfaces.1.port",
    }
    # Now supply the placeholders and re-import → fully validated.
    cfg, missing = import_token(tok, "correct horse", placeholders={
        "interfaces.0.allowed_interfaces": ["wlan0"],
        "interfaces.1.port": "/dev/ttyACM0",
    })
    assert missing == []
    by_name = {i["name"]: i for i in cfg["interfaces"]}
    assert by_name["lan"]["allowed_interfaces"] == ["wlan0"]
    assert by_name["lora"]["port"] == "/dev/ttyACM0"


def test_wrong_passphrase_rejected():
    tok = export_token(_cfg(), "right")
    with pytest.raises(TokenError):
        import_token(tok, "wrong")


def test_truncation_rejected():
    tok = export_token(_cfg(), "p")
    with pytest.raises(TokenError):
        import_token(tok[:-8], "p")


def test_bad_prefix_rejected():
    with pytest.raises(TokenError):
        import_token("nope.foo", "p")


def test_replication_token_excludes_identity_by_default():
    tok = mint_replication_token(_cfg(), "fresh")
    cfg, _ = import_token(tok, "fresh", placeholders={
        "interfaces.0.allowed_interfaces": ["x"],
        "interfaces.1.port": "/dev/ttyACM0",
    })
    assert cfg.get("identity_pub") is None
    assert cfg.get("identity_prv") is None


def test_identity_round_trip():
    tok = export_token(_cfg(), "p", include_identity=True,
                       identity_pub=b"\x01" * 32,
                       identity_prv=b"\x02" * 32)
    cfg, _ = import_token(tok, "p", placeholders={
        "interfaces.0.allowed_interfaces": ["x"],
        "interfaces.1.port": "/dev/ttyACM0",
    })
    assert cfg["identity_pub"] == ("01" * 32)
    assert cfg["identity_prv"] == ("02" * 32)
