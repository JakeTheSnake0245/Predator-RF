"""Daemon publish path: Packet vs Link/Resource selection, drain
semantics in restart_interface. Runs in stub mode (no `rns` import
required) by injecting a minimal fake RNS module if the real one is
absent — what we want to verify is _our_ branching, not upstream RNS."""
from __future__ import annotations

import sys
import types

import pytest

from backend.rns import daemon as daemon_mod


class _FakeIface:
    def __init__(self, name: str) -> None:
        self.name = name
        self.OUT = True
        self.detached = False
        self.pending_outgoing: list = []

    def detach(self) -> None:
        self.detached = True


class _FakeDestination:
    def __init__(self) -> None:
        self.packets: list[bytes] = []
        self.links: list = []

    def announce(self) -> None:
        pass

    def set_packet_callback(self, _fn) -> None:
        pass


class _FakePacket:
    def __init__(self, dest, data):
        self.dest = dest
        self.data = data

    def send(self):
        self.dest.packets.append(self.data)


class _FakeResource:
    def __init__(self, data, link):
        link.resources.append(data)


class _FakeLink:
    def __init__(self, dest):
        dest.links.append(self)
        self.resources: list = []
        self.torn = False

    def teardown(self):
        self.torn = True


def _install_fake_rns(monkeypatch) -> _FakeDestination:
    fake = types.SimpleNamespace()
    fake.Packet = _FakePacket
    fake.Link = _FakeLink
    fake.Resource = _FakeResource
    fake.Transport = types.SimpleNamespace(interfaces=[])
    monkeypatch.setattr(daemon_mod, "_HAVE_RNS", True)
    monkeypatch.setattr(daemon_mod, "RNS", fake)
    return fake


def test_publish_uses_packet_under_mtu(monkeypatch, tmp_path):
    fake = _install_fake_rns(monkeypatch)
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    d._destination = _FakeDestination()
    d._publish_envelope(b"x" * 100, reliable=False)
    assert d._destination.packets == [b"x" * 100]
    assert d._destination.links == []


def test_publish_promotes_to_link_when_oversize(monkeypatch, tmp_path):
    _install_fake_rns(monkeypatch)
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    d._destination = _FakeDestination()
    big = b"x" * (d.PACKET_MDU + 10)
    d._publish_envelope(big, reliable=False)
    assert d._destination.packets == []
    assert len(d._destination.links) == 1
    link = d._destination.links[0]
    assert link.resources == [big]
    assert link.torn is True


def test_publish_link_when_reliable_even_for_small_payload(monkeypatch, tmp_path):
    _install_fake_rns(monkeypatch)
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    d._destination = _FakeDestination()
    d._publish_envelope(b"hi", reliable=True)
    assert d._destination.packets == []
    assert len(d._destination.links) == 1


def test_teardown_drains_and_detaches(monkeypatch, tmp_path):
    fake = _install_fake_rns(monkeypatch)
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    iface = _FakeIface("eth0")
    fake.Transport.interfaces.append(iface)
    iid = "abc"
    d._iface_runtime[iid] = {
        "id": iid, "name": "eth0", "type": "udp", "iface": iface,
        "up": True, "peers": 0, "bytes_in": 0, "bytes_out": 0,
        "last_error": "",
    }
    d._teardown_interface(iid, drain_s=0.2)
    assert iface.OUT is False
    assert iface.detached is True
    assert iface not in fake.Transport.interfaces
    assert iid not in d._iface_runtime


def test_publish_fans_out_to_known_peers(monkeypatch, tmp_path):
    """Spec: outbound envelopes go to every known peer's OUT
    destination, not just looped to our own IN destination."""
    fake = _install_fake_rns(monkeypatch)
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    d._destination = _FakeDestination()
    peer_a, peer_b = _FakeDestination(), _FakeDestination()
    d._peers = {
        "a" * 16: {"identity": object(), "destination": peer_a,
                   "iface_id": None, "first_seen": 0.0},
        "b" * 16: {"identity": object(), "destination": peer_b,
                   "iface_id": None, "first_seen": 0.0},
    }
    d._publish_envelope(b"x" * 50, reliable=False)
    assert peer_a.packets == [b"x" * 50]
    assert peer_b.packets == [b"x" * 50]
    # When peers are known we MUST NOT also loop to our own IN dest.
    assert d._destination.packets == []


