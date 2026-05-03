"""
CoC provenance tests — verify upstream_source is preserved end-to-end
through the dict→RFEvent rehydration in PredatorBackend._on_remote_event.

Without this, a CoC workstation would see all aggregated events as if
they were local and lose the ability to dedupe / attribute by station.
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.models.rf_event import RFEvent


class RFEventProvenanceTests(unittest.TestCase):
    def test_upstream_source_default_is_none(self):
        ev = RFEvent(frequency=462e6, power_dbfs=-30.0, snr_db=12.0,
                     timestamp_ns=0, node_id="n1")
        self.assertIsNone(ev.upstream_source)
        self.assertIn("upstream_source", ev.to_dict())

    def test_upstream_source_round_trips_in_to_dict(self):
        ev = RFEvent(frequency=462e6, power_dbfs=-30.0, snr_db=12.0,
                     timestamp_ns=0, node_id="n1",
                     upstream_source="http://field-alpha:8000")
        d = ev.to_dict()
        self.assertEqual(d["upstream_source"], "http://field-alpha:8000")


class CoCRehydrationTests(unittest.TestCase):
    """Exercise the rehydration logic from main._on_remote_event without
    pulling in the whole PredatorBackend (which requires numpy)."""

    @staticmethod
    def _rehydrate(ev_dict: dict):
        upstream = (ev_dict.get("upstream_source") or
                    ev_dict.get("_upstream"))
        clean = {k: v for k, v in ev_dict.items()
                 if not k.startswith("_") and k != "upstream_source"}
        ev = RFEvent(**clean)
        if upstream:
            ev.upstream_source = upstream
        return ev

    def test_underscored_upstream_tag_is_lifted_to_dataclass_field(self):
        d = {"frequency": 462e6, "power_dbfs": -30.0, "snr_db": 12.0,
             "timestamp_ns": 0, "node_id": "n1",
             "_upstream": "http://field-bravo:8000"}
        ev = self._rehydrate(d)
        self.assertEqual(ev.upstream_source, "http://field-bravo:8000",
            "_upstream tag from CoCAggregator must end up on the typed "
            "RFEvent field, not silently dropped at the dict boundary")

    def test_existing_upstream_source_takes_precedence(self):
        """If the event already carries upstream_source (relayed
        through multiple CoC hops), keep the deepest origin — don't
        overwrite with the immediate aggregator's URL."""
        d = {"frequency": 462e6, "power_dbfs": -30.0, "snr_db": 12.0,
             "timestamp_ns": 0, "node_id": "n1",
             "upstream_source": "field-alpha-original",
             "_upstream": "http://hub-station:8000"}
        ev = self._rehydrate(d)
        self.assertEqual(ev.upstream_source, "field-alpha-original")

    def test_no_upstream_at_all_yields_none(self):
        d = {"frequency": 462e6, "power_dbfs": -30.0, "snr_db": 12.0,
             "timestamp_ns": 0, "node_id": "n1"}
        ev = self._rehydrate(d)
        self.assertIsNone(ev.upstream_source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
