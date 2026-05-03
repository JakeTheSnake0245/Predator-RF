"""
SweepCoordinator — coordinated wideband sweep across the fleet.

Problem
-------
Operator wants to find LPI/LPD (Low Probability of Intercept / Detection)
emitters in a wide band — say, 100 MHz to 2 GHz. Each node can only
listen to a small slice at a time (HackRF: ~20 MHz instantaneous,
RTL-SDR: ~2.4 MHz). If every node hops randomly, you get a lot of
overlap (waste) and a lot of gaps that never get sampled fast enough
to catch a 50 ms LPI burst.

Strategy
--------
1. Divide the search band into N non-overlapping segments of width
   `segment_bandwidth_hz` (defaults to the smallest node's bandwidth
   so every node can cover any segment).
2. In phase k, assign each node a segment such that no two nodes look
   at the same segment in the same phase (maximises instantaneous
   coverage across the band).
3. Rotate the assignment every `dwell_seconds` so the gap between
   "covered" segments walks across the band — an LPI emitter sitting
   in a fixed frequency hole can only hide for one full rotation
   period before some node visits its slice.
4. With `M` nodes and `N` segments, full-band revisit time is
   `ceil(N / M) * dwell_seconds`. With 8 nodes × 20 MHz over
   1900 MHz: 95 segments / 8 nodes = 12 phases, ~12*dwell seconds
   to revisit any frequency.

Multi-SDR awareness
-------------------
A node with multiple SDRs (e.g. a workstation with HackRF + RTL-SDR
both plugged in) gets multiple slots per phase, one per `SDRBackend`
in `node.all_sdr_backends()`. This proportionally improves coverage.

The coordinator does NOT actually send commands here — it produces a
SweepPlan and the orchestrator dispatches via the fleet manager's
send_scan_command. This keeps the math testable on hosts without
network access (this Repl included).
"""
from __future__ import annotations

import logging
import math
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

logger = logging.getLogger(__name__)


@dataclass
class Segment:
    """A contiguous slice of spectrum the fleet will cover."""
    start_hz: float
    end_hz: float

    @property
    def width_hz(self) -> float:
        return self.end_hz - self.start_hz

    @property
    def center_hz(self) -> float:
        return (self.start_hz + self.end_hz) / 2.0


@dataclass
class Assignment:
    """One node-SDR's tasking for one phase."""
    node_id: str
    backend_id: str
    segment: Segment


@dataclass
class SweepPlan:
    """Output of plan_phase(). What every SDR in the fleet should
    listen to during this phase."""
    phase_index: int
    dwell_seconds: float
    assignments: List[Assignment] = field(default_factory=list)
    uncovered_segments: List[Segment] = field(default_factory=list)

    def coverage_fraction(self, total_segments: int) -> float:
        if total_segments <= 0:
            return 0.0
        return len(self.assignments) / total_segments


