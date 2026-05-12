"""
CustodyElector — sensor-custody / N-best election for emitter tracks.

Background
----------
Before this module existed, `DecisionEngine._select_nodes_for_tasking()`
made an ad-hoc selection per assessment with no scoring contract:

  * For high/critical threats it returned every TDOA-capable node.
  * Otherwise it returned every node already monitoring the band.

That worked for v1 but had three operational gaps:

  1. **No election** — every assessment re-tasked every eligible node, so
     two equally-good nodes both got tuned to the same emitter,
     wasting RF time and burning radio dwell budget. There was no
     concept of a *primary* sensor with named *backups*.

  2. **No stand-down** — a node that was tasked yesterday and is no
     longer the best fit kept getting re-tasked because nobody told it
     to drop the track. The track simply fell off the auto-tasker's
     `recommended_nodes` list and the node coasted on its previous tune
     until the operator intervened.

  3. **No handover overlap** — when conditions changed (a closer node
     came online, the original primary went thermal), tasking flipped
     in a single tick with no overlap window, so the new primary's
     first observation was an empty band while the old primary was
     already gone.

This module replaces that logic with a deterministic, scored election
producing a `CustodyDecision` per track per re-election cycle.

Design contract
---------------
* **Pure scoring** — `_score_node()` is side-effect-free and depends
  only on its arguments, so it's trivially unit-testable and the
  scoring weights can be tuned without touching call sites.
* **Hard gates first**, weighted soft score after — a node missing a
  required decoder or GPS-sync for a TDOA-required threat scores 0
  with a `rejected_reason` so the operator can see WHY.
* **Per-track previous-decision cache** keyed by `track_id` so the
  caller doesn't have to thread state through; `elect()` accepts an
  optional `previous` override for tests.
* **Handover overlap** is expressed in the decision itself
  (`handover_from`, `tasked_nodes` includes the outgoing primary
  until `handover_until_ns`), so AutoTasker keeps the old node
  tuned until the new primary has a clean track.
* **Backwards compatible** — `DecisionEngine.assess()` populates
  `recommended_nodes` from `custody.tasked_nodes`; existing AutoTasker
  code path keeps working without change.
"""
from __future__ import annotations

import logging
import math
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

from backend.models.emitter_track import EmitterTrack
from backend.models.sensor_node import SensorNodeTrust

logger = logging.getLogger(__name__)


# ── Tunable defaults ───────────────────────────────────────────────────────

# Default total nodes elected (1 primary + K-1 backups). 3 is the sweet
# spot: enough for TDOA (needs 3+ GPS-synced nodes) without burning the
# whole fleet on one emitter.
DEFAULT_K_TOTAL = 3

# Overlap window during which the old primary stays tasked alongside the
# new one after a handover. 15 s is comfortably longer than typical
# tune-and-resync latency on a HackRF/RTL-SDR (1-3 s) so the new primary
# almost always has a clean track before the old one stands down.
DEFAULT_HANDOVER_OVERLAP_S = 15.0

# Above this GPS-fix age the node's location is treated as unreliable.
# 5 minutes covers ordinary operator pauses; longer than that and the
# operator has likely moved without re-fixing.
DEFAULT_STALE_GPS_AFTER_S = 300.0

# Soft-score weights. Sum should be ~1.0 for readable totals; the
# election only cares about rank order so absolute scale doesn't matter.
DEFAULT_WEIGHTS: Dict[str, float] = {
    "snr":      0.30,   # detecting_nodes membership + signal quality proxy
    "distance": 0.20,   # node-to-track geometry (closer is better)
    "gps_age":  0.10,   # GPS freshness
    "trust":    0.20,   # SensorNodeTrust.compute_trust_score()
    "load":     0.10,   # current custody load (fewer is better)
    "decoder":  0.10,   # bonus when node has the right decoder
}


# ── Result dataclasses ────────────────────────────────────────────────────


@dataclass
class CustodyScore:
    """Per-node score breakdown for one election. Kept on the decision
    so the operator UI / logs can explain WHY a node won or lost."""
    node_id: str
    total: float = 0.0                          # 0..1
    components: Dict[str, float] = field(default_factory=dict)
    rejected_reason: str = ""                   # set when hard-gated out

    def to_dict(self) -> dict:
        return {
            "node_id": self.node_id,
            "total": round(self.total, 4),
            "components": {k: round(v, 4) for k, v in self.components.items()},
            "rejected_reason": self.rejected_reason,
        }


