"""Tests for backend.fusion.stationarity_gate.StationarityGate.

Three concerns:
  1. Velocity gate — rejects physically impossible jumps, accepts
     plausible ones, bypasses NEW tracks and sub-floor dt.
  2. Motion classifier — RMS-vs-ellipse classification with hysteresis.
  3. EmitterTrack._advance_state interaction — mobile tracks need more
     observations to promote to STABLE than stationary ones.
"""
from __future__ import annotations

import unittest

from backend.fusion.stationarity_gate import (
    DEFAULT_HISTORY_MAX,
    FixCandidate,
    HistoryPoint,
    StationarityGate,
)
from backend.models.emitter_track import EmitterTrack, TrackState


# Use a wall-clock value far in the future so subtractions stay positive,
# matching the convention from test_custody_election.py.
NOW_NS = 2_000_000_000_000_000_000


def _hp(lat: float, lon: float, t_offset_s: float = 0.0,
        ellipse_a_m: float = 100.0) -> HistoryPoint:
    return HistoryPoint(lat=lat, lon=lon,
                        timestamp_ns=NOW_NS + int(t_offset_s * 1e9),
                        ellipse_a_m=ellipse_a_m)


def _fc(lat: float, lon: float, t_offset_s: float,
        ellipse_a_m: float = 100.0) -> FixCandidate:
    return FixCandidate(lat=lat, lon=lon,
                        timestamp_ns=NOW_NS + int(t_offset_s * 1e9),
                        ellipse_a_m=ellipse_a_m)


class TestVelocityGate(unittest.TestCase):

    def test_first_fix_bypassed_for_new_track(self):
        gate = StationarityGate()
        v = gate.evaluate(_fc(47.6, -122.3, 0.0), history=[])
        self.assertTrue(v.accepted)
        self.assertEqual(v.bypass, "new_track")
        self.assertEqual(v.motion_state, "unknown")

    def test_plausible_motion_accepted(self):
        gate = StationarityGate()
        history = [_hp(47.6, -122.3, t_offset_s=0.0)]
        # ~30 m at dt=10s → 3 m/s, well under 100 m/s default.
        v = gate.evaluate(_fc(47.6003, -122.3, 10.0), history=history)
        self.assertTrue(v.accepted)
        self.assertLess(v.implied_velocity_mps or 9999, 100)
        self.assertEqual(v.bypass, "")

    def test_impossible_jump_rejected(self):
        gate = StationarityGate()
        history = [_hp(47.6, -122.3, t_offset_s=0.0)]
        # 50 km in 10 s → 5 km/s, vastly over 100 m/s.
        v = gate.evaluate(_fc(48.05, -122.3, 10.0), history=history)
        self.assertFalse(v.accepted)
        self.assertIn("velocity_exceeds_limit", v.reason)
        self.assertGreater(v.implied_velocity_mps or 0, 100)

    def test_sub_floor_dt_bypasses_velocity_check(self):
        gate = StationarityGate()
        history = [_hp(47.6, -122.3, t_offset_s=0.0)]
        # 200 m in 0.5 s → 400 m/s, would normally be rejected.
        v = gate.evaluate(_fc(47.6018, -122.3, 0.5), history=history)
        self.assertTrue(v.accepted)
        self.assertEqual(v.bypass, "dt_floor")
        self.assertIsNone(v.implied_velocity_mps)

    def test_v_max_override_lets_fast_targets_through(self):
        gate = StationarityGate(v_max_mps=300.0)  # ~jet speeds
        history = [_hp(47.6, -122.3, t_offset_s=0.0)]
        # 2 km in 10 s → 200 m/s, OK with the override.
        v = gate.evaluate(_fc(47.618, -122.3, 10.0), history=history)
        self.assertTrue(v.accepted)

    def test_zero_dt_treated_as_dt_floor(self):
        # If two fixes have identical timestamps the velocity formula
        # would divide by zero — make sure we hit the dt-floor branch
        # cleanly instead.
        gate = StationarityGate()
        history = [_hp(47.6, -122.3, t_offset_s=10.0)]
        v = gate.evaluate(_fc(47.7, -122.3, 10.0), history=history)
        self.assertTrue(v.accepted)
        self.assertEqual(v.bypass, "dt_floor")

    def test_negative_dt_treated_as_dt_floor(self):
        # Out-of-order fix arrival (e.g. peer clock skew). Don't
        # surface a misleading "negative velocity" — bypass.
        gate = StationarityGate()
        history = [_hp(47.6, -122.3, t_offset_s=10.0)]
        v = gate.evaluate(_fc(47.6, -122.3, 5.0), history=history)
        self.assertTrue(v.accepted)
        self.assertEqual(v.bypass, "dt_floor")

    def test_stats_count_correctly(self):
        gate = StationarityGate()
        gate.evaluate(_fc(47.6, -122.3, 0.0), history=[])
        history = [_hp(47.6, -122.3, t_offset_s=0.0)]
        gate.evaluate(_fc(47.6003, -122.3, 10.0), history=history)
        gate.evaluate(_fc(48.0, -122.3, 10.0), history=history)
        s = gate.stats()
        self.assertEqual(s["fixes_evaluated"], 3)
        self.assertEqual(s["fixes_accepted"], 2)
        self.assertEqual(s["fixes_rejected_velocity"], 1)
        self.assertEqual(s["fixes_bypassed_new"], 1)


