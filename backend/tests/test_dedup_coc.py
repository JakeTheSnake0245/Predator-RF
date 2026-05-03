"""CrossStationDedup: only merges when origins differ; respects freq /
location / time tolerances; never merges purely-local pairs."""
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.fusion.cross_station_dedup import CrossStationDedup


class _Track:
    def __init__(self, emitter_id, freq=462e6, lat=None, lon=None,
                 modulation=None, protocol=None, upstream=None,
                 first_seen_offset=0, last_seen_offset=0):
        self.emitter_id = emitter_id
        self.primary_frequency = freq
        self.estimated_lat = lat
        self.estimated_lon = lon
        self.location_confidence = 0.5 if lat else 0.0
        self.modulation = modulation
        self.protocol = protocol
        self.upstream_source = upstream
        now = time.time_ns()
        self.first_seen_ns = now + first_seen_offset
        self.last_seen_ns = now + last_seen_offset
        self.detecting_nodes = []
        self.confidence = 0.5
        self.observation_count = 1


class _TM:
    def __init__(self, tracks):
        self.tracks = {t.emitter_id: t for t in tracks}


class CrossStationDedupTests(unittest.TestCase):
    def test_merges_local_and_remote_at_same_location(self):
        local = _Track("local", lat=35.0, lon=-106.0, upstream=None)
        remote = _Track("remote", lat=35.001, lon=-106.001,
                        upstream="http://field-bravo")
        tm = _TM([local, remote])
        d = CrossStationDedup()
        n = d.run(tm)
        self.assertEqual(n, 1)
        self.assertEqual(set(tm.tracks.keys()), {"local"})

    def test_does_not_merge_two_local_tracks(self):
        a = _Track("a", lat=35.0, lon=-106.0)
        b = _Track("b", lat=35.0, lon=-106.0)
        tm = _TM([a, b])
        self.assertEqual(CrossStationDedup().run(tm), 0)

    def test_does_not_merge_when_freq_differs(self):
        a = _Track("a", freq=462e6, lat=35.0, lon=-106.0)
        b = _Track("b", freq=465e6, lat=35.0, lon=-106.0,
                   upstream="peer")
        tm = _TM([a, b])
        self.assertEqual(CrossStationDedup().run(tm), 0)

    def test_does_not_merge_when_locations_far_apart(self):
        a = _Track("a", lat=35.0, lon=-106.0)
        b = _Track("b", lat=36.0, lon=-106.0, upstream="peer")
        tm = _TM([a, b])
        self.assertEqual(CrossStationDedup().run(tm), 0)

    def test_falls_back_to_modulation_match_when_one_has_no_location(self):
        a = _Track("a", lat=35.0, lon=-106.0, modulation="P25")
        b = _Track("b", lat=None, lon=None, modulation="P25",
                   upstream="peer")
        tm = _TM([a, b])
        self.assertEqual(CrossStationDedup().run(tm), 1)

    def test_does_not_merge_when_modulations_disagree(self):
        a = _Track("a", lat=None, lon=None, modulation="DMR")
        b = _Track("b", lat=None, lon=None, modulation="P25",
                   upstream="peer")
        tm = _TM([a, b])
        self.assertEqual(CrossStationDedup().run(tm), 0)

    def test_older_track_wins(self):
        old = _Track("old", lat=35.0, lon=-106.0,
                     first_seen_offset=-60_000_000_000)
        new = _Track("new", lat=35.0001, lon=-106.0,
                     upstream="peer", first_seen_offset=0)
        tm = _TM([old, new])
        CrossStationDedup().run(tm)
        self.assertIn("old", tm.tracks)
        self.assertNotIn("new", tm.tracks)

    def test_merges_total_counter(self):
        d = CrossStationDedup()
        for _ in range(3):
            tm = _TM([_Track("a", lat=35.0, lon=-106.0),
                       _Track("b", lat=35.0, lon=-106.0,
                              upstream="peer")])
            d.run(tm)
        self.assertEqual(d.merges_total, 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