@dataclass
class CustodyDecision:
    """Result of one election cycle for one track."""
    track_id: str
    decided_ns: int = field(default_factory=time.time_ns)

    # Tasking outputs
    primary: Optional[str] = None               # node_id of elected primary
    backups: List[str] = field(default_factory=list)        # ordered, K-1 long
    tasked_nodes: List[str] = field(default_factory=list)   # full keep-tuned set
    stand_down: List[str] = field(default_factory=list)     # release these

    # Handover bookkeeping
    handover_from: Optional[str] = None         # previous primary, if changed
    handover_until_ns: int = 0                  # overlap deadline (0 = no handover)

    # Explainability
    scores: List[CustodyScore] = field(default_factory=list)
    reason: str = ""

    def to_dict(self) -> dict:
        return {
            "track_id": self.track_id,
            "decided_ns": self.decided_ns,
            "primary": self.primary,
            "backups": list(self.backups),
            "tasked_nodes": list(self.tasked_nodes),
            "stand_down": list(self.stand_down),
            "handover_from": self.handover_from,
            "handover_until_ns": self.handover_until_ns,
            "scores": [s.to_dict() for s in self.scores],
            "reason": self.reason,
        }

    def is_handover(self) -> bool:
        return self.handover_from is not None


# ── Geo helper ────────────────────────────────────────────────────────────


def _haversine_m(a_lat: float, a_lon: float,
                 b_lat: float, b_lon: float) -> float:
    """Great-circle distance in metres between two WGS-84 points.
    Local copy (rather than importing fusion.cross_station_dedup) so
    this module stays free of fusion-layer dependencies and doesn't
    cycle through the dedup / track-manager imports."""
    R = 6_371_000.0
    a_lat_r = math.radians(a_lat)
    b_lat_r = math.radians(b_lat)
    d_lat = b_lat_r - a_lat_r
    d_lon = math.radians(b_lon - a_lon)
    h = (math.sin(d_lat / 2) ** 2
         + math.cos(a_lat_r) * math.cos(b_lat_r) * math.sin(d_lon / 2) ** 2)
    return 2 * R * math.asin(math.sqrt(h))


# ── Elector ───────────────────────────────────────────────────────────────


