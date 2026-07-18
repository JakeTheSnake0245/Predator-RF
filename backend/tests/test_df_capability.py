"""
Fleet DF capability summary tests — backend/fusion/df_capability.py.

Covers the non-Kraken fleet hardening: LOB detection, TDOA viability
gating (hardware-timed vs system-clock), RSSI-only fallback labeling,
and offline/GPS-stale node exclusion.
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.fusion.df_capability import compute_df_capability
from backend.fusion.tdoa_coordinator import SYSTEM_CLOCK_MIN_DISTINCT


class _Node:
    """Minimal SensorNodeTrust shim."""
    def __init__(self, node_id, hardware_code="rtlsdr", can_do_tdoa=False,
                 is_online=True, location_gps=(35.1, -106.5),
                 location_gps_updated_ns=0, available_detectors=None):
        self.node_id = node_id
        self.hardware_code = hardware_code
        self.can_do_tdoa = can_do_tdoa
        self.is_online = is_online
        self.location_gps = location_gps
        self.location_gps_updated_ns = location_gps_updated_ns
        self.available_detectors = available_detectors or []


class DFCapabilityTests(unittest.TestCase):

    def test_empty_fleet_is_none(self):
        d = compute_df_capability([])
        self.assertEqual(d["df_mode"], "none")
        self.assertIn("No sensor nodes online", d["warning"])

    def test_kraken_hardware_is_lob(self):
        d = compute_df_capability([
            _Node("k1", hardware_code="krakensdr"),
            _Node("r1", hardware_code="rtlsdr"),
        ])
        self.assertEqual(d["df_mode"], "lob")
        self.assertEqual(d["lob_capable_count"], 1)
        self.assertEqual(d["lob_capable_nodes"], ["k1"])

    def test_kraken_lob_detector_is_lob(self):
        d = compute_df_capability([
            _Node("n1", hardware_code="rtlsdr",
                  available_detectors=["KRAKEN_LOB"]),
        ])
        self.assertEqual(d["df_mode"], "lob")

    def test_offline_kraken_does_not_count(self):
        d = compute_df_capability([
            _Node("k1", hardware_code="krakensdr", is_online=False),
            _Node("r1"), _Node("r2"),
        ])
        self.assertNotEqual(d["df_mode"], "lob")
        self.assertEqual(d["lob_capable_count"], 0)
        self.assertEqual(d["total_node_count"], 3)
        self.assertEqual(d["online_node_count"], 2)

    def test_two_hackrf_hardware_timed_is_tdoa(self):
        d = compute_df_capability([
            _Node("h1", hardware_code="hackrf", can_do_tdoa=True),
            _Node("h2", hardware_code="hackrf", can_do_tdoa=True),
        ])
        self.assertEqual(d["df_mode"], "tdoa")
        self.assertTrue(d["tdoa_viable"])
        self.assertEqual(d["hardware_timed_count"], 2)
        self.assertIn("No LOB-capable node online", d["warning"])

    def test_two_rtl_system_clock_is_rssi_only(self):
        """2 system-clock nodes < SYSTEM_CLOCK_MIN_DISTINCT → no TDOA."""
        d = compute_df_capability([_Node("r1"), _Node("r2")])
        self.assertEqual(d["df_mode"], "rssi_only")
        self.assertTrue(d["rssi_only"])
        self.assertFalse(d["tdoa_viable"])
        self.assertIn("No DF hardware", d["warning"])

    def test_three_rtl_system_clock_is_tdoa_with_warning(self):
        nodes = [_Node(f"r{i}") for i in range(SYSTEM_CLOCK_MIN_DISTINCT)]
        d = compute_df_capability(nodes)
        self.assertEqual(d["df_mode"], "tdoa")
        self.assertTrue(d["tdoa_viable"])
        self.assertEqual(d["hardware_timed_count"], 0)
        self.assertEqual(d["system_clock_count"], SYSTEM_CLOCK_MIN_DISTINCT)
        self.assertIn("system-clock", d["warning"])

    def test_one_hw_timed_plus_one_sysclock_is_tdoa(self):
        d = compute_df_capability([
            _Node("h1", hardware_code="hackrf", can_do_tdoa=True),
            _Node("r1"),
        ])
        self.assertTrue(d["tdoa_viable"])
        self.assertEqual(d["tdoa_viable_count"], 2)

    def test_no_gps_excluded_from_tdoa(self):
        d = compute_df_capability([
            _Node("h1", hardware_code="hackrf", can_do_tdoa=True,
                  location_gps=None),
            _Node("h2", hardware_code="hackrf", can_do_tdoa=True),
        ])
        self.assertFalse(d["tdoa_viable"])
        self.assertEqual(d["hardware_timed_count"], 1)
        self.assertEqual(d["df_mode"], "rssi_only")

    def test_stale_gps_excluded_from_tdoa(self):
        now = time.time_ns()
        stale = now - int(120 * 1e9)
        d = compute_df_capability([
            _Node("h1", hardware_code="hackrf", can_do_tdoa=True,
                  location_gps_updated_ns=stale),
            _Node("h2", hardware_code="hackrf", can_do_tdoa=True,
                  location_gps_updated_ns=now),
        ], gps_max_age_s=60.0, now_ns=now)
        self.assertEqual(d["hardware_timed_count"], 1)
        self.assertFalse(d["tdoa_viable"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
