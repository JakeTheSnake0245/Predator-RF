# Predator RF — Correlation Engine & Fleet State (Roadmaps A + C)

## Overview

Two intertwined subsystems:

- **CorrelationEngine** (`backend/coordination/correlation_engine.py`) —
  operator-defined rules that fire when a configurable number of nodes hear
  a signal in the same band within a time window. Also handles known-target
  fingerprint match response (geo-cue + intercept log).

- **FleetStateManager** (`backend/coordination/fleet_state_manager.py`) —
  in-memory snapshot of every peer node's current tracks, events, GPS, and
  SDR status. Updated from the KujhadClient polling loop. Provides a global
  event ring with monotonically increasing serial numbers for lossless
  client catch-up on reconnect.

---

## CorrelationEngine

### `CorrelationRule` dataclass

```python
@dataclass
class CorrelationRule:
    rule_id:    str           # UUID or operator-assigned ID
    name:       str           # human label
    nodes:      List[str]     # [] means any node
    freq_lo_hz: float         # band low edge
    freq_hi_hz: float         # band high edge
    window_s:   float         # observation time window (seconds)
    min_nodes:  int           # minimum distinct nodes to fire (default 2)
    action:     str           # "alert" | "geo_cue" | "intercept_log"
    enabled:    bool = True
```

### Methods

```python
engine = CorrelationEngine(store, signal_repo, auto_tasker)
engine.on_alert(callback: Callable[[dict], None])   # subscribe to alerts
engine.add_rule(rule: CorrelationRule)
engine.remove_rule(rule_id: str) → bool
engine.list_rules() → List[dict]
engine.on_event(node_id, freq_hz, power_dbfs=0.0)   # call from RF event path
await engine.on_known_target_detected(track, signal_id, similarity, node_ids)
```

### Firing logic

1. `on_event()` records `(node_id, freq_hz, timestamp)` in a per-rule
   sliding window deque.
2. Window entries older than `rule.window_s` are pruned on each call.
3. If the number of **distinct** node IDs in the window reaches
   `rule.min_nodes`, the rule fires.
4. Node filter: if `rule.nodes` is non-empty, only events from those nodes
   count toward the threshold.
5. Fired rules call `alert_callbacks` with a dict:
   ```json
   {
     "type": "correlation_rule",
     "rule_id": "...",
     "rule_name": "...",
     "freq_hz": 433920000,
     "node_ids": ["node-A", "node-B"],
     "ts": 1234567890.0
   }
   ```

### Known-target response (`on_known_target_detected`)

Fired from `PredatorBackend._check_fingerprint_match()` when a STABLE
track's fingerprint matches a stored signal above the similarity threshold:

1. Saves a `correlated_intercept` row in `SignalRepository`.
2. Fires the alert callback with `"type": "known_target_match"`.
3. If `auto_tasker` is available, issues a geo-cue retask via
   `AutoTasker.handle_assessment()` to bring additional nodes onto the
   frequency.

---

## FleetStateManager

### NodeSnapshot

```python
@dataclass
class NodeSnapshot:
    node_id:       str
    last_seen_ns:  int
    tracks:        List[Dict]
    recent_events: List[Dict]
    gps:           Dict          # {"lat": ..., "lon": ...}
    timing:        Dict
    hw_profile:    str
    version:       str
    role:          str           # "device" | "controller"
    sdr_running:   bool
    center_freq:   float
    scan_running:  bool
    is_online:     bool          # property — stale after 120 s
```

### Methods

```python
mgr = FleetStateManager(store=None)
mgr.update_node_snapshot(node_id, *, tracks, events, gps, timing, status)
mgr.get_fleet_snapshot() → Dict[str, NodeSnapshot]
mgr.get_node(node_id) → Optional[NodeSnapshot]
mgr.node_ids() → List[str]
mgr.online_node_ids() → List[str]
mgr.get_events_since(serial: int) → List[dict]  # serial is inclusive (>=)
mgr.get_all_tracks() → List[dict]               # de-duplicated by emitter_id
mgr.serialize() → dict                           # JSON-safe summary
```

### Event ring

All events from all nodes are appended to a global ring of max 4096 entries.
Each entry gets a monotonically increasing `_fleet_serial` and `_node_id`
field. `get_events_since(serial)` returns entries with `_fleet_serial >= serial`,
so clients can request exactly the events they missed since their last poll.

### Track deduplication

`get_all_tracks()` merges tracks across nodes by `emitter_id`. When the same
emitter appears in multiple nodes' snapshots, the higher `last_seen_ns` wins
for field values, and an `_observing_nodes` list accumulates all node IDs.

---

## IQCaptureService

`backend/coordination/iq_capture_service.py`

Operator-demand IQ recording. Takes a signal_id and node_id, sends a
`{"class": "iq", "action": "capture", "args": {...}}` command to the
target Device node via `KujhadClient`, then records the resulting file path
in `SignalRepository`.

```python
svc = IQCaptureService(signal_repo, fleet_manager)
capture_id = await svc.request_capture(
    signal_id, node_id, duration_s, sample_rate_hz, center_hz)
```

---

## SSE events pushed to operator dashboard

| `kind` field | Fired when |
|---|---|
| `correlation_alert` | A CorrelationRule fires or a known-target match occurs |
| `custody_change` | Track primary node changes |

---

## Test suite

```
python -m unittest backend.tests.test_signal_repository -v
```

Covers: rule CRUD, single-node no-fire, two-node fire, out-of-band ignore,
node filter, fleet snapshot, event ring, track deduplication, serialization.
