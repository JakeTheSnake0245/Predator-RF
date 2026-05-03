"""Shared config schema for Predator RF RNS interfaces.

The exact field list is frozen in `.local/tasks/task-27.md` section B.
Both the daemon and the Kujhad UI call into here for validation so a
field that is rejected in the UI is also rejected by the daemon.

Device-local fields are marked in `DEVICE_LOCAL_FIELDS`. Token export
replaces them with `{"$placeholder": "<field_path>"}` markers; token
import re-prompts the operator for them.
"""
from __future__ import annotations

import uuid
from typing import Any, Dict, List, Tuple


class SchemaError(ValueError):
    """Raised when a config field fails validation."""


# Per-type field specs. Each entry is (name, type, required, validator|None).
# `validator` is a callable that returns None for OK, str for error.

def _is_int(v: Any) -> bool:
    return isinstance(v, bool) is False and isinstance(v, int)


def _port(v: Any) -> str | None:
    if not _is_int(v) or not (1 <= v <= 65535):
        return "must be int 1..65535"
    return None


def _positive_int(v: Any) -> str | None:
    if not _is_int(v) or v < 0:
        return "must be non-negative int"
    return None


def _positive_int_strict(v: Any) -> str | None:
    if not _is_int(v) or v <= 0:
        return "must be positive int"
    return None


def _str_nonempty(v: Any) -> str | None:
    if not isinstance(v, str) or not v.strip():
        return "must be non-empty string"
    return None


def _str_optional(v: Any) -> str | None:
    if not isinstance(v, str):
        return "must be string"
    return None


def _bool(v: Any) -> str | None:
    if not isinstance(v, bool):
        return "must be bool"
    return None


def _str_list(v: Any) -> str | None:
    if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
        return "must be list of strings"
    return None


def _enum(allowed: List[str]):
    def check(v: Any) -> str | None:
        if v not in allowed:
            return f"must be one of {allowed}"
        return None
    return check


def _int_range(lo: int, hi: int):
    def check(v: Any) -> str | None:
        if not _is_int(v) or not (lo <= v <= hi):
            return f"must be int {lo}..{hi}"
        return None
    return check


# Common fields present on every interface entry.
COMMON_FIELDS: List[Tuple[str, type | tuple, bool, Any]] = [
    ("id", str, False, None),  # auto-generated if missing
    ("name", str, True, _str_nonempty),
    ("type", str, True, _str_nonempty),
    ("enabled", bool, False, _bool),
    ("mode", str, False, _enum(
        ["full", "gateway", "access_point", "roaming", "boundary"])),
    ("outgoing", bool, False, _bool),
    ("bitrate_hint_bps", int, False, _positive_int),
    ("announce_interval_s", int, False, _positive_int),
    ("notes", str, False, _str_optional),
    ("reliable_cot", bool, False, _bool),
    # Reticulum Interface Access Code (IFAC) — optional per-interface
    # gate that hashes every Reticulum frame with a pre-shared netkey
    # so foreign nodes (without the same netname/netkey) can't even
    # decode link-layer framing. Three fields, all optional, must be
    # set together to actually take effect (RNS requires netname AND
    # netkey; size is the truncation length of the keyed hash).
    ("ifac_size", int, False, _int_range(8, 512)),
    ("ifac_netname", str, False, _str_optional),
    ("ifac_netkey", str, False, _str_optional),
]

PER_TYPE_FIELDS: Dict[str, List[Tuple[str, type | tuple, bool, Any]]] = {
    "tcp_client": [
        ("target_host", str, True, _str_nonempty),
        ("target_port", int, True, _port),
        ("kiss_framing", bool, False, _bool),
        ("i2p_tunneled", bool, False, _bool),
    ],
    "tcp_server": [
        ("listen_address", str, False, _str_nonempty),
        ("listen_port", int, True, _port),
        ("prefer_ipv6", bool, False, _bool),
        ("i2p_tunneled", bool, False, _bool),
    ],
    "udp": [
        ("listen_address", str, False, _str_nonempty),
        ("listen_port", int, True, _port),
        ("forward_address", str, False, _str_optional),
        ("forward_port", int, False, _port),
    ],
    "i2p": [
        ("peers", list, False, _str_list),
        ("connectable", bool, False, _bool),
        ("i2p_sam_address", str, False, _str_nonempty),
    ],
    "auto_interface": [
        ("group_id", str, True, _str_nonempty),
        ("discovery_scope", str, False, _enum(
            ["link", "admin", "site", "organisation", "global"])),
        ("discovery_port", int, False, _port),
        ("data_port", int, False, _port),
        ("allowed_interfaces", list, False, _str_list),
        ("ignored_interfaces", list, False, _str_list),
    ],
    "rnode": [
        ("port", str, True, _str_nonempty),
        ("frequency_hz", int, True, _positive_int_strict),
        ("bandwidth_hz", int, True, _positive_int_strict),
        ("txpower_dbm", int, True, _int_range(-10, 30)),
        ("spreadingfactor", int, True, _int_range(7, 12)),
        ("codingrate", int, True, _int_range(5, 8)),
        ("flow_control", bool, False, _bool),
        ("id_callsign", str, False, _str_optional),
        ("id_interval_s", int, False, _positive_int),
    ],
    "kiss_tnc": [
        ("port", str, True, _str_nonempty),
        ("speed_baud", int, True, _positive_int_strict),
        ("databits", int, False, _int_range(5, 8)),
        ("parity", str, False, _enum(["none", "even", "odd"])),
        ("stopbits", int, False, _int_range(1, 2)),
        ("preamble_ms", int, False, _positive_int),
        ("txtail_ms", int, False, _positive_int),
        ("persistence", int, False, _int_range(0, 255)),
        ("slottime_ms", int, False, _positive_int),
        ("flow_control", bool, False, _bool),
        ("beacon_interval_s", int, False, _positive_int),
        ("beacon_data", str, False, _str_optional),
    ],
    "ax25_kiss": [
        ("port", str, True, _str_nonempty),
        ("speed_baud", int, True, _positive_int_strict),
        ("databits", int, False, _int_range(5, 8)),
        ("parity", str, False, _enum(["none", "even", "odd"])),
        ("stopbits", int, False, _int_range(1, 2)),
        ("preamble_ms", int, False, _positive_int),
        ("txtail_ms", int, False, _positive_int),
        ("persistence", int, False, _int_range(0, 255)),
        ("slottime_ms", int, False, _positive_int),
        ("flow_control", bool, False, _bool),
        ("beacon_interval_s", int, False, _positive_int),
        ("beacon_data", str, False, _str_optional),
        ("callsign", str, True, _str_nonempty),
        ("ssid", int, True, _int_range(0, 15)),
        ("axint_port", str, True, _str_nonempty),
    ],
    "pipe": [
        ("command", str, True, _str_nonempty),
        ("respawn_delay_s", int, False, _positive_int),
    ],
}

