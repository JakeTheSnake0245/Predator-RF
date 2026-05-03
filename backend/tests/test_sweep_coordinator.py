"""
SweepCoordinator tests — coordinated wideband sweep for LPI/LPD detection.

Verifies the spectrum-segmentation algorithm produces non-overlapping
assignments per phase, full-band coverage over a finite number of
phases, and respects per-SDR frequency limits.
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.coordination.sweep_coordinator import (
    SweepCoordinator, SweepPlan, Segment, Assignment)
from backend.models.sdr_backend import SDRBackend


class _Node:
    """Stand-in for SensorNodeTrust.all_sdr_backends() — only that
    method is read by the coordinator."""
    def __init__(self, node_id, sdr_backends):
        self.node_id = node_id
        self._backends = list(sdr_backends)

    def all_sdr_backends(self):
        return list(self._backends)


def _hf(name, bw_hz=20_000_000, lo=1e6, hi=6e9):
    return SDRBackend(backend_id=name, hardware_code="hackrf",
                      instantaneous_bandwidth_hz=bw_hz,
                      min_freq_hz=lo, max_freq_hz=hi)


def _rtl(name, bw_hz=2_400_000, lo=24e6, hi=1.7e9):
    return SDRBackend(backend_id=name, hardware_code="rtlsdr",
                      instantaneous_bandwidth_hz=bw_hz,
                      min_freq_hz=lo, max_freq_hz=hi)


class SweepCoordinatorTests(unittest.IsolatedAsyncioTestCase):
    def test_band_validation(self):
        with self.assertRaises(ValueError):
            SweepCoordinator(band_start_hz=1e9, band_end_hz=500e6)
        with self.assertRaises(ValueError):
            SweepCoordinator(band_start_hz=100e6, band_end_hz=200e6,
                             segment_bandwidth_hz=0)

    def test_segments_partition_the_band(self):
        sc = SweepCoordinator(band_start_hz=100e6, band_end_hz=300e6,
                              segment_bandwidth_hz=50e6)
        segs = sc.build_segments(50e6)
        self.assertEqual(len(segs), 4)
        self.assertEqual(segs[0].start_hz, 100e6)
        self.assertEqual(segs[-1].end_hz, 300e6)
        # No gaps, no overlap
        for a, b in zip(segs, segs[1:]):
            self.assertEqual(a.end_hz, b.start_hz)

    def test_eight_nodes_twenty_mhz_each_assigns_disjoint_segments(self):
        """8 HackRFs × 20 MHz over a 1900 MHz band — 95 segments,
        8 assignments per phase, all distinct."""
        nodes = [_Node(f"n{i}", [_hf(f"n{i}-A", 20_000_000)])
                 for i in range(8)]
        sc = SweepCoordinator(band_start_hz=100e6, band_end_hz=2_000e6,
                              segment_bandwidth_hz=20e6, dwell_seconds=1.0)
        plan = sc.plan_phase(nodes, phase_index=0)
        self.assertEqual(len(plan.assignments), 8)
        seg_keys = {(a.segment.start_hz, a.segment.end_hz)
                    for a in plan.assignments}
        self.assertEqual(len(seg_keys), 8,
            "all 8 nodes must look at distinct segments in one phase")
        node_ids = {a.node_id for a in plan.assignments}
        self.assertEqual(len(node_ids), 8)

    def test_phase_rotation_walks_the_gap_across_the_band(self):
        """Across phases, the set of covered segments shifts so no
        single segment is permanently in the gap. After enough phases,
        every segment must have been covered at least once."""
        nodes = [_Node(f"n{i}", [_hf(f"n{i}-A", 20_000_000)])
                 for i in range(8)]
        sc = SweepCoordinator(band_start_hz=100e6, band_end_hz=2_000e6,
                              segment_bandwidth_hz=20e6)
        n_segments = len(sc.build_segments(20e6))
        ever_covered = set()
        for k in range(n_segments):
            plan = sc.plan_phase(nodes, phase_index=k)
            for a in plan.assignments:
                ever_covered.add((a.segment.start_hz, a.segment.end_hz))
        self.assertEqual(len(ever_covered), n_segments,
            "after one full rotation every segment must have been covered")

    def test_revisit_and_gap_estimates(self):
        nodes = [_Node(f"n{i}", [_hf(f"n{i}-A", 20_000_000)])
                 for i in range(8)]
        sc = SweepCoordinator(band_start_hz=100e6, band_end_hz=2_000e6,
                              segment_bandwidth_hz=20e6, dwell_seconds=1.5)
        revisit = sc.estimate_revisit_time_s(nodes)
        # 95 segments / 8 slots = 12 phases × 1.5s = 18s
        self.assertAlmostEqual(revisit, 18.0, places=4)
        gap = sc.estimate_gap_fraction(nodes)
        # 8 covered of 95 = ~91.6% gap at any given phase
        self.assertAlmostEqual(gap, 1 - 8 / 95, places=4)

    def test_multi_sdr_node_gets_multiple_slots_per_phase(self):
        """A single workstation node with 3 SDRs contributes 3 slots,
        not 1 — covers 3x the spectrum."""
        ws = _Node("workstation", [
            _hf("hf-1"), _hf("hf-2"),
            _rtl("rtl-1"),
        ])
        peers = [_Node(f"p{i}", [_hf(f"p{i}-A")]) for i in range(2)]
        sc = SweepCoordinator(band_start_hz=100e6, band_end_hz=2_000e6,
                              segment_bandwidth_hz=20e6)
        plan = sc.plan_phase([ws] + peers, phase_index=0)
        ws_assignments = [a for a in plan.assignments
                          if a.node_id == "workstation"]
        self.assertEqual(len(ws_assignments), 3,
            "multi-SDR node must get one slot per backend")
        # Total slots = 3 (workstation) + 2 (peers) = 5 distinct segments
        self.assertEqual(len(plan.assignments), 5)

    def test_rtlsdr_skipped_for_out_of_range_segments(self):
        """An RTL-SDR can't tune above 1.7 GHz. When its rotated
        assignment falls in 1.7-2 GHz it should be reassigned to a
        feasible segment, not silently sent an impossible tune
        command."""
        rtl_node = _Node("rtl-only", [_rtl("rtl-A")])
        sc = SweepCoordinator(band_start_hz=1_500e6, band_end_hz=2_000e6,
                              segment_bandwidth_hz=2_400_000)
        # Find a phase where the natural slot would be > 1.7 GHz
        bad_phase = None
        for k in range(250):
            plan = sc.plan_phase([rtl_node], phase_index=k)
            if not plan.assignments:
                bad_phase = k
                break
            seg = plan.assignments[0].segment
            if seg.center_hz > 1.7e9:
                self.fail(f"phase {k}: rtl-only assigned an out-of-range "
                          f"segment center {seg.center_hz}")
        # Either every phase found a feasible slot OR some phases had none —
        # both outcomes are acceptable, what we forbid is a tune to >1.7 GHz.

    def test_empty_fleet_produces_empty_plan_with_uncovered_list(self):
        sc = SweepCoordinator(band_start_hz=100e6, band_end_hz=200e6,
                              segment_bandwidth_hz=20e6)
        plan = sc.plan_phase([], phase_index=0)
        self.assertEqual(plan.assignments, [])
        self.assertEqual(len(plan.uncovered_segments), 5)

    def test_no_double_booking_when_fallback_steals_a_segment(self):
        """Regression: the natural-path assignment must check
        covered_indices, not just the fallback path. Otherwise an
        earlier slot's fallback can claim a segment, and a later
        slot's natural rotation lands on the same segment — silent
        double-booking, silent under-coverage."""
        # Construct a forced collision: slot 0 is RTL-only and must
        # fall back away from a high-freq natural assignment, taking
        # a segment that slot 1's natural rotation also wants.
        rtl = _Node("rtl-station", [_rtl("rtl-A", lo=24e6, hi=1.7e9)])
        wide = _Node("hackrf-station", [_hf("hf-A", lo=1e6, hi=6e9)])
        # Band straddles the RTL ceiling so the rotation collides
        sc = SweepCoordinator(band_start_hz=1_500e6, band_end_hz=2_000e6,
                              segment_bandwidth_hz=100e6)
        for phase in range(20):
            plan = sc.plan_phase([rtl, wide], phase_index=phase)
            seg_keys = [(a.segment.start_hz, a.segment.end_hz)
                        for a in plan.assignments]
            self.assertEqual(len(seg_keys), len(set(seg_keys)),
                f"phase {phase}: double-booked segment "
                f"{seg_keys}")

    def test_legacy_single_sdr_node_keeps_hardware_freq_range(self):
        """Regression: the synthesised legacy SDRBackend must use the
        node's hardware_capabilities freq range, not the SDRBackend
        class defaults (which would clamp a 6-GHz HackRF down to
        1.7 GHz)."""
        # Use a real SensorNodeTrust so capability lookup runs
        from backend.models.sensor_node import SensorNodeTrust
        n = SensorNodeTrust(node_id="hf-node", hardware_code="hackrf")
        backends = n.all_sdr_backends()
        self.assertEqual(len(backends), 1)
        # HackRF reaches 6 GHz — must NOT be clamped to 1.7 GHz default
        self.assertGreater(backends[0].max_freq_hz, 5e9,
            "legacy HackRF synthesis was clamped to RTL-SDR range — "
            "SweepCoordinator would refuse to task it above 1.7 GHz")

    def test_advance_phase(self):
        sc = SweepCoordinator(band_start_hz=100e6, band_end_hz=200e6)
        self.assertEqual(sc.phase_index, 0)
        self.assertEqual(sc.advance_phase(), 1)
        self.assertEqual(sc.advance_phase(), 2)

    async def test_execute_phase_dispatches_scan_commands(self):
        """execute_phase calls send_scan_command on each assigned node's
        client."""
        calls: list = []

        class _FakeClient:
            async def send_scan_command(self, freq_start_hz, freq_end_hz,
                                         dwell_ms=500, start=True):
                calls.append((freq_start_hz, freq_end_hz, dwell_ms))
                return True

        class _FakeFleet:
            def __init__(self):
                self.client = _FakeClient()
            def get_client(self, node_id):
                return self.client

        nodes = [_Node(f"n{i}", [_hf(f"n{i}-A", 20_000_000)])
                 for i in range(3)]
        sc = SweepCoordinator(band_start_hz=100e6, band_end_hz=200e6,
                              segment_bandwidth_hz=20e6, dwell_seconds=0.5)
        plan = sc.plan_phase(nodes, phase_index=0)
        ok = await sc.execute_phase(_FakeFleet(), plan)
        self.assertEqual(ok, len(plan.assignments))
        self.assertEqual(len(calls), len(plan.assignments))
        for (start, end, dwell_ms) in calls:
            self.assertEqual(dwell_ms, 500)
            self.assertEqual(end - start, 20_000_000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
