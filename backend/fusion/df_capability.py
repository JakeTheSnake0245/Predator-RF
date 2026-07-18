"""
Fleet DF (direction-finding) capability assessment.

Problem: in a fleet of HackRF/RTL-SDR nodes (no KrakenSDR), the system
produces zero bearings and silently falls back to GPS-timing-dependent
TDOA or the 0.20-confidence RSSI proximity ring. The operator is never
told they lack DF capability. This module derives an explicit fleet
capability summary from node hardware codes and timing trust so a
bearing-less fleet fails LOUDLY instead of quietly.

Definitions:
  - LOB-capable: node hardware can produce a line of bearing
    (KrakenSDR coherent array — hardware_code contains "kraken" or the
    node advertises a KRAKEN_LOB detector).
  - Hardware-timed: node has a dedicated TDOA timing path
    (can_do_tdoa hardware — GPSDO/OCXO or an SDR with a coherent
    timestamping path).
  - TDOA-viable: node is online, has a fresh GPS fix, and is either
    hardware-timed or a system-clock node (system-clock nodes only
    count toward TDOA when >= 3 of them are viable — mirrors the
    SYSTEM_CLOCK_MIN_DISTINCT gate in tdoa_coordinator).

Pure/stateless — pass in the node list, get a dict. No I/O, no config.
"""
from __future__ import annotations

import time
from typing import Iterable, Optional

from backend.fusion.tdoa_coordinator import SYSTEM_CLOCK_MIN_DISTINCT

# Substrings of hardware_code that indicate LOB (bearing) capability.
_LOB_HARDWARE_HINTS = ("kraken",)
_LOB_DETECTOR_NAMES = ("KRAKEN_LOB",)


def _node_is_lob_capable(node) -> bool:
    hw = str(getattr(node, "hardware_code", "") or "").lower()
    if any(h in hw for h in _LOB_HARDWARE_HINTS):
        return True
    detectors = getattr(node, "available_detectors", None) or []
    return any(d in detectors for d in _LOB_DETECTOR_NAMES)


def _node_gps_fresh(node, max_age_s: float, now_ns: Optional[int]) -> bool:
    """Fresh GPS = has a position AND (no freshness timestamp supplied,
    which means the caller opted out of gating, OR the lock is younger
    than max_age_s). Mirrors TDOACoordinator.record_measurement."""
    if not getattr(node, "location_gps", None):
        return False
    gps_ts = getattr(node, "location_gps_updated_ns", 0) or 0
    if gps_ts <= 0:
        return True
    now = now_ns if now_ns is not None else time.time_ns()
    return (now - gps_ts) / 1e9 <= max_age_s


def compute_df_capability(nodes: Iterable,
                          gps_max_age_s: float = 60.0,
                          now_ns: Optional[int] = None) -> dict:
    """Compute the fleet DF capability summary.

    `nodes` is any iterable of SensorNodeTrust-like objects (getattr
    with defaults throughout, so shims/test fakes work). Offline nodes
    are counted in totals but excluded from capability counts — a
    powered-off KrakenSDR provides no bearings.
    """
    nodes = list(nodes)
    online = [n for n in nodes if getattr(n, "is_online", False)]

    lob_nodes = [n.node_id for n in online if _node_is_lob_capable(n)]

    hw_timed, sys_clock = [], []
    for n in online:
        if not _node_gps_fresh(n, gps_max_age_s, now_ns):
            continue
        if getattr(n, "can_do_tdoa", False):
            hw_timed.append(n.node_id)
        else:
            sys_clock.append(n.node_id)

    # TDOA viability mirrors the solver's gating:
    #   - >= 2 hearers when at least one is hardware-timed
    #   - >= SYSTEM_CLOCK_MIN_DISTINCT hearers when all are system-clock
    total_gps = len(hw_timed) + len(sys_clock)
    if hw_timed:
        tdoa_viable = total_gps >= 2
    else:
        tdoa_viable = total_gps >= SYSTEM_CLOCK_MIN_DISTINCT
    tdoa_nodes = (hw_timed + sys_clock) if tdoa_viable else []

    if lob_nodes:
        df_mode = "lob"
    elif tdoa_viable:
        df_mode = "tdoa"
    elif online:
        df_mode = "rssi_only"
    else:
        df_mode = "none"

    warning = None
    if df_mode == "rssi_only":
        warning = ("No DF hardware — TDOA/RSSI only. Fleet has no "
                   "LOB-capable node and too few timing/GPS-trusted "
                   "nodes for TDOA. Location estimates are proximity "
                   "rings, not fixes.")
    elif df_mode == "tdoa" and not hw_timed:
        warning = ("No LOB-capable node online. TDOA runs on "
                   "system-clock timing only — fixes are confidence-"
                   f"capped and require >={SYSTEM_CLOCK_MIN_DISTINCT} "
                   "distinct hearers.")
    elif df_mode == "tdoa":
        warning = ("No LOB-capable node online — bearings unavailable, "
                   "geolocation is TDOA-only.")
    elif df_mode == "none":
        warning = "No sensor nodes online."

    return {
        "df_mode": df_mode,                      # lob | tdoa | rssi_only | none
        "lob_capable_count": len(lob_nodes),
        "lob_capable_nodes": lob_nodes,
        "tdoa_viable": tdoa_viable,
        "tdoa_viable_count": len(tdoa_nodes),
        "tdoa_viable_nodes": tdoa_nodes,
        "hardware_timed_count": len(hw_timed),
        "system_clock_count": len(sys_clock),
        "system_clock_min_distinct": SYSTEM_CLOCK_MIN_DISTINCT,
        "rssi_only": df_mode == "rssi_only",
        "online_node_count": len(online),
        "total_node_count": len(nodes),
        "warning": warning,
    }
