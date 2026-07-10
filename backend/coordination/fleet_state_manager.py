"""
FleetStateManager — shared fleet state replication across all sensor nodes.

Problem it solves:
  Today each node is a silo.  When node-A drops and reconnects, it has no
  memory of what node-B saw while it was down.  The operator workstation loses
  the consolidated picture on any single-node outage.

Solution:
  A single in-memory + SQLite snapshot of every node's current tracks and
  events, updated via the existing KujhadClient polling loop.  New nodes
  receive a state diff since their last-known serial number on reconnect,
  so no events are lost.

API:
    mgr.update_node_snapshot(node_id, tracks, events, gps, timing)
    mgr.get_fleet_snapshot() → {node_id: NodeSnapshot, ...}
    mgr.get_events_since(serial) → List[dict]
    mgr.node_ids() → List[str]
    mgr.get_node(node_id) → NodeSnapshot | None

NodeSnapshot fields:
    node_id, last_seen_ns, tracks (list), recent_events (list),
    gps (dict), timing (dict), is_online (bool)
"""
from __future__ import annotations

import logging
import time
import threading
from dataclasses import dataclass, field
from typing import Dict, List, Optional

logger = logging.getLogger(__name__)

_NODE_STALE_S = 120.0


@dataclass
class NodeSnapshot:
    node_id:       str
    last_seen_ns:  int = 0
    tracks:        List[Dict] = field(default_factory=list)
    recent_events: List[Dict] = field(default_factory=list)
    gps:           Dict = field(default_factory=dict)
    timing:        Dict = field(default_factory=dict)
    hw_profile:    str  = ""
    version:       str  = ""
    role:          str  = "device"
    sdr_running:   bool = False
    center_freq:   float = 0.0
    scan_running:  bool = False

    @property
    def is_online(self) -> bool:
        age_s = (time.time_ns() - self.last_seen_ns) / 1e9
        return self.last_seen_ns > 0 and age_s < _NODE_STALE_S


class FleetStateManager:
    def __init__(self, store=None):
        self._store  = store
        self._lock   = threading.Lock()
        self._nodes: Dict[str, NodeSnapshot] = {}
        self._global_event_ring: List[Dict]  = []
        self._global_serial: int = 0
        self._MAX_RING = 4096

    def update_node_snapshot(self,
                              node_id: str,
                              *,
                              tracks: Optional[List[Dict]] = None,
                              events: Optional[List[Dict]] = None,
                              gps: Optional[Dict] = None,
                              timing: Optional[Dict] = None,
                              status: Optional[Dict] = None):
        now = time.time_ns()
        with self._lock:
            snap = self._nodes.setdefault(node_id, NodeSnapshot(node_id=node_id))
            snap.last_seen_ns = now
            if tracks is not None:
                snap.tracks = tracks
            if events is not None:
                snap.recent_events = events
                for ev in events:
                    ev = dict(ev)
                    ev["_node_id"]     = node_id
                    ev["_fleet_serial"] = self._global_serial
                    self._global_serial += 1
                    self._global_event_ring.append(ev)
                while len(self._global_event_ring) > self._MAX_RING:
                    self._global_event_ring.pop(0)
            if gps is not None:
                snap.gps = gps
            if timing is not None:
                snap.timing = timing
            if status is not None:
                snap.sdr_running  = status.get("sdr_running", snap.sdr_running)
                snap.center_freq  = status.get("center_freq", snap.center_freq)
                snap.scan_running = status.get("scan_running", snap.scan_running)
                snap.role         = status.get("role", snap.role)
                snap.hw_profile   = status.get("hw_profile", snap.hw_profile)
                snap.version      = status.get("version", snap.version)

    def get_fleet_snapshot(self) -> Dict[str, NodeSnapshot]:
        with self._lock:
            return dict(self._nodes)

    def get_node(self, node_id: str) -> Optional[NodeSnapshot]:
        with self._lock:
            return self._nodes.get(node_id)

    def node_ids(self) -> List[str]:
        with self._lock:
            return list(self._nodes.keys())

    def online_node_ids(self) -> List[str]:
        with self._lock:
            return [nid for nid, snap in self._nodes.items() if snap.is_online]

    def get_events_since(self, serial: int) -> List[Dict]:
        """Return all events with _fleet_serial >= serial."""
        with self._lock:
            return [ev for ev in self._global_event_ring
                    if ev.get("_fleet_serial", 0) >= serial]

    def get_all_tracks(self) -> List[Dict]:
        """Merged track list across all online nodes (de-duplicated by emitter_id)."""
        seen: Dict[str, Dict] = {}
        with self._lock:
            for snap in self._nodes.values():
                if not snap.is_online:
                    continue
                for t in snap.tracks:
                    eid = t.get("emitter_id", "")
                    if not eid:
                        continue
                    existing = seen.get(eid)
                    if existing is None:
                        seen[eid] = dict(t)
                        seen[eid]["_observing_nodes"] = [snap.node_id]
                    else:
                        if t.get("last_seen_ns", 0) > existing.get("last_seen_ns", 0):
                            seen[eid].update(t)
                        existing_nodes = seen[eid].setdefault("_observing_nodes", [])
                        if snap.node_id not in existing_nodes:
                            existing_nodes.append(snap.node_id)
        return list(seen.values())

    def serialize(self) -> Dict:
        with self._lock:
            return {
                "nodes": {
                    nid: {
                        "node_id":      s.node_id,
                        "is_online":    s.is_online,
                        "last_seen_ns": s.last_seen_ns,
                        "sdr_running":  s.sdr_running,
                        "center_freq":  s.center_freq,
                        "scan_running": s.scan_running,
                        "role":         s.role,
                        "gps":          s.gps,
                        "timing":       s.timing,
                        "track_count":  len(s.tracks),
                    }
                    for nid, s in self._nodes.items()
                },
                "fleet_serial": self._global_serial,
                "online_count": sum(1 for s in self._nodes.values() if s.is_online),
            }