def test_publish_per_interface_reliable_cot_overrides(monkeypatch, tmp_path):
    """Per-interface `reliable_cot=True` forces Link/Resource for that
    peer even when the bridge requested opportunistic Packet."""
    fake = _install_fake_rns(monkeypatch)
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    d.config["interfaces"] = [
        {"id": "if-rel", "name": "lo", "type": "udp",
         "reliable_cot": True, "listen_port": 4242},
        {"id": "if-opp", "name": "lo2", "type": "udp",
         "reliable_cot": False, "listen_port": 4243},
    ]
    peer_rel, peer_opp = _FakeDestination(), _FakeDestination()
    d._peers = {
        "a" * 16: {"identity": object(), "destination": peer_rel,
                   "iface_id": "if-rel", "first_seen": 0.0},
        "b" * 16: {"identity": object(), "destination": peer_opp,
                   "iface_id": "if-opp", "first_seen": 0.0},
    }
    d._publish_envelope(b"hi", reliable=False)
    # if-rel forces Link path despite tiny payload
    assert peer_rel.packets == [] and len(peer_rel.links) == 1
    # if-opp uses opportunistic Packet
    assert peer_opp.packets == [b"hi"] and peer_opp.links == []


def test_import_config_replaces_identity(monkeypatch, tmp_path):
    """Token-borne identity must be written to identity.prv (0600) and
    reloaded so the node hash persists across the import."""
    import os, types
    fake = _install_fake_rns(monkeypatch)

    class _Id:
        def __init__(self, raw): self.raw = raw; self.hash = raw[:32]
        @classmethod
        def from_file(cls, p):
            with open(p, "rb") as f: return cls(f.read())

    fake.Identity = _Id
    fake.hexrep = lambda b, delimit=False: b.hex()
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    seen = {}
    monkeypatch.setattr(daemon_mod, "import_token",
        lambda *a, **k: ({"schema_version": 1, "interfaces": [],
                          "cot_bridge": {}, "peer_allowlist": [],
                          "identity_prv": ("aa" * 64),
                          "identity_pub": ("bb" * 32)}, []))
    monkeypatch.setattr(daemon_mod, "validate_config", lambda c: c)
    res = d.import_config("tok", "pw")
    assert res["identity_replaced"] is True
    assert os.path.exists(d.identity_path)
    with open(d.identity_path, "rb") as f:
        assert f.read() == bytes.fromhex("aa" * 64)
    assert (os.stat(d.identity_path).st_mode & 0o777) == 0o600


def test_restart_records_forced_close(monkeypatch, tmp_path):
    """When teardown can't cleanly close, restart_interface must
    surface forced_close=True and last_error='forced'."""
    fake = _install_fake_rns(monkeypatch)
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    d._running = True
    d.config["interfaces"] = [
        {"id": "i1", "name": "x", "type": "udp", "enabled": True,
         "listen_port": 4242}]

    class _BadIface:
        OUT = True
        def detach(self): raise RuntimeError("stuck")
        def close(self):  raise RuntimeError("stuck")
        def teardown(self): raise RuntimeError("stuck")
        def stop(self):   raise RuntimeError("stuck")
    fake.Transport.interfaces.append(_BadIface())
    d._iface_runtime["i1"] = {
        "id": "i1", "name": "x", "type": "udp",
        "iface": fake.Transport.interfaces[-1],
        "up": True, "peers": 0, "bytes_in": 0, "bytes_out": 0,
        "last_error": ""}
    monkeypatch.setattr(d, "_spawn_interface",
        lambda e: d._iface_runtime.__setitem__(e["id"], {
            "id": e["id"], "name": e["name"], "type": e["type"],
            "up": True, "peers": 0, "bytes_in": 0, "bytes_out": 0,
            "last_error": ""}))
    res = d.restart_interface("i1", drain_timeout_s=0.05,
                              start_timeout_s=0.5)
    assert res["restarted"] is True
    assert res["forced_close"] is True
    assert res["up"] is True
    assert res["timed_out"] is False


def test_status_polls_live_iface_attrs(monkeypatch, tmp_path):
    """status() must reflect live interface attributes (online/rxb/txb/
    bitrate/clients), not just whatever was cached at spawn."""
    fake = _install_fake_rns(monkeypatch)
    d = daemon_mod.RNSDaemon(state_dir=str(tmp_path))
    d.config["interfaces"] = [
        {"id": "i1", "name": "x", "type": "udp", "enabled": True,
         "listen_port": 4242}]
    class _LiveIface:
        online = True
        rxb = 1234
        txb = 5678
        clients = 3
        bitrate = 9600
    d._iface_runtime["i1"] = {
        "id": "i1", "name": "x", "type": "udp",
        "iface": _LiveIface(),
        "up": False, "peers": 0, "bytes_in": 0, "bytes_out": 0,
        "last_error": ""}
    s = d.status()
    row = s["interfaces"][0]
    assert row["up"] is True
    assert row["bytes_in"] == 1234
    assert row["bytes_out"] == 5678
    assert row["peers"] == 3
    assert row["bitrate_bps"] == 9600
