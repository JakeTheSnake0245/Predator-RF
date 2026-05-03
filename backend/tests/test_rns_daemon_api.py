"""Daemon control API tests — exercised in stub mode (no `rns` package
required at test time; the daemon reports daemon=stub when RNS isn't
installed and the API still works)."""
from __future__ import annotations

import os
import tempfile

import pytest

from backend.rns.daemon import RNSDaemon
from backend.rns.bridge import RNSCotBridge
from backend.rns.schema import SchemaError


@pytest.fixture()
def daemon(tmp_path):
    bridge = RNSCotBridge(own_hash16="0" * 16)
    d = RNSDaemon(state_dir=str(tmp_path), cot_bridge=bridge)
    bridge.own_hash16 = d.identity_hash16()
    yield d
    d.stop()


def test_status_before_start(daemon):
    s = daemon.status()
    assert "daemon" in s and "interfaces" in s
    assert s["interfaces"] == []


def test_add_update_remove_interface_persists(tmp_path, daemon):
    e = daemon.add_interface({"name": "lan", "type": "udp",
                              "listen_port": 4242})
    assert e["id"]
    assert os.path.exists(daemon.config_path)
    listed = daemon.list_interfaces()
    assert len(listed) == 1
    daemon.update_interface(e["id"], {"listen_port": 4343})
    again = daemon.get_interface(e["id"])
    assert again["listen_port"] == 4343
    daemon.set_enabled(e["id"], False)
    assert daemon.get_interface(e["id"])["enabled"] is False
    assert daemon.remove_interface(e["id"]) is True
    assert daemon.list_interfaces() == []


def test_validate_interface_pure(daemon):
    with pytest.raises(SchemaError):
        daemon.validate_interface({"name": "x", "type": "xyz"})
    ok = daemon.validate_interface({
        "name": "p", "type": "pipe", "command": "/bin/true"})
    assert ok["type"] == "pipe"


def test_export_then_import_round_trip(daemon):
    daemon.add_interface({"name": "lan", "type": "auto_interface",
                          "group_id": "g",
                          "allowed_interfaces": ["eth0"]})
    tok = daemon.export_config("p", include_identity=False)["token"]
    # Import on the same daemon — placeholders re-prompted, supply them.
    res = daemon.import_config(tok, "p")
    assert res["applied"] is False
    assert "interfaces.0.allowed_interfaces" in res["missing_placeholders"]
    res = daemon.import_config(tok, "p", placeholders={
        "interfaces.0.allowed_interfaces": ["wlan0"]})
    assert res["applied"] is True
    assert daemon.get_interface(daemon.list_interfaces()[0]["id"])[
        "allowed_interfaces"] == ["wlan0"]


def test_replication_token_round_trip(daemon):
    daemon.add_interface({"name": "lan", "type": "udp",
                          "listen_port": 4242})
    tok = daemon.mint_replication_token("rep", include_identity=False)["token"]
    res = daemon.import_config(tok, "rep")
    # UDP listen_address is device-local but we never set it, so import
    # applies cleanly with no placeholders prompted.
    assert res["applied"] is True


def test_corrupted_config_is_rejected_and_moved_aside(tmp_path):
    # Pre-seed a broken config file.
    cfg_path = tmp_path / "config.json"
    cfg_path.write_text("{not json")
    bridge = RNSCotBridge(own_hash16="0" * 16)
    d = RNSDaemon(state_dir=str(tmp_path), cot_bridge=bridge)
    assert d.config["interfaces"] == []
    assert os.path.exists(str(cfg_path) + ".broken")
    d.stop()


def test_restart_interface_is_safe_when_not_running(daemon):
    e = daemon.add_interface({"name": "u", "type": "udp",
                              "listen_port": 4242})
    res = daemon.restart_interface(e["id"])
    assert res["restarted"] is False
