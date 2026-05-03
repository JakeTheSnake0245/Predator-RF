"""OverrideRegistry: friendly emitters, freq blacklist, manual
location overrides. No store needed."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.operator.overrides import OverrideRegistry


class FriendlyTests(unittest.IsolatedAsyncioTestCase):
    async def test_add_then_check(self):
        r = OverrideRegistry()
        await r.add_friendly("em-1", label="our gmrs")
        self.assertTrue(r.is_friendly("em-1"))
        self.assertFalse(r.is_friendly("em-2"))

    async def test_remove(self):
        r = OverrideRegistry()
        await r.add_friendly("em-1", "x")
        self.assertTrue(await r.remove_friendly("em-1"))
        self.assertFalse(r.is_friendly("em-1"))
        self.assertFalse(await r.remove_friendly("em-1"))


class BlacklistTests(unittest.IsolatedAsyncioTestCase):
    async def test_in_range_is_blacklisted(self):
        r = OverrideRegistry()
        await r.add_blacklist(462e6, 468e6, reason="GMRS noise")
        self.assertTrue(r.is_blacklisted(462e6))
        self.assertTrue(r.is_blacklisted(465e6))
        self.assertTrue(r.is_blacklisted(468e6))
        self.assertFalse(r.is_blacklisted(150e6))

    async def test_swap_start_end_does_not_break_check(self):
        r = OverrideRegistry()
        await r.add_blacklist(end_hz=462e6, start_hz=468e6)
        self.assertTrue(r.is_blacklisted(465e6))


class ManualLocationTests(unittest.IsolatedAsyncioTestCase):
    async def test_set_then_get(self):
        r = OverrideRegistry()
        await r.set_manual_location("em-1", 35.1, -106.5,
                                    confidence=0.95, source="df_gear")
        ml = r.get_manual_location("em-1")
        self.assertIsNotNone(ml)
        self.assertEqual(ml.lat, 35.1)
        self.assertEqual(ml.source, "df_gear")

    async def test_apply_to_track_overrides_estimate(self):
        r = OverrideRegistry()
        await r.set_manual_location("em-1", 35.1, -106.5)
        track = {"emitter_id": "em-1", "estimated_lat": 0.0,
                 "estimated_lon": 0.0, "location_confidence": 0.1,
                 "threat_level": "high"}
        out = r.apply_to_track(track)
        self.assertEqual(out["estimated_lat"], 35.1)
        self.assertEqual(out["location_confidence"], 0.95)
        self.assertEqual(out["location_source"], "operator")

    async def test_apply_to_track_marks_friendly(self):
        r = OverrideRegistry()
        await r.add_friendly("em-1", label="ourBeacon")
        out = r.apply_to_track({"emitter_id": "em-1",
                                  "threat_level": "high"})
        self.assertEqual(out["threat_level"], "friendly")
        self.assertEqual(out["operator_label"], "ourBeacon")


if __name__ == "__main__":
    unittest.main(verbosity=2)
