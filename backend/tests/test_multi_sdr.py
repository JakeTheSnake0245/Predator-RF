"""
Multi-SDR node-model tests.

Backwards compat: nodes with no explicit `sdr_backends` list still
work via a synthesised single backend.
Forward path: nodes with multiple SDRs report combined bandwidth and
let the SweepCoordinator allocate proportionally more spectrum.
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))


class _MinimalNode:
    """Hand-rolled stand-in for SensorNodeTrust that doesn't require
    importing backend.sensor (numpy chain). Mirrors only the fields
    SDRBackend / SensorNodeTrust helpers actually read."""
    def __init__(self, node_id, sdr_backends=None,
                 hardware_code="hackrf", max_sample_rate_hz=20_000_000):
        self.node_id = node_id
        self.sdr_backends = list(sdr_backends or [])
        self.hardware_code = hardware_code
        self.hardware_serial = ""
        self.max_sample_rate_hz = max_sample_rate_hz
        self.timing_stability_trust = 0.9
        self.sensitivity_trust = 0.9
        self.frequency_stability_trust = 0.9


# Borrow only the helpers — implementing them inline here to avoid
# triggering the backend.sensor → numpy import chain via SensorNodeTrust.
from backend.models.sdr_backend import SDRBackend


def _all_sdr_backends(node):
    if node.sdr_backends:
        return list(node.sdr_backends)
    return [SDRBackend(
        backend_id=node.node_id + ":default",
        hardware_code=node.hardware_code,
        max_sample_rate_hz=node.max_sample_rate_hz,
        instantaneous_bandwidth_hz=node.max_sample_rate_hz,
    )]


def _total_bandwidth(node):
    return sum(s.instantaneous_bandwidth_hz for s in _all_sdr_backends(node))


class MultiSDRTests(unittest.TestCase):
    def test_legacy_single_sdr_node_synthesises_default_backend(self):
        n = _MinimalNode("n1", hardware_code="hackrf",
                         max_sample_rate_hz=20_000_000)
        backends = _all_sdr_backends(n)
        self.assertEqual(len(backends), 1)
        self.assertEqual(backends[0].hardware_code, "hackrf")
        self.assertEqual(backends[0].instantaneous_bandwidth_hz, 20_000_000)

    def test_explicit_multi_sdr_list_returned_as_is(self):
        n = _MinimalNode("n2", sdr_backends=[
            SDRBackend(backend_id="hf-A", hardware_code="hackrf",
                       instantaneous_bandwidth_hz=20_000_000),
            SDRBackend(backend_id="rtl-B", hardware_code="rtlsdr",
                       instantaneous_bandwidth_hz=2_400_000,
                       min_freq_hz=24e6, max_freq_hz=1.7e9),
        ])
        backends = _all_sdr_backends(n)
        self.assertEqual(len(backends), 2)
        self.assertEqual(_total_bandwidth(n), 22_400_000)

    def test_sdr_frequency_coverage_check(self):
        rtl = SDRBackend(backend_id="rtl", hardware_code="rtlsdr",
                          min_freq_hz=24e6, max_freq_hz=1.7e9)
        hf = SDRBackend(backend_id="hf", hardware_code="hackrf",
                         min_freq_hz=1e6, max_freq_hz=6e9)
        self.assertTrue(rtl.covers(462e6))
        self.assertFalse(rtl.covers(2.4e9), "rtlsdr can't reach 2.4 GHz")
        self.assertTrue(hf.covers(2.4e9))
        self.assertTrue(hf.covers(5.8e9))

    def test_to_dict_round_trips_key_fields(self):
        s = SDRBackend(backend_id="hf-A", hardware_code="hackrf",
                       hardware_serial="123abc",
                       max_sample_rate_hz=20_000_000,
                       instantaneous_bandwidth_hz=20_000_000,
                       min_freq_hz=1e6, max_freq_hz=6e9,
                       current_center_freq_hz=433.92e6,
                       in_use=True)
        d = s.to_dict()
        self.assertEqual(d["backend_id"], "hf-A")
        self.assertEqual(d["hardware_code"], "hackrf")
        self.assertEqual(d["instantaneous_bandwidth_hz"], 20_000_000)
        self.assertTrue(d["in_use"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
