"""Daemon control socket: line-delimited JSON request/response over a
Unix socket with peer-uid auth. Section F of the spec."""
from __future__ import annotations

import json
import os
import socket
import time

import pytest

from backend.rns.daemon import ControlServer, RNSDaemon


def _request(sock_path: str, body: dict) -> dict:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sock_path)
        s.sendall((json.dumps(body) + "\n").encode("utf-8"))
        buf = b""
        deadline = time.time() + 2.0
        s.settimeout(2.0)
        while b"\n" not in buf and time.time() < deadline:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        line = buf.split(b"\n", 1)[0]
        return json.loads(line.decode("utf-8"))
    finally:
        s.close()


@pytest.fixture
def daemon_with_socket(tmp_path):
    d = RNSDaemon(state_dir=str(tmp_path))
    sock = str(tmp_path / "ctrl.sock")
    server = ControlServer(d, sock_path=sock)
    server.start()
    # Allow the listener thread to bind.
    for _ in range(20):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    yield d, server, sock
    server.stop()


def test_status_and_validate_round_trip(daemon_with_socket):
    _, _, sock = daemon_with_socket
    resp = _request(sock, {"id": 1, "method": "status"})
    assert resp["ok"] is True
    assert "daemon" in resp["result"]
    assert "identity_hash" in resp["result"]

    resp = _request(sock, {"id": 2, "method": "validate_interface",
                            "params": {"cfg": {
                                "name": "udp1", "type": "udp",
                                "listen_port": 4242}}})
    assert resp["ok"] is True
    assert resp["result"]["type"] == "udp"


def test_add_list_remove_via_socket(daemon_with_socket):
    _, _, sock = daemon_with_socket
    add = _request(sock, {"id": 3, "method": "add_interface",
                           "params": {"cfg": {
                               "name": "udp_a", "type": "udp",
                               "listen_port": 4243}}})
    assert add["ok"] is True
    iid = add["result"]["id"]
    lst = _request(sock, {"id": 4, "method": "list_interfaces"})
    assert lst["ok"] is True
    assert any(i["id"] == iid for i in lst["result"])
    rm = _request(sock, {"id": 5, "method": "remove_interface",
                          "params": {"iid": iid}})
    assert rm["ok"] is True
    assert rm["result"] is True


def test_unknown_method_returns_error(daemon_with_socket):
    _, _, sock = daemon_with_socket
    resp = _request(sock, {"id": 6, "method": "no_such_method"})
    assert resp["ok"] is False
    assert "error" in resp


def test_export_import_round_trip_via_socket(daemon_with_socket):
    _, _, sock = daemon_with_socket
    _request(sock, {"id": 7, "method": "add_interface", "params": {
        "cfg": {"name": "u_exp", "type": "udp", "listen_port": 4244}}})
    exp = _request(sock, {"id": 8, "method": "export_config",
                           "params": {"passphrase": "pw",
                                      "include_identity": False}})
    assert exp["ok"] is True
    token = exp["result"]["token"]
    assert token.startswith("prf-rns-v1.")
    imp = _request(sock, {"id": 9, "method": "import_config",
                           "params": {"token": token,
                                      "passphrase": "pw"}})
    assert imp["ok"] is True
    assert imp["result"]["applied"] is True