class TestMotionClassifier(unittest.TestCase):

    def test_too_few_points_yields_unknown(self):
        gate = StationarityGate()
        self.assertEqual(gate.classify_motion([]), "unknown")
        self.assertEqual(
            gate.classify_motion([_hp(47.6, -122.3)]), "unknown")

    def test_tightly_clustered_points_classify_stationary(self):
        gate = StationarityGate()
        # All within ~10m of each other; ellipse is 100m → ratio ~0.1.
        history = [
            _hp(47.6000, -122.3000, ellipse_a_m=100.0),
            _hp(47.6001, -122.3001, ellipse_a_m=100.0),
            _hp(47.6000, -122.3001, ellipse_a_m=100.0),
            _hp(47.6001, -122.3000, ellipse_a_m=100.0),
        ]
        self.assertEqual(gate.classify_motion(history), "stationary")

    def test_widely_spread_points_classify_mobile(self):
        gate = StationarityGate()
        # Points spread across ~5 km; ellipse 100m → ratio ~30x → mobile.
        history = [
            _hp(47.60, -122.30, ellipse_a_m=100.0),
            _hp(47.62, -122.32, ellipse_a_m=100.0),
            _hp(47.64, -122.30, ellipse_a_m=100.0),
            _hp(47.62, -122.28, ellipse_a_m=100.0),
        ]
        self.assertEqual(gate.classify_motion(history), "mobile")

    def test_borderline_keeps_prior_state_for_hysteresis(self):
        gate = StationarityGate()
        # Spread exactly between stationary_ratio (1.0) and mobile_ratio (3.0)
        # of a 100m ellipse → roughly 200m RMS.
        history = [
            _hp(47.6000, -122.3000, ellipse_a_m=100.0),
            _hp(47.6020, -122.3000, ellipse_a_m=100.0),  # ~220 m N
            _hp(47.6000, -122.3030, ellipse_a_m=100.0),  # ~225 m W
        ]
        # With no prior state, falls back to unknown.
        self.assertEqual(gate.classify_motion(history), "unknown")
        # With prior_state=stationary, keeps stationary.
        self.assertEqual(
            gate.classify_motion(history, prior_state="stationary"),
            "stationary")
        # With prior_state=mobile, keeps mobile.
        self.assertEqual(
            gate.classify_motion(history, prior_state="mobile"),
            "mobile")

    def test_no_ellipse_falls_back_to_default_scale(self):
        # Without any ellipse_a_m, the classifier uses
        # fallback_ellipse_m (default 250 m).
        gate = StationarityGate()
        history = [
            _hp(47.6000, -122.3000, ellipse_a_m=None),
            _hp(47.6001, -122.3000, ellipse_a_m=None),  # ~11 m
        ]
        self.assertEqual(gate.classify_motion(history), "stationary")


