"""Preflight check primitives. Each check is exercised in isolation
so a regression in one (e.g. the disk-free math) doesn't get
masked by another check happening to fail at the same time."""
from __future__ import annotations

import asyncio
import os
import socket
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from deploy.preflight import (
    check_data_dir_writable, check_db_schema, check_disk_space,
    check_port_free, check_token, check_tx_posture, check_fleet,
    run_all,
)


class CheckPrimitives(unittest.TestCase):
    def test_disk_space_pass_when_plenty(self):
        with tempfile.TemporaryDirectory() as d:
            r = check_disk_space(d, min_mb=1)
            self.assertEqual(r["severity"], "PASS")

    def test_disk_space_fail_with_huge_threshold(self):
        with tempfile.TemporaryDirectory() as d:
            r = check_disk_space(d, min_mb=10**9)  # 1 PB
            self.assertEqual(r["severity"], "FAIL")

    def test_data_dir_writable_pass(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(check_data_dir_writable(d)["severity"], "PASS")

    def test_db_schema_fresh_db_passes(self):
        with tempfile.TemporaryDirectory() as d:
            r = check_db_schema(os.path.join(d, "fresh.db"))
            self.assertEqual(r["severity"], "PASS")

    def test_token_unset_lab_warns_not_fails(self):
        self.assertEqual(check_token("", allow_lab=True)["severity"], "WARN")

    def test_token_unset_field_fails(self):
        self.assertEqual(check_token("", allow_lab=False)["severity"], "FAIL")

    def test_token_short_warns(self):
        self.assertEqual(check_token("short", allow_lab=False)["severity"],
                          "WARN")

    def test_token_long_passes(self):
        self.assertEqual(check_token("a" * 32, allow_lab=False)["severity"],
                          "PASS")

    def test_port_free_passes_on_random_port(self):
        # Find a known-free port the OS just gave us, close it, probe it.
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        r = check_port_free("127.0.0.1", port)
        self.assertEqual(r["severity"], "PASS")

    def test_port_free_fails_when_taken(self):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.listen(1)
        try:
            r = check_port_free("127.0.0.1", port)
            self.assertEqual(r["severity"], "FAIL")
        finally:
            s.close()

    def test_tx_posture_rx_only_passes(self):
        r = check_tx_posture(False, False, False)
        self.assertEqual(r["severity"], "PASS")
        self.assertIn("RX-only", r["message"])

    def test_tx_posture_cot_without_approval_warns(self):
        r = check_tx_posture(True, False, False)
        self.assertEqual(r["severity"], "WARN")

    def test_tx_posture_cot_with_approval_passes(self):
        r = check_tx_posture(True, False, True)
        self.assertEqual(r["severity"], "PASS")


class FleetCheckTests(unittest.IsolatedAsyncioTestCase):
    async def test_empty_fleet_warns(self):
        r = await check_fleet("")
        self.assertEqual(r["severity"], "WARN")

    async def test_unparseable_node_fails(self):
        r = await check_fleet("bogus-no-at-sign")
        self.assertEqual(r["severity"], "FAIL")

    async def test_unreachable_node_fails(self):
        # Pick an unused-by-design port.
        r = await check_fleet("alpha@127.0.0.1:1", http_timeout_s=0.5)
        self.assertEqual(r["severity"], "FAIL")

    async def test_reachable_node_passes(self):
        # Spin up a throwaway listener; preflight just needs the
        # TCP handshake to succeed.
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        port = s.getsockname()[1]
        try:
            r = await check_fleet(f"alpha@127.0.0.1:{port}",
                                   http_timeout_s=1.0)
            self.assertEqual(r["severity"], "PASS")
        finally:
            s.close()


class RunAllTests(unittest.IsolatedAsyncioTestCase):
    async def test_run_all_returns_summary(self):
        # Don't assert GO/NO-GO — depends on the env the test runs in.
        report = await run_all(allow_lab=True)
        self.assertIn("results", report)
        self.assertIn("summary", report)
        self.assertIn("go", report)
        self.assertEqual(set(report["summary"].keys()),
                          {"PASS", "WARN", "FAIL"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