class SweepCoordinator:
    """
    Compute and (optionally) execute coordinated sweeps across the fleet.

    Stateless aside from `phase_index` and the chosen plan parameters.
    Re-run plan_phase() each tick of the external scheduler.
    """

    def __init__(self,
                 band_start_hz: float,
                 band_end_hz: float,
                 segment_bandwidth_hz: Optional[float] = None,
                 dwell_seconds: float = 1.0):
        if band_end_hz <= band_start_hz:
            raise ValueError("band_end_hz must be > band_start_hz")
        if segment_bandwidth_hz is not None and segment_bandwidth_hz <= 0:
            raise ValueError("segment_bandwidth_hz must be positive")
        self.band_start_hz = float(band_start_hz)
        self.band_end_hz = float(band_end_hz)
        self.segment_bandwidth_hz = segment_bandwidth_hz
        self.dwell_seconds = float(dwell_seconds)
        self.phase_index = 0

    def _resolve_segment_bandwidth(self, nodes: List) -> float:
        """If the operator didn't pin a segment width, default to the
        smallest SDR's instantaneous bandwidth so every SDR can be
        tasked any segment."""
        if self.segment_bandwidth_hz is not None:
            return self.segment_bandwidth_hz
        widths = []
        for n in nodes:
            for s in n.all_sdr_backends():
                if s.instantaneous_bandwidth_hz > 0:
                    widths.append(s.instantaneous_bandwidth_hz)
        if not widths:
            # No SDR data → 1 MHz default. The caller will get an empty
            # plan but won't crash.
            return 1_000_000.0
        return float(min(widths))

    def build_segments(self, segment_bandwidth_hz: float) -> List[Segment]:
        """Divide the search band into non-overlapping segments."""
        n = int(math.ceil(
            (self.band_end_hz - self.band_start_hz) / segment_bandwidth_hz))
        out = []
        for i in range(n):
            s = self.band_start_hz + i * segment_bandwidth_hz
            e = min(s + segment_bandwidth_hz, self.band_end_hz)
            out.append(Segment(start_hz=s, end_hz=e))
        return out

    def plan_phase(self, nodes: List,
                   phase_index: Optional[int] = None) -> SweepPlan:
        """Compute the assignments for one phase of the sweep.

        Algorithm: build segments; flatten the fleet into (node, sdr)
        slots respecting per-SDR frequency coverage; rotate the
        slot→segment mapping by `phase_index` so the gap walks across
        the band over time; emit one Assignment per slot that has a
        frequency-feasible segment.
        """
        if phase_index is None:
            phase_index = self.phase_index
        seg_bw = self._resolve_segment_bandwidth(nodes)
        segments = self.build_segments(seg_bw)
        if not segments:
            return SweepPlan(phase_index=phase_index,
                             dwell_seconds=self.dwell_seconds)

        # Flatten fleet into (node_id, sdr) slots. A node with 2 SDRs
        # contributes 2 slots → covers 2x the spectrum per phase.
        slots: List[Tuple[str, "SDRBackend"]] = []
        for n in nodes:
            for sdr in n.all_sdr_backends():
                slots.append((n.node_id, sdr))
        if not slots:
            return SweepPlan(phase_index=phase_index,
                             dwell_seconds=self.dwell_seconds,
                             uncovered_segments=segments)

        plan = SweepPlan(phase_index=phase_index,
                         dwell_seconds=self.dwell_seconds)
        covered_indices: set = set()
        # Rotation: in phase k, slot i looks at segment (i + k) mod N
        offset = phase_index % len(segments)
        for i, (node_id, sdr) in enumerate(slots):
            if i >= len(segments):
                break  # more SDRs than segments → fleet overcovers; skip
            seg_idx = (i + offset) % len(segments)
            seg = segments[seg_idx]
            # Two reasons to walk a fallback: (a) this SDR can't tune
            # the natural segment (e.g. RTL-SDR rotated above 1.7 GHz);
            # (b) an earlier slot's fallback already claimed our natural
            # segment, so taking it would double-book and silently
            # under-cover the band. Both must trigger the search.
            needs_fallback = (
                seg_idx in covered_indices or not sdr.covers(seg.center_hz))
            if needs_fallback:
                fallback = None
                for j in range(len(segments)):
                    candidate_idx = (seg_idx + j) % len(segments)
                    if candidate_idx in covered_indices:
                        continue
                    candidate = segments[candidate_idx]
                    if sdr.covers(candidate.center_hz):
                        fallback = (candidate_idx, candidate)
                        break
                if fallback is None:
                    continue
                seg_idx, seg = fallback
            covered_indices.add(seg_idx)
            plan.assignments.append(Assignment(
                node_id=node_id,
                backend_id=sdr.backend_id or f"{node_id}:default",
                segment=seg,
            ))

        plan.uncovered_segments = [
            s for i, s in enumerate(segments) if i not in covered_indices]
        return plan

    def advance_phase(self) -> int:
        """Bump phase_index — call between plan/dispatch cycles."""
        self.phase_index += 1
        return self.phase_index

    def estimate_revisit_time_s(self, nodes: List) -> float:
        """How long until every frequency in the search band has been
        visited at least once."""
        seg_bw = self._resolve_segment_bandwidth(nodes)
        n_segments = len(self.build_segments(seg_bw))
        n_slots = sum(len(n.all_sdr_backends()) for n in nodes)
        if n_slots == 0:
            return float("inf")
        n_phases = math.ceil(n_segments / n_slots)
        return n_phases * self.dwell_seconds

    def estimate_gap_fraction(self, nodes: List) -> float:
        """Instantaneous fraction of the search band that's uncovered
        in any single phase. 0.0 = perfect (n_slots ≥ n_segments),
        approaches 1.0 as the fleet shrinks relative to the band."""
        seg_bw = self._resolve_segment_bandwidth(nodes)
        n_segments = len(self.build_segments(seg_bw))
        n_slots = sum(len(n.all_sdr_backends()) for n in nodes)
        if n_segments == 0:
            return 0.0
        covered = min(n_slots, n_segments)
        return max(0.0, 1.0 - covered / n_segments)

    async def execute_phase(self, fleet, plan: SweepPlan) -> int:
        """Dispatch a SweepPlan to the fleet via send_scan_command. The
        fleet object only needs `get_client(node_id)` returning a client
        with `async send_scan_command(start, end, dwell_ms, start=True)`.

        Returns the number of successful dispatches."""
        ok = 0
        dwell_ms = int(self.dwell_seconds * 1000)
        for a in plan.assignments:
            client = fleet.get_client(a.node_id)
            if client is None:
                logger.warning("SweepCoordinator: no client for node %s",
                               a.node_id)
                continue
            try:
                result = await client.send_scan_command(
                    a.segment.start_hz, a.segment.end_hz,
                    dwell_ms=dwell_ms, start=True)
                if result:
                    ok += 1
            except Exception as exc:
                logger.warning("SweepCoordinator: scan dispatch to %s "
                               "failed: %s", a.node_id, exc)
        logger.info("SweepCoordinator phase %d: %d/%d assignments dispatched, "
                    "%d segment(s) uncovered this phase",
                    plan.phase_index, ok, len(plan.assignments),
                    len(plan.uncovered_segments))
        return ok