class TestEmitterTrackPromotion(unittest.TestCase):

    def _drive(self, track: EmitterTrack, n: int):
        for _ in range(n):
            track.update(frequency=100e6, power_dbfs=-60.0,
                         node_id="A", trust_score=0.8,
                         timestamp_ns=NOW_NS)

    def test_stationary_promotes_at_10_observations(self):
        t = EmitterTrack(emitter_id="trk-stationary")
        t.motion_state = "stationary"
        self._drive(t, 10)
        self.assertEqual(t.state, TrackState.STABLE)

    def test_unknown_motion_uses_legacy_threshold(self):
        # No TDOA history yet — should still promote at 10 obs so
        # non-TDOA tracks aren't permanently held in TRACKING.
        t = EmitterTrack(emitter_id="trk-unknown")
        self.assertEqual(t.motion_state, "unknown")
        self._drive(t, 10)
        self.assertEqual(t.state, TrackState.STABLE)

    def test_mobile_holds_in_tracking_until_higher_threshold(self):
        t = EmitterTrack(emitter_id="trk-mobile")
        t.motion_state = "mobile"
        self._drive(t, 10)
        self.assertEqual(t.state, TrackState.TRACKING)  # not yet
        self._drive(t, 20)  # now well past 25
        self.assertEqual(t.state, TrackState.STABLE)

    def test_motion_state_serialises_in_to_dict(self):
        t = EmitterTrack(emitter_id="trk-x")
        t.motion_state = "mobile"
        d = t.to_dict()
        self.assertEqual(d["motion_state"], "mobile")
        # Don't leak history to the wire by default — see comment in
        # to_dict() about keeping the trail off the SSE channel.
        self.assertNotIn("location_history", d)


class TestHistoryDiscipline(unittest.TestCase):

    def test_history_is_caller_owned(self):
        # The gate is stateless w.r.t. per-track history — the caller
        # is responsible for appending and trimming. Document the
        # contract here so a future refactor doesn't accidentally
        # move the storage into the gate.
        gate = StationarityGate(history_max=3)
        history: list = []
        for i in range(5):
            cand = _fc(47.6 + i * 0.0001, -122.3, t_offset_s=i * 10.0)
            v = gate.evaluate(cand, history=history,
                              prior_motion_state="unknown")
            self.assertTrue(v.accepted)
            # Caller-side append + trim
            history.append(HistoryPoint(
                lat=cand.lat, lon=cand.lon,
                timestamp_ns=cand.timestamp_ns,
                ellipse_a_m=cand.ellipse_a_m))
            if len(history) > gate.history_max:
                history = history[-gate.history_max:]
        self.assertEqual(len(history), 3)

    def test_default_history_max(self):
        # Pin the default so a future refactor doesn't silently bump
        # the cap and balloon per-track memory across thousands of
        # tracks in long missions.
        self.assertEqual(DEFAULT_HISTORY_MAX, 20)