INTERFACE_TYPES = tuple(PER_TYPE_FIELDS.keys())

# Device-local fields per interface type — these get replaced by
# placeholders during token export and re-prompted during import.
DEVICE_LOCAL_FIELDS: Dict[str, Tuple[str, ...]] = {
    "tcp_client": (),
    "tcp_server": ("listen_address",),
    "udp": ("listen_address",),
    "i2p": ("i2p_sam_address",),
    "auto_interface": ("allowed_interfaces", "ignored_interfaces"),
    "rnode": ("port",),
    "kiss_tnc": ("port",),
    "ax25_kiss": ("port", "axint_port"),
    "pipe": (),
}


def _check_fields(cfg: Dict[str, Any],
                  fields: List[Tuple[str, type | tuple, bool, Any]],
                  ctx: str) -> None:
    for name, _typ, required, validator in fields:
        if name not in cfg:
            if required:
                raise SchemaError(f"{ctx}: missing required field '{name}'")
            continue
        v = cfg[name]
        if validator is not None:
            err = validator(v)
            if err:
                raise SchemaError(f"{ctx}: field '{name}' {err}")


def validate_interface(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Validate one interface entry. Raises SchemaError on failure.
    Returns a normalized copy with defaults filled in and an `id` minted
    if absent.
    """
    if not isinstance(cfg, dict):
        raise SchemaError("interface entry must be an object")
    iftype = cfg.get("type")
    if iftype not in PER_TYPE_FIELDS:
        raise SchemaError(
            f"unknown interface type: {iftype!r} "
            f"(allowed: {list(PER_TYPE_FIELDS)})")
    allowed = {n for n, *_ in COMMON_FIELDS} | {
        n for n, *_ in PER_TYPE_FIELDS[iftype]}
    for k in cfg:
        if k not in allowed:
            raise SchemaError(
                f"interface {cfg.get('name', '?')}: unknown field {k!r}")
    ctx = f"interface[{cfg.get('name', '?')}/{iftype}]"
    _check_fields(cfg, COMMON_FIELDS, ctx)
    _check_fields(cfg, PER_TYPE_FIELDS[iftype], ctx)
    out = dict(cfg)
    out.setdefault("id", str(uuid.uuid4()))
    out.setdefault("enabled", True)
    out.setdefault("mode", "full")
    out.setdefault("outgoing", True)
    # Per spec section C: reliable_cot defaults to FALSE on LoRa
    # (rnode) and TRUE on every link-layer where Link/Resource is
    # cheap (TCP/UDP/I2P/Auto/Pipe + KISS variants which sit over
    # serial/AX.25 framing). Operators override per-interface.
    if "reliable_cot" not in out:
        out["reliable_cot"] = (iftype != "rnode")
    return out


def validate_config(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Validate the full config (`{interfaces: [...], ...}`)."""
    if not isinstance(cfg, dict):
        raise SchemaError("config must be an object")
    interfaces = cfg.get("interfaces", [])
    if not isinstance(interfaces, list):
        raise SchemaError("`interfaces` must be a list")
    norm: List[Dict[str, Any]] = []
    seen_ids: set[str] = set()
    seen_names: set[str] = set()
    for entry in interfaces:
        v = validate_interface(entry)
        if v["id"] in seen_ids:
            raise SchemaError(f"duplicate interface id: {v['id']}")
        if v["name"] in seen_names:
            raise SchemaError(f"duplicate interface name: {v['name']}")
        seen_ids.add(v["id"])
        seen_names.add(v["name"])
        norm.append(v)
    out = dict(cfg)
    out["interfaces"] = norm
    out.setdefault("schema_version", 1)
    out.setdefault("cot_bridge", {})
    out.setdefault("peer_allowlist", [])
    return out


def placeholder_paths(cfg: Dict[str, Any]) -> List[str]:
    """Return JSON-pointer-ish paths to every device-local field actually
    present on the (validated) config. Used by token export to swap them
    out and by token import to re-prompt the operator."""
    paths: List[str] = []
    for i, entry in enumerate(cfg.get("interfaces", [])):
        iftype = entry.get("type")
        for fname in DEVICE_LOCAL_FIELDS.get(iftype, ()):
            if fname in entry:
                paths.append(f"interfaces.{i}.{fname}")
    return paths