class CustodyElector:
    """Stateful elector. Holds previous-decision cache so callers don't
    have to thread it through. Per-track state lives in `_last_decisions`
    keyed by `track.emitter_id`."""

    def __init__(self,
                 *,
                 k_total: int = DEFAULT_K_TOTAL,
                 handover_overlap_s: float = DEFAULT_HANDOVER_OVERLAP_S,
                 stale_gps_after_s: float = DEFAULT_STALE_GPS_AFTER_S,
                 weights: Optional[Dict[str, float]] = None,
                 on_change: Optional[Callable[[CustodyDecision], None]] = None):
        if k_total < 1:
            raise ValueError("k_total must be >= 1")
        self.k_total = int(k_total)
        self.handover_overlap_s = float(handover_overlap_s)
        self.stale_gps_after_s = float(stale_gps_after_s)
        self.weights: Dict[str, float] = dict(DEFAULT_WEIGHTS)
        if weights:
            self.weights.update(weights)
        # Optional callback fired only when the primary actually
        # changes (not on every re-confirmation). Wired by the
        # backend to the SSE channel.
        self._on_change = on_change

        # Per-track decision cache. Bounded by track lifetime — when
        # TrackManager archives a track it should call forget(track_id)
        # to release the entry.
        self._last_decisions: Dict[str, CustodyDecision] = {}

        # Cheap stats for /metrics + tests.
        self.elections_total = 0
        self.elections_with_handover = 0
        self.elections_no_eligible_node = 0

    # ── Public API ───────────────────────────────────────────────────────

    def elect(self,
              track: EmitterTrack,
              available_nodes: List[SensorNodeTrust],
              *,
              previous: Optional[CustodyDecision] = None,
              now_ns: Optional[int] = None,
              node_loads: Optional[Dict[str, int]] = None) -> CustodyDecision:
        """Run one election cycle for `track`.

        Args:
            track: The emitter track whose custody we are deciding.
            available_nodes: All sensor nodes currently online.
            previous: Optional prior decision override. When None, the
                cached entry for `track.emitter_id` is used (or empty if
                this is the first election for this track).
            now_ns: UNIX-ns for "now". Defaults to time.time_ns(); the
                override is for deterministic tests.
            node_loads: Optional map node_id → current custody count
                (number of tracks for which the node is primary or
                backup). Used by the load score; defaults to all-zero.

        Returns:
            The new CustodyDecision. Also stored in the internal cache
            and (when the primary changed and on_change is set) emitted
            via the on_change callback.
        """
        now_ns = int(now_ns) if now_ns is not None else time.time_ns()
        node_loads = dict(node_loads or {})
        if previous is None:
            previous = self._last_decisions.get(track.emitter_id)

        # Score every available node. Stable sort by node_id within
        # equal scores so the election is deterministic across runs.
        scores: List[CustodyScore] = []
        for node in available_nodes:
            scores.append(self._score_node(track, node, now_ns,
                                           node_loads.get(node.node_id, 0)))
        scores.sort(key=lambda s: (-s.total, s.node_id))

        eligible = [s for s in scores if s.total > 0.0]
        primary = eligible[0].node_id if eligible else None
        backups = [s.node_id for s in eligible[1:self.k_total]]

        # Handover logic. Two cases produce a handover_from in the
        # current decision:
        #
        #   (a) NEW handover this tick — the elected primary differs
        #       from the previous primary AND the previous primary is
        #       still in available_nodes (otherwise the old node is
        #       offline and there's nothing to overlap with).
        #
        #   (b) IN-PROGRESS handover from a previous tick — the prior
        #       decision already set a handover_from and we're still
        #       inside `handover_until_ns`. Without this branch the
        #       old primary is correctly added to tasked_nodes on the
        #       election where the handover STARTS but is dropped on
        #       the next re-election cycle, which collapses the
        #       overlap window to a single tick (~ms in tests, but a
        #       few seconds in prod). The old primary must stay
        #       tasked across every election that falls inside the
        #       overlap window so AutoTasker keeps it tuned.
        handover_from: Optional[str] = None
        handover_until_ns = 0
        prev_primary = previous.primary if previous else None
        available_ids = {n.node_id for n in available_nodes}
        prev_primary_still_available = (prev_primary is not None
                                         and prev_primary in available_ids)
        if (prev_primary is not None
                and prev_primary != primary
                and prev_primary_still_available):
            # Case (a): start a new handover window.
            handover_from = prev_primary
            handover_until_ns = now_ns + int(self.handover_overlap_s * 1e9)
            self.elections_with_handover += 1
        elif (previous is not None
                and previous.handover_from is not None
                and previous.handover_from != primary
                and previous.handover_from in available_ids
                and now_ns < previous.handover_until_ns):
            # Case (b): inherit an in-progress handover. We keep the
            # SAME deadline (do NOT reset the timer) so a long-running
            # track with stable primary doesn't permanently keep the
            # outgoing node tasked. When now_ns >= previous.handover_until_ns
            # the deadline has expired and we naturally drop the old
            # primary into stand_down on this very election.
            handover_from = previous.handover_from
            handover_until_ns = previous.handover_until_ns

        tasked: List[str] = []
        for nid in [primary, *backups, handover_from]:
            if nid and nid not in tasked:
                tasked.append(nid)

        # Stand-down = nodes previously kept tuned that are no longer
        # in the new tasked set. AutoTasker uses this to send an
        # explicit "release this freq" hint (item #6 on the roadmap;
        # for now we just log + expose via SSE so the operator sees it).
        prev_tasked = list(previous.tasked_nodes) if previous else []
        stand_down = sorted(set(prev_tasked) - set(tasked))

        reason = self._build_reason(track, primary, backups, handover_from,
                                     scores)

        decision = CustodyDecision(
            track_id=track.emitter_id,
            decided_ns=now_ns,
            primary=primary,
            backups=backups,
            tasked_nodes=tasked,
            stand_down=stand_down,
            handover_from=handover_from,
            handover_until_ns=handover_until_ns,
            # Trim to top-N for the explainability payload — sending
            # 50 node scores per assessment over SSE wastes bandwidth.
            scores=scores[:max(self.k_total * 2, 5)],
            reason=reason,
        )

        # Cache + change notification.
        self._last_decisions[track.emitter_id] = decision
        self.elections_total += 1
        if primary is None:
            self.elections_no_eligible_node += 1
        primary_changed = (prev_primary or "") != (primary or "")
        if primary_changed and self._on_change is not None:
            try:
                self._on_change(decision)
            except Exception:
                # Never let the SSE callback take down the elector.
                logger.exception("on_change callback raised — ignored")

        return decision

    def forget(self, track_id: str) -> None:
        """Drop the cached decision for `track_id`. Call when the track
        is archived so the cache doesn't grow without bound."""
        self._last_decisions.pop(track_id, None)

    def last_decision(self, track_id: str) -> Optional[CustodyDecision]:
        return self._last_decisions.get(track_id)

    def stats(self) -> dict:
        return {
            "k_total": self.k_total,
            "handover_overlap_s": self.handover_overlap_s,
            "weights": dict(self.weights),
            "elections_total": self.elections_total,
            "elections_with_handover": self.elections_with_handover,
            "elections_no_eligible_node": self.elections_no_eligible_node,
            "tracks_in_cache": len(self._last_decisions),
        }

    # ── Scoring ──────────────────────────────────────────────────────────

    def _score_node(self,
                    track: EmitterTrack,
                    node: SensorNodeTrust,
                    now_ns: int,
                    current_load: int) -> CustodyScore:
        """Score one node for one track. Side-effect-free."""
        s = CustodyScore(node_id=node.node_id)

        # ── Hard gates ───────────────────────────────────────────────────
        # A hard-gated node gets total=0 and a human-readable reason.
        # We still record the components so the explainability payload
        # shows what the node WOULD have scored otherwise.
        gate_reason = self._hard_gate(track, node, now_ns)

        # ── Soft components (always computed for explainability) ─────────
        s.components["snr"]      = self._snr_component(track, node)
        s.components["distance"] = self._distance_component(track, node)
        s.components["gps_age"]  = self._gps_age_component(node, now_ns)
        s.components["trust"]    = self._trust_component(node)
        s.components["load"]     = self._load_component(current_load)
        s.components["decoder"]  = self._decoder_component(track, node)

        if gate_reason:
            s.rejected_reason = gate_reason
            s.total = 0.0
            return s

        weighted = sum(self.weights.get(k, 0.0) * v
                       for k, v in s.components.items())

        # Multiplicative thermal penalty applied AFTER the weighted sum
        # so it acts as a uniform scaling factor across all components
        # rather than overwhelming any single one.
        if node.thermal_throttling_active:
            weighted *= 0.5

        # Clamp to [0, 1] for readability (rounding noise can push
        # weighted slightly above 1 if weights sum > 1).
        s.total = max(0.0, min(1.0, weighted))
        return s

    def _hard_gate(self, track: EmitterTrack, node: SensorNodeTrust,
                   now_ns: int) -> str:
        """Return a non-empty rejection reason if the node MUST NOT be
        elected. Empty string means the soft score applies."""

        # 1. TDOA-required threats need GPS-synced nodes. Without sync
        #    the node's timestamps are useless for TDOA, so it can't
        #    contribute to a fix and shouldn't waste a slot.
        if track.threat_level in ("high", "critical"):
            if not node.gps_synchronized:
                return "tdoa_threat_requires_gps_sync"

        # 2. Stale GPS — if the node's last fix was hours ago, its
        #    location is whatever the operator's last known position
        #    was, which may be far away. Don't task it for a new
        #    track that needs accurate geometry.
        if (track.threat_level in ("high", "critical")
                and node.location_gps_updated_ns > 0):
            gps_age_s = (now_ns - node.location_gps_updated_ns) / 1e9
            if gps_age_s > self.stale_gps_after_s:
                return f"gps_fix_stale_{int(gps_age_s)}s"

        # 3. Decoder gate. If the track has a known protocol AND the
        #    node has reported any available_decoders (meaning the
        #    capability probe ran successfully), then the node MUST
        #    have the decoder for that protocol. If available_decoders
        #    is empty we don't know the node's capabilities and can't
        #    fairly hard-gate.
        if track.protocol and node.available_decoders:
            wanted = track.protocol.lower()
            available = {d.lower() for d in node.available_decoders}
            if wanted not in available:
                return f"missing_decoder_{wanted}"

        return ""

    def _snr_component(self, track: EmitterTrack,
                       node: SensorNodeTrust) -> float:
        # We don't have per-node SNR readings on the track itself
        # (RFEvent has snr_db but it's not preserved on EmitterTrack).
        # Use detecting_nodes membership + node sensitivity as a proxy:
        #   - node has heard this track AND has good sensitivity → 1.0
        #   - node has heard this track but lower sensitivity         → 0.7
        #   - node has not heard this track but is sensitive          → 0.5
        #   - node has not heard this track, weak sensitivity         → 0.3
        heard = node.node_id in track.detecting_nodes
        # sensitivity_trust is 0.5..1.0; renormalize to 0..1
        sens = max(0.0, (node.sensitivity_trust - 0.5) * 2.0)
        if heard:
            return 0.7 + 0.3 * sens
        return 0.3 + 0.2 * sens

    def _distance_component(self, track: EmitterTrack,
                            node: SensorNodeTrust) -> float:
        if (track.estimated_lat is None or track.estimated_lon is None
                or node.location_gps is None):
            # No geometry available — neutral score so distance doesn't
            # accidentally penalise nodes during track bootstrap when
            # no fix exists yet.
            return 0.5
        dist_m = _haversine_m(track.estimated_lat, track.estimated_lon,
                               node.location_gps[0], node.location_gps[1])
        # Soft falloff: 0 m → 1.0, 10 km → 0.5, 50 km+ → ~0.1.
        # exp(-dist / 14_000) lands close to those targets without
        # cliffs that would cause flapping at the threshold.
        return float(math.exp(-dist_m / 14_000.0))

    def _gps_age_component(self, node: SensorNodeTrust,
                           now_ns: int) -> float:
        if node.location_gps_updated_ns <= 0:
            # Never had a fix — 0.3 instead of 0 because some hardware
            # legitimately runs without GPS (CoC workstation, fixed-
            # install indoor monitor) and we don't want to lock them
            # out of low-threat tasking entirely.
            return 0.3
        age_s = (now_ns - node.location_gps_updated_ns) / 1e9
        if age_s <= 0:
            return 1.0
        if age_s >= self.stale_gps_after_s:
            return 0.0
        return 1.0 - (age_s / self.stale_gps_after_s)

    def _trust_component(self, node: SensorNodeTrust) -> float:
        # compute_trust_score returns 0.05..0.98 by construction.
        return float(node.compute_trust_score())

    def _load_component(self, current_load: int) -> float:
        # 0 tracks → 1.0, 1 → 0.5, 2 → 0.33, 3 → 0.25, ... so the
        # election naturally spreads custody across the fleet rather
        # than piling everything onto the most-trusted node.
        return 1.0 / (1.0 + max(0, int(current_load)))

    def _decoder_component(self, track: EmitterTrack,
                           node: SensorNodeTrust) -> float:
        if not track.protocol:
            return 0.5  # neutral — we don't know what's needed
        if not node.available_decoders:
            return 0.5  # node hasn't reported capabilities yet
        wanted = track.protocol.lower()
        available = {d.lower() for d in node.available_decoders}
        return 1.0 if wanted in available else 0.0

    # ── Reason builder ───────────────────────────────────────────────────

    def _build_reason(self,
                      track: EmitterTrack,
                      primary: Optional[str],
                      backups: List[str],
                      handover_from: Optional[str],
                      scores: List[CustodyScore]) -> str:
        if primary is None:
            # Surface the top rejection reason if any so the operator
            # can see why no one was elected (e.g. all GPS stale).
            for s in scores:
                if s.rejected_reason:
                    return (f"no eligible node — top candidate {s.node_id} "
                            f"rejected: {s.rejected_reason}")
            return "no eligible nodes available"
        bits = [f"primary={primary}"]
        if backups:
            bits.append(f"backups={','.join(backups)}")
        if handover_from:
            bits.append(f"handover_from={handover_from}")
        bits.append(f"threat={track.threat_level}")
        return " ".join(bits)