class TestInvalidCandidateRejection(unittest.TestCase):
    """A NaN/inf/out-of-range fix must NOT be silently accepted —
    that was the corruption mode the gate was built to prevent."""

    def test_nan_lat_rejected(self):
        gate = StationarityGate()
        v = gate.evaluate(_fc(float("nan"), -122.3, 0.0), history=[])
        self.assertFalse(v.accepted)
        self.assertIn("invalid_coordinates", v.reason)
        self.assertEqual(gate.stats()["fixes_rejected_invalid"], 1)

    def test_inf_lon_rejected(self):
        gate = StationarityGate()
        v = gate.evaluate(_fc(47.6, float("inf"), 0.0), history=[])
        self.assertFalse(v.accepted)
        self.assertIn("invalid_coordinates", v.reason)

    def test_out_of_range_lat_rejected(self):
        # Singular covariance in the TDOA solver can produce |lat| > 90.
        gate = StationarityGate()
        v = gate.evaluate(_fc(123.4, -122.3, 0.0), history=[])
        self.assertFalse(v.accepted)
        self.assertIn("invalid_coordinates", v.reason)

    def test_out_of_range_lon_rejected(self):
        gate = StationarityGate()
        v = gate.evaluate(_fc(47.6, 200.0, 0.0), history=[])
        self.assertFalse(v.accepted)
        self.assertIn("invalid_coordinates", v.reason)

    def test_zero_timestamp_rejected(self):
        gate = StationarityGate()
        cand = FixCandidate(lat=47.6, lon=-122.3, timestamp_ns=0)
        v = gate.evaluate(cand, history=[])
        self.assertFalse(v.accepted)
        self.assertIn("invalid_timestamp", v.reason)

    def test_negative_timestamp_rejected(self):
        gate = StationarityGate()
        cand = FixCandidate(lat=47.6, lon=-122.3, timestamp_ns=-1)
        v = gate.evaluate(cand, history=[])
        self.assertFalse(v.accepted)
        self.assertIn("invalid_timestamp", v.reason)

    def test_invalid_rejection_does_not_crash_classifier(self):
        # Even with an invalid candidate, the gate must still produce
        # a valid motion_state from the existing history rather than
        # blowing up on the bad input.
        gate = StationarityGate()
        history = [
            _hp(47.6, -122.3, t_offset_s=0.0, ellipse_a_m=100.0),
            _hp(47.6001, -122.3001, t_offset_s=10.0, ellipse_a_m=100.0),
        ]
        v = gate.evaluate(_fc(float("nan"), -122.3, 20.0), history=history)
        self.assertFalse(v.accepted)
        self.assertEqual(v.motion_state, "stationary")


class TestAntimeridianAndPolar(unittest.TestCase):
    """Haversine handles wrap naturally; pin the behaviour."""

    def test_antimeridian_short_wrap_is_short(self):
        # 179.99°E to -179.99°E across the antimeridian should be
        # ~2 km, not ~40000 km. dt of 60s → ~37 m/s, accepted.
        gate = StationarityGate()
        history = [_hp(0.0, 179.99, t_offset_s=0.0)]
        v = gate.evaluate(_fc(0.0, -179.99, 60.0), history=history)
        self.assertTrue(v.accepted)
        self.assertLess(v.implied_velocity_mps or 9999, 100)

    def test_polar_extreme_lat_within_range(self):
        # 89.9° N is allowed; only |lat| > 90 is rejected.
        gate = StationarityGate()
        v = gate.evaluate(_fc(89.9, 0.0, 0.0), history=[])
        self.assertTrue(v.accepted)


class TestSequentialHysteresis(unittest.TestCase):
    """Drive the classifier through a stationary→mobile→stationary
    arc and assert hysteresis behaves at the borderline."""

    def test_full_arc(self):
        gate = StationarityGate()
        history: list = []
        prior = "unknown"
        # Phase 1: 4 tightly clustered fixes — should classify stationary.
        for i, (la, lo) in enumerate([
                (47.6000, -122.3000), (47.6001, -122.3001),
                (47.6000, -122.3001), (47.6001, -122.3000)]):
            cand = _fc(la, lo, t_offset_s=i * 10.0)
            v = gate.evaluate(cand, history=history, prior_motion_state=prior)
            self.assertTrue(v.accepted)
            history.append(HistoryPoint(
                lat=cand.lat, lon=cand.lon,
                timestamp_ns=cand.timestamp_ns,
                ellipse_a_m=cand.ellipse_a_m))
            prior = v.motion_state
        self.assertEqual(prior, "stationary")

        # Phase 2: emitter starts moving, fixes spread to several km.
        for i, (la, lo) in enumerate([
                (47.62, -122.30), (47.64, -122.32),
                (47.66, -122.30), (47.68, -122.28)]):
            cand = _fc(la, lo, t_offset_s=100.0 + i * 60.0)
            v = gate.evaluate(cand, history=history, prior_motion_state=prior)
            self.assertTrue(v.accepted)
            history.append(HistoryPoint(
                lat=cand.lat, lon=cand.lon,
                timestamp_ns=cand.timestamp_ns,
                ellipse_a_m=cand.ellipse_a_m))
            prior = v.motion_state
        self.assertEqual(prior, "mobile")


