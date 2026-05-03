"""Schema validation tests for backend/rns/schema.py.

Covers all 9 interface types — accept-path with required fields and a
representative reject case for each.
"""
from __future__ import annotations

import pytest

from backend.rns.schema import (
    INTERFACE_TYPES,
    DEVICE_LOCAL_FIELDS,
    SchemaError,
    placeholder_paths,
    validate_config,
    validate_interface,
)


def _base(name: str, type_: str, **extra) -> dict:
    return {"name": name, "type": type_, **extra}


def test_all_9_types_listed():
    assert set(INTERFACE_TYPES) == {
        "tcp_client", "tcp_server", "udp", "i2p", "auto_interface",
        "rnode", "kiss_tnc", "ax25_kiss", "pipe",
    }


def test_tcp_client_ok_and_rejects_bad_port():
    ok = validate_interface(_base("up", "tcp_client",
                                  target_host="1.2.3.4", target_port=4242))
    assert ok["id"] and ok["enabled"] is True and ok["mode"] == "full"
    with pytest.raises(SchemaError):
        validate_interface(_base("bad", "tcp_client",
                                  target_host="1.2.3.4", target_port=99999))


def test_tcp_server_requires_listen_port():
    validate_interface(_base("s", "tcp_server", listen_port=4242))
    with pytest.raises(SchemaError):
        validate_interface(_base("s", "tcp_server"))


def test_udp_listen_port_required():
    validate_interface(_base("u", "udp", listen_port=4242))
    with pytest.raises(SchemaError):
        validate_interface(_base("u", "udp"))


def test_i2p_minimal():
    v = validate_interface(_base("i", "i2p"))
    assert v["type"] == "i2p"


def test_auto_interface_requires_group_id():
    validate_interface(_base("a", "auto_interface", group_id="ops"))
    with pytest.raises(SchemaError):
        validate_interface(_base("a", "auto_interface"))


def test_auto_interface_scope_enum():
    with pytest.raises(SchemaError):
        validate_interface(_base("a", "auto_interface",
                                  group_id="x", discovery_scope="bogus"))


def test_rnode_full_field_set():
    validate_interface(_base("r", "rnode", port="/dev/ttyUSB0",
                              frequency_hz=915_000_000,
                              bandwidth_hz=125_000, txpower_dbm=17,
                              spreadingfactor=8, codingrate=5))
    with pytest.raises(SchemaError):
        validate_interface(_base("r", "rnode", port="/dev/ttyUSB0",
                                  frequency_hz=915_000_000,
                                  bandwidth_hz=125_000, txpower_dbm=17,
                                  spreadingfactor=99, codingrate=5))


def test_kiss_and_ax25():
    validate_interface(_base("k", "kiss_tnc",
                              port="/dev/ttyS0", speed_baud=9600))
    validate_interface(_base("ax", "ax25_kiss",
                              port="/dev/ttyS0", speed_baud=9600,
                              callsign="N0CALL", ssid=2,
                              axint_port="ax0"))


def test_pipe_requires_command():
    validate_interface(_base("p", "pipe", command="/usr/bin/cat"))
    with pytest.raises(SchemaError):
        validate_interface(_base("p", "pipe"))


def test_unknown_field_rejected():
    with pytest.raises(SchemaError):
        validate_interface(_base("u", "udp", listen_port=1, bogus=True))


def test_unknown_type_rejected():
    with pytest.raises(SchemaError):
        validate_interface({"name": "x", "type": "xyz"})


def test_validate_config_dedupes_names_and_ids():
    cfg = {"interfaces": [
        _base("a", "udp", listen_port=1),
        _base("a", "udp", listen_port=2),
    ]}
    with pytest.raises(SchemaError):
        validate_config(cfg)


def test_placeholder_paths_lists_device_local_fields():
    cfg = {"interfaces": [
        _base("r", "rnode", port="/dev/ttyUSB0",
              frequency_hz=915_000_000, bandwidth_hz=125_000,
              txpower_dbm=17, spreadingfactor=8, codingrate=5),
        _base("a", "auto_interface", group_id="g",
              allowed_interfaces=["eth0"]),
    ]}
    cfg = validate_config(cfg)
    paths = placeholder_paths(cfg)
    assert "interfaces.0.port" in paths
    assert "interfaces.1.allowed_interfaces" in paths


def test_device_local_map_covers_every_type():
    for t in INTERFACE_TYPES:
        assert t in DEVICE_LOCAL_FIELDS


def test_reliable_cot_default_lora_false_others_true():
    """Spec section C: reliable_cot defaults to False on rnode (LoRa)
    and True on every other interface type."""
    from backend.rns.schema import validate_interface
    rnode = validate_interface({
        "name": "lora0", "type": "rnode", "port": "/dev/ttyACM0",
        "frequency_hz": 868_000_000, "bandwidth_hz": 125_000,
        "txpower_dbm": 17, "spreadingfactor": 9, "codingrate": 5})
    assert rnode["reliable_cot"] is False
    for t, extra in (("tcp_client", {"target_host": "h", "target_port": 4242}),
                     ("tcp_server", {"listen_port": 4242}),
                     ("udp", {"listen_port": 4242}),
                     ("i2p", {}),
                     ("auto_interface", {"group_id": "g"}),
                     ("kiss_tnc", {"port": "/dev/x", "speed_baud": 9600}),
                     ("ax25_kiss", {"port": "/dev/x", "speed_baud": 9600,
                                    "callsign": "N0CALL", "ssid": 0,
                                    "axint_port": "ax0"}),
                     ("pipe", {"command": "/bin/cat"})):
        e = {"name": f"i_{t}", "type": t, **extra}
        assert validate_interface(e)["reliable_cot"] is True, t
    # Explicit override is preserved.
    out = validate_interface({"name": "lora1", "type": "rnode",
        "port": "/dev/ttyACM0", "frequency_hz": 868_000_000,
        "bandwidth_hz": 125_000, "txpower_dbm": 17,
        "spreadingfactor": 9, "codingrate": 5, "reliable_cot": True})
    assert out["reliable_cot"] is True
