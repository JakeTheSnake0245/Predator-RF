"""End-to-end: an event arriving via CoC tagged with `upstream_source`
propagates through TrackManager.ingest() → EmitterTrack.upstream_source
so CrossStationDedup can key on it. This was the bug the architect
flagged: tracks were always treated as local because the field wasn't
plumbed through the ingest path."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.fusion.track_manager import TrackManager
from backend.fusion.cross_station_dedup import CrossStationDedup
from backend.models.rf_event import RFEvent
from backend.models.sensor_node import SensorNodeTrust


def _ev(node_id, freq, ts_ns, upstream=None):
    e = RFEvent(
        node_id=node_id, frequency=freq, power_dbfs=-50.0,
        snr_db=20.0, timestamp_ns=ts_ns, node_trust_score=0.8)
    e.upstream_source = upstream
    return e


class UpstreamPropagationTests(unittest.TestCase):
    def setUp(self):
        self.tm = TrackManager()
        self.tm.sensor_nodes["local-1"] = SensorNodeTrust(
            node_id="local-1", hardware_code="LO")
        self.tm.sensor_nodes["peer-1"] = SensorNodeTrust(
            node_id="peer-1", hardware_code="PR")

    def test_remote_only_event_stamps_upstream(self):
        t = self.tm.ingest(_ev("peer-1", 462.6e6, 1_000_000_000,
                                upstream="https://peer.example/coc"))
        self.assertEqual(t.upstream_source, "https://peer.example/coc")

    def test_local_event_leaves_upstream_none(self):
        t = self.tm.ingest(_ev("local-1", 462.6e6, 1_000_000_000))
        self.assertIsNone(t.upstream_source)

    def test_dedup_picks_up_propagated_field(self):
        # Same physical emitter heard locally AND by peer. The
        # associator may collapse them on ingest (same freq within its
        # tolerance) so we install two tracks directly — the dedup pass
        # is for the case where the per-cluster TrackManagers each
        # produced their own track and we're coalescing across them.
        import time as _time
        from backend.models.emitter_track import EmitterTrack
        now = _time.time_ns()
        local_t = EmitterTrack(
            primary_frequency=462.6e6, last_power_dbfs=-50.0,
            first_seen_ns=now, last_seen_ns=now, observation_count=5,
            estimated_lat=35.0, estimated_lon=-106.5,
            location_confidence=0.7, upstream_source=None)
        peer_t = EmitterTrack(
            primary_frequency=462.6e6, last_power_dbfs=-50.0,
            first_seen_ns=now, last_seen_ns=now, observation_count=5,
            estimated_lat=35.00001, estimated_lon=-106.50001,
            location_confidence=0.7,
            upstream_source="https://peer.example/coc")
        self.tm.tracks = {local_t.emitter_id: local_t,
                          peer_t.emitter_id: peer_t}

        dedup = CrossStationDedup(freq_tolerance_hz=5_000.0,
                                   location_tolerance_m=200.0)
        n_merged = dedup.run(self.tm)
        self.assertGreaterEqual(n_merged, 1,
            "dedup should have merged local + peer tracks now that "
            "upstream_source propagates through the EmitterTrack model")


if __name__ == "__main__":
    unittest.main(verbosity=2)