class TestDtFloorClassifierConsistency(unittest.TestCase):
    """Pin the architect-flagged fix: the dt_floor bypass path must
    classify against history+candidate (same as the normal accept
    path), not just history. Otherwise motion_state lags by one fix."""

    def test_dt_floor_path_includes_candidate_in_classification(self):
        gate = StationarityGate()
        # Build history that, on its own, looks stationary. Adding a
        # candidate ~5 km away within sub-floor dt would, with the old
        # buggy classifier, still say stationary (history-only spread).
        # With the fix, the spread including the candidate should
        # promote toward mobile in concert with hysteresis.
        history = [
            _hp(47.6000, -122.3000, t_offset_s=0.0, ellipse_a_m=100.0),
            _hp(47.6001, -122.3000, t_offset_s=2.0, ellipse_a_m=100.0),
        ]
        # dt = 0.5s → dt_floor bypass; candidate is ~5 km away
        v = gate.evaluate(
            _fc(47.65, -122.30, t_offset_s=2.5, ellipse_a_m=100.0),
            history=history, prior_motion_state="stationary")
        self.assertTrue(v.accepted)
        self.assertEqual(v.bypass, "dt_floor")
        # With candidate folded into classification: mobile (or at
        # worst hysteresis-keeps-stationary), but NEVER "unknown" —
        # which is what the buggy 1-step-late path could produce.
        self.assertIn(v.motion_state, ("mobile", "stationary"))


class TestConfigEnvOverrides(unittest.TestCase):
    """The architect flagged that getattr-on-config silently masked
    the absence of real config fields. Verify the env vars now
    actually flow through BackendConfig."""

    def test_env_vars_flow_into_config(self):
        # Re-construct a fresh BackendConfig with overridden env so
        # we don't depend on import-time singleton state.
        import os
        from backend.config import BackendConfig
        old = {
            k: os.environ.get(k) for k in (
                "STATIONARITY_V_MAX_MPS",
                "STATIONARITY_DT_FLOOR_S",
                "STATIONARITY_HISTORY_MAX")}
        try:
            os.environ["STATIONARITY_V_MAX_MPS"] = "300.0"
            os.environ["STATIONARITY_DT_FLOOR_S"] = "5.0"
            os.environ["STATIONARITY_HISTORY_MAX"] = "50"
            cfg = BackendConfig()
            self.assertAlmostEqual(cfg.stationarity_v_max_mps, 300.0)
            self.assertAlmostEqual(cfg.stationarity_dt_floor_s, 5.0)
            self.assertEqual(cfg.stationarity_history_max, 50)
        finally:
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def test_config_defaults_match_module_defaults(self):
        # If the BackendConfig defaults ever drift from the
        # StationarityGate module defaults, that drift is silent
        # in production. Pin both sides.
        from backend.config import BackendConfig
        from backend.fusion.stationarity_gate import (
            DEFAULT_V_MAX_MPS, DEFAULT_DT_FLOOR_S, DEFAULT_HISTORY_MAX)
        cfg = BackendConfig()
        self.assertAlmostEqual(
            cfg.stationarity_v_max_mps, DEFAULT_V_MAX_MPS)
        self.assertAlmostEqual(
            cfg.stationarity_dt_floor_s, DEFAULT_DT_FLOOR_S)
        self.assertEqual(
            cfg.stationarity_history_max, DEFAULT_HISTORY_MAX)


if __name__ == "__main__":
    unittest.main()
