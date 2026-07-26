"""Unit tests for the DF-Kracked LOB sensor.

Covers:
  * doa_result → KRAKEN_LOB event-row conversion (shape + raw schema)
  * /v1/events?since= serial cursor semantics
  * X-Kujhad-Key auth rejection on /v1/*

Run: python3 -m pytest df_kracked_sensor/test_sensor.py
 or: python3 -m unittest df_kracked_sensor.test_sensor
"""
import asyncio
import json
import os
import stat
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sensor  # noqa: E402
from sensor import (  # noqa: E402
    EventRing,
    KrakenIngester,
    KujhadSensorApp,
    NodeEquipment,
    NodePosition,
    SweepIngester,
    antenna_gain_at,
    cal_db_at,
    doa_result_to_event_row,
    parse_sweep_csv_line,
    sweep_hit_to_event_row,
    sweep_line_to_hits,
    validate_node_config,
)


def make_doa(**over):
    base = {
        "type": "doa_result",
        "freq_hz": 433920000,
        "bearing_deg": 127.5,
        "bearing_std_deg": 5.2,
        "confidence": 0.83,
        "power_dbfs": -42.1,
        "snr_db": 12.3,
        "gps_lat": 37.4,
        "gps_lon": -122.1,
        "heading_deg": 0.0,
        "timestamp_unix": 1718035200.123,
        "node_id": "kraken-0",
    }
    base.update(over)
    return base


class TestConversion(unittest.TestCase):
    def test_full_row_shape(self):
        row = doa_result_to_event_row(
            make_doa(), serial=7, node_id="fallback", source_label="Sensor:x",
            fallback_lat=0.0, fallback_lon=0.0, fallback_heading=0.0,
            have_fix=False)
        self.assertIsNotNone(row)
        # Top-level fields the controller expects.
        self.assertEqual(row["type"], "decoder")
        self.assertEqual(row["decoder"], "KRAKEN_LOB")
        self.assertEqual(row["hitState"], "decoded")
        self.assertEqual(row["protocol"], "DOA")
        self.assertEqual(row["frequency"], 433920000)
        self.assertEqual(row["talkgroup"], "Unknown")
        self.assertEqual(row["radioId"], "Unknown")
        self.assertFalse(row["hasAudio"])
        self.assertTrue(row["hasData"])
        self.assertEqual(row["source"], "Sensor:x")
        self.assertTrue(row["gpsFix"])
        self.assertEqual(row["lat"], 37.4)
        self.assertEqual(row["lon"], -122.1)
        self.assertEqual(row["serial"], 7)
        self.assertIn("eventId", row)
        # time must be a formatted local-time STRING (controller reads it
        # via readJsonString), not an int epoch.
        self.assertIsInstance(row["time"], str)
        self.assertRegex(row["time"], r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$")
        # machine-readable time stays in raw.timestamp_unix
        self.assertAlmostEqual(row["raw"]["timestamp_unix"], 1718035200.123)

    def test_raw_schema_load_bearing_keys(self):
        row = doa_result_to_event_row(
            make_doa(), serial=1, node_id="n", source_label="s",
            fallback_lat=0.0, fallback_lon=0.0, fallback_heading=0.0,
            have_fix=False)
        raw = row["raw"]
        # These keys are read by the controller aggregator.
        self.assertAlmostEqual(raw["bearing_deg"], 127.5)
        self.assertAlmostEqual(raw["gps_lat"], 37.4)
        self.assertAlmostEqual(raw["gps_lon"], -122.1)
        self.assertAlmostEqual(raw["timestamp_unix"], 1718035200.123)
        self.assertAlmostEqual(raw["bearing_std_deg"], 5.2)
        self.assertAlmostEqual(raw["confidence"], 0.83)
        self.assertAlmostEqual(raw["freq_hz"], 433920000)
        self.assertAlmostEqual(raw["heading_deg"], 0.0)
        self.assertEqual(raw["node_id"], "kraken-0")

    def test_native_aliases(self):
        # doa_max_deg + frequency_hz + doa_std_deg (native producer form)
        msg = {
            "type": "doa_result",
            "doa_max_deg": 200.0,
            "frequency_hz": 100000000,
            "doa_std_deg": 3.3,
            "gps_lat": 1.0, "gps_lon": 2.0,
        }
        row = doa_result_to_event_row(
            msg, 1, "n", "s", 0.0, 0.0, 0.0, False)
        self.assertAlmostEqual(row["raw"]["bearing_deg"], 200.0)
        self.assertAlmostEqual(row["frequency"], 100000000)
        self.assertAlmostEqual(row["raw"]["bearing_std_deg"], 3.3)

    def test_reject_non_doa(self):
        self.assertIsNone(doa_result_to_event_row(
            {"type": "status"}, 1, "n", "s", 0, 0, 0, False))

    def test_reject_bad_bearing(self):
        self.assertIsNone(doa_result_to_event_row(
            make_doa(bearing_deg=400.0), 1, "n", "s", 0, 0, 0, False))

    def test_fallback_position_used(self):
        msg = make_doa()
        del msg["gps_lat"]
        del msg["gps_lon"]
        row = doa_result_to_event_row(
            msg, 1, "n", "s", 51.5, -0.12, 45.0, have_fix=True)
        self.assertEqual(row["lat"], 51.5)
        self.assertEqual(row["lon"], -0.12)

    def test_reject_no_position_no_fix(self):
        msg = make_doa()
        del msg["gps_lat"]
        del msg["gps_lon"]
        self.assertIsNone(doa_result_to_event_row(
            msg, 1, "n", "s", 0, 0, 0, have_fix=False))

    def test_reject_null_island(self):
        self.assertIsNone(doa_result_to_event_row(
            make_doa(gps_lat=0.0, gps_lon=0.0), 1, "n", "s", 0, 0, 0, False))


class TestEventRing(unittest.TestCase):
    def _row(self, ring):
        s = ring.next_serial()
        row = doa_result_to_event_row(
            make_doa(), s, "n", "s", 0, 0, 0, False)
        ring.append(row)
        return s

    def test_serial_monotonic(self):
        ring = EventRing()
        s1 = ring.next_serial()
        s2 = ring.next_serial()
        self.assertEqual(s2, s1 + 1)

    def test_since_cursor(self):
        ring = EventRing()
        base = ring.last_serial   # boot-time serial base (restart-monotonic)
        for _ in range(5):
            self._row(ring)
        events, last_id = ring.since(base)
        self.assertEqual(len(events), 5)
        self.assertEqual(last_id, base + 5)
        # since=base+3 → only serials base+4, base+5
        events, last_id = ring.since(base + 3)
        self.assertEqual([e["serial"] for e in events], [base + 4, base + 5])
        self.assertEqual(last_id, base + 5)
        # since at head → nothing new, lastId stays at since
        events, last_id = ring.since(base + 5)
        self.assertEqual(events, [])
        self.assertEqual(last_id, base + 5)

    def test_since_oldest_first(self):
        ring = EventRing()
        for _ in range(3):
            self._row(ring)
        events, _ = ring.since(0)
        serials = [e["serial"] for e in events]
        self.assertEqual(serials, sorted(serials))

    def test_ring_bounded(self):
        ring = EventRing(maxlen=3)
        base = ring.last_serial
        for _ in range(5):
            self._row(ring)
        events, last_id = ring.since(base)
        self.assertEqual(len(events), 3)          # only last 3 retained
        self.assertEqual([e["serial"] for e in events],
                         [base + 3, base + 4, base + 5])
        self.assertEqual(last_id, base + 5)

    def test_since_caps_backlog_to_newest_rows(self):
        # A fresh cursor (since=0 after peer add) must not replay the whole
        # ring — only the newest SINCE_MAX_ROWS, with lastId at the head so
        # the skipped backlog is never re-sent.
        ring = EventRing()
        base = ring.last_serial
        for _ in range(EventRing.SINCE_MAX_ROWS * 2 + 20):
            self._row(ring)
        events, last_id = ring.since(base)
        self.assertEqual(len(events), EventRing.SINCE_MAX_ROWS)
        self.assertEqual(last_id, ring.last_serial)
        self.assertEqual(events[-1]["serial"], ring.last_serial)  # newest kept

    def test_serial_base_survives_restart(self):
        # A fresh ring (simulated restart) must start ABOVE any serial a
        # controller could have seen from the previous run, else its
        # since-cursor mutes all new events (field-hit bug).
        # The ms-clock base outruns real emission (throttled to ~2 events/s
        # vs 1000 serials/s of clock advance); emulate that ratio here.
        old = EventRing()
        for _ in range(50):
            old.next_serial()
        time.sleep(0.1)
        fresh = EventRing()
        self.assertGreater(fresh.last_serial + 1, old.last_serial)


class TestThrottleDedup(unittest.TestCase):
    def test_dedup_identical_bearing(self):
        ring = EventRing()
        pos = NodePosition(0, 0, 0, False)
        ing = KrakenIngester("ws://x/ws", ring, pos, "n", "s")
        r1 = ing.handle_message(json.dumps(make_doa(bearing_deg=90.0,
                                                    gps_lat=1.0, gps_lon=2.0)))
        self.assertIsNotNone(r1)
        # Immediate identical bearing → coalesced (None).
        r2 = ing.handle_message(json.dumps(make_doa(bearing_deg=90.0,
                                                    gps_lat=1.0, gps_lon=2.0)))
        self.assertIsNone(r2)

    def test_rate_cap(self):
        ring = EventRing()
        pos = NodePosition(0, 0, 0, False)
        ing = KrakenIngester("ws://x/ws", ring, pos, "n", "s")
        ing.handle_message(json.dumps(make_doa(bearing_deg=10.0,
                                               gps_lat=1.0, gps_lon=2.0)))
        # Different bearing but within the rate window → suppressed.
        r = ing.handle_message(json.dumps(make_doa(bearing_deg=200.0,
                                                   gps_lat=1.0, gps_lon=2.0)))
        self.assertIsNone(r)


# ── DF Aggregator XML polling (stock krakensdr_doa bearing feed) ────────────

# Field-verified sample from the operator's Kraken Pi.
DFA_SAMPLE = (
    "<DATA><STATION_ID>STATIC</STATION_ID><TIME>1784858533581</TIME>"
    "<GPS_TIME>0</GPS_TIME><FREQUENCY>854.182592</FREQUENCY>"
    "<LOCATION><LATITUDE>39.1928</LATITUDE><LONGITUDE>-76.7241</LONGITUDE>"
    "<HEADING>180</HEADING></LOCATION><DOA>181.0</DOA><PWR>59.7</PWR>"
    "<CONF>122</CONF><LATENCY>436</LATENCY><PROCESSING_TIME>10</PROCESSING_TIME></DATA>"
)


class TestDfAggregatorParse(unittest.TestCase):
    def test_field_verified_sample(self):
        msgs = sensor.parse_df_aggregator_xml(DFA_SAMPLE)
        self.assertEqual(len(msgs), 1)
        m = msgs[0]
        self.assertEqual(m["type"], "doa_result")
        # True bearing = DOA(181) + HEADING(180) mod 360 = 1.0
        self.assertAlmostEqual(m["bearing_deg"], 1.0)
        self.assertAlmostEqual(m["heading_deg"], 180.0)
        self.assertAlmostEqual(m["frequency_hz"], 854182592.0, delta=1.0)
        self.assertAlmostEqual(m["timestamp_unix"], 1784858533.581, places=3)
        self.assertAlmostEqual(m["power_dbfs"], 59.7)
        self.assertEqual(m["confidence"], 1.0)   # CONF 122 clamps to 1.0
        self.assertAlmostEqual(m["gps_lat"], 39.1928)
        self.assertAlmostEqual(m["gps_lon"], -76.7241)
        self.assertEqual(m["station_id"], "STATIC")

    def test_doa_is_true_skips_heading_add(self):
        m = sensor.parse_df_aggregator_xml(DFA_SAMPLE, doa_is_true=True)[0]
        self.assertAlmostEqual(m["bearing_deg"], 181.0)

    def test_zero_latlon_omitted(self):
        blob = DFA_SAMPLE.replace("39.1928", "0.0").replace("-76.7241", "0.0")
        m = sensor.parse_df_aggregator_xml(blob)[0]
        self.assertNotIn("gps_lat", m)

    def test_garbage_and_missing_doa(self):
        self.assertEqual(sensor.parse_df_aggregator_xml("not xml"), [])
        self.assertEqual(sensor.parse_df_aggregator_xml(""), [])
        self.assertEqual(
            sensor.parse_df_aggregator_xml("<DATA><TIME>1</TIME></DATA>"), [])
        self.assertEqual(sensor.parse_df_aggregator_xml(None), [])

    def test_multiple_data_blocks(self):
        msgs = sensor.parse_df_aggregator_xml(DFA_SAMPLE + DFA_SAMPLE.replace(
            "181.0", "90.0"))
        self.assertEqual(len(msgs), 2)

    def test_parsed_msg_flows_into_event_row(self):
        ring = EventRing()
        pos = NodePosition(0, 0, 0, False)
        ing = KrakenIngester("http://x/DOA_value.html", ring, pos, "n", "s")
        m = sensor.parse_df_aggregator_xml(DFA_SAMPLE)[0]
        row = ing.handle_doa_msg(m)
        self.assertIsNotNone(row)
        self.assertEqual(row["decoder"], "KRAKEN_LOB")
        self.assertAlmostEqual(row["raw"]["bearing_deg"], 1.0)
        self.assertAlmostEqual(row["lat"], 39.1928)


# ── Generic-SDR power sweep (rtl_power / hackrf_sweep) ───────────────────────

# rtl_power CSV: date, time, hz_low, hz_high, hz_step, samples, db, db, ...
RTL_POWER_LINE = (
    "2024-06-10, 12:00:00, 433800000, 434000000, 100000, 128, "
    "-70.0, -71.0, -30.0, -69.5"
)
# hackrf_sweep shares the shape (whole-Hz edges, MHz-derived).
HACKRF_LINE = (
    "2024-06-10, 12:00:01, 400000000, 400500000, 100000, 20, "
    "-80.0, -79.0, -78.5, -20.0, -81.0"
)


class TestSweepCsvParse(unittest.TestCase):
    def test_parse_rtl_power_line(self):
        p = parse_sweep_csv_line(RTL_POWER_LINE)
        self.assertIsNotNone(p)
        self.assertAlmostEqual(p["hz_low"], 433800000)
        self.assertAlmostEqual(p["hz_high"], 434000000)
        self.assertAlmostEqual(p["hz_step"], 100000)
        self.assertAlmostEqual(p["samples"], 128)
        self.assertEqual(p["dbs"], [-70.0, -71.0, -30.0, -69.5])

    def test_parse_hackrf_line(self):
        p = parse_sweep_csv_line(HACKRF_LINE)
        self.assertIsNotNone(p)
        self.assertEqual(len(p["dbs"]), 5)
        self.assertAlmostEqual(p["dbs"][3], -20.0)

    def test_reject_comment_header_short(self):
        self.assertIsNone(parse_sweep_csv_line("# comment"))
        self.assertIsNone(parse_sweep_csv_line(""))
        self.assertIsNone(parse_sweep_csv_line("a, b, c"))
        self.assertIsNone(parse_sweep_csv_line(None))

    def test_reject_non_numeric_header(self):
        bad = "date, time, hz_low, hz_high, hz_step, samples, db"
        self.assertIsNone(parse_sweep_csv_line(bad))

    def test_trailing_empty_field_skipped(self):
        line = RTL_POWER_LINE + ", "
        p = parse_sweep_csv_line(line)
        self.assertEqual(p["dbs"], [-70.0, -71.0, -30.0, -69.5])


class TestSweepThresholding(unittest.TestCase):
    def test_hit_above_floor(self):
        p = parse_sweep_csv_line(RTL_POWER_LINE)
        # floor = median([-70,-71,-30,-69.5]) = (-70 + -69.5)/2 = -69.75
        # only -30 exceeds floor + 12 dB (-57.75).
        hits = sweep_line_to_hits(p, snr_threshold=12.0)
        self.assertEqual(len(hits), 1)
        hit = hits[0]
        self.assertAlmostEqual(hit["db"], -30.0)
        self.assertAlmostEqual(hit["snr_db"], 39.75)
        # bin index 2 → center = 433800000 + (2 + 0.5)*100000 = 434050000
        self.assertAlmostEqual(hit["freq_hz"], 434050000.0)

    def test_no_hit_when_flat(self):
        flat = ("2024-06-10, 12:00:00, 100000000, 100100000, 50000, 10, "
                "-60.0, -60.5, -59.5")
        p = parse_sweep_csv_line(flat)
        self.assertEqual(sweep_line_to_hits(p, snr_threshold=12.0), [])

    def test_lower_threshold_yields_more_hits(self):
        p = parse_sweep_csv_line(HACKRF_LINE)
        # floor = median([-80,-79,-78.5,-20,-81]) = -79.0
        # threshold 5 → -20 (snr 59) only; the rest are near/below floor.
        hits = sweep_line_to_hits(p, snr_threshold=5.0)
        self.assertEqual(len(hits), 1)
        self.assertAlmostEqual(hits[0]["db"], -20.0)


class TestSweepEventRow(unittest.TestCase):
    def test_hit_row_shape_matches_accepted_type(self):
        hit = {"freq_hz": 434050000.0, "db": -30.0, "snr_db": 39.75}
        row = sweep_hit_to_event_row(
            hit, serial=42, node_id="sdr-0", source_label="Sensor:x",
            lat=37.4, lon=-122.1, heading=10.0, have_fix=True,
            ts_unix=1718035200.0)
        # type MUST be one the coordinator fleet ingest accepts ('hit').
        self.assertEqual(row["type"], "hit")
        self.assertEqual(row["detector"], "sweep")
        self.assertEqual(row["frequency"], 434050000.0)
        self.assertEqual(row["freqHz"], 434050000.0)
        self.assertEqual(row["strengthDb"], -30.0)
        self.assertAlmostEqual(row["snrDb"], 39.75)
        self.assertTrue(row["gpsFix"])
        self.assertEqual(row["lat"], 37.4)
        self.assertEqual(row["lon"], -122.1)
        self.assertEqual(row["serial"], 42)
        self.assertNotIn("bearing_deg", row["raw"])  # no bearing on a sweep
        self.assertIsInstance(row["time"], str)
        self.assertRegex(row["time"], r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$")
        self.assertAlmostEqual(row["raw"]["freq_hz"], 434050000.0)
        self.assertAlmostEqual(row["raw"]["snr_db"], 39.75)


class TestSweepRateLimit(unittest.TestCase):
    def test_process_line_emits_and_rate_limits(self):
        ring = EventRing()
        pos = NodePosition(37.0, -122.0, 0.0, True)
        sw = SweepIngester(ring, pos, "sdr-0", "Sensor:x",
                           ranges=["433800000:434000000:100000"],
                           snr_threshold=12.0)
        base = ring.last_serial
        sw._process_line(RTL_POWER_LINE)
        events, _ = ring.since(base)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["type"], "hit")
        # Same bucket again immediately → rate-limited (no new event).
        sw._process_line(RTL_POWER_LINE)
        events, _ = ring.since(base)
        self.assertEqual(len(events), 1)

    def test_build_cmd_rtl_power(self):
        sw = SweepIngester(EventRing(), NodePosition(0, 0, 0, False),
                           "n", "s", ranges=["400M:470M:100k"], interval_s=5)
        cmd = sw._build_cmd("rtl_power")
        self.assertEqual(cmd[0], "rtl_power")
        self.assertIn("400M:470M:100k", cmd)
        self.assertIn("-", cmd)

    def test_build_cmd_hackrf_sweep_mhz(self):
        sw = SweepIngester(EventRing(), NodePosition(0, 0, 0, False),
                           "n", "s", ranges=["400M:470M:100k"])
        cmd = sw._build_cmd("hackrf_sweep")
        self.assertEqual(cmd[0], "hackrf_sweep")
        self.assertIn("400:470", cmd)


# ── Node equipment calibration (mirrors node_equipment.h) ───────────────────


class TestAntennaGain(unittest.TestCase):
    def test_empty_curve_is_zero(self):
        self.assertEqual(antenna_gain_at([], 465e6), 0.0)

    def test_zero_or_invalid_freq_is_zero(self):
        curve = [{"f": 150.0, "g": 2.0}, {"f": 915.0, "g": 8.0}]
        self.assertEqual(antenna_gain_at(curve, 0.0), 0.0)
        self.assertEqual(antenna_gain_at(curve, -1.0), 0.0)
        self.assertEqual(antenna_gain_at(curve, float("inf")), 0.0)

    def test_single_point_is_flat(self):
        curve = [{"f": 465.0, "g": 5.0}]
        self.assertAlmostEqual(antenna_gain_at(curve, 100e6), 5.0)
        self.assertAlmostEqual(antenna_gain_at(curve, 900e6), 5.0)

    def test_end_hold_outside_range(self):
        curve = [{"f": 150.0, "g": 2.0}, {"f": 915.0, "g": 8.0}]
        # Below the lowest point → hold first gain.
        self.assertAlmostEqual(antenna_gain_at(curve, 50e6), 2.0)
        # Above the highest point → hold last gain.
        self.assertAlmostEqual(antenna_gain_at(curve, 2000e6), 8.0)

    def test_log_f_interpolation_midpoint(self):
        # Two points; the geometric-mean frequency (equal in log10 space)
        # must yield the arithmetic mean of the two gains.
        curve = [{"f": 100.0, "g": 0.0}, {"f": 1000.0, "g": 10.0}]
        mid_hz = (100.0 * 1000.0) ** 0.5 * 1e6   # ~316.2 MHz
        self.assertAlmostEqual(antenna_gain_at(curve, mid_hz), 5.0, places=4)

    def test_endpoints_exact(self):
        curve = [{"f": 150.0, "g": 2.0}, {"f": 915.0, "g": 8.0}]
        self.assertAlmostEqual(antenna_gain_at(curve, 150e6), 2.0)
        self.assertAlmostEqual(antenna_gain_at(curve, 915e6), 8.0)

    def test_unsorted_input_sorted(self):
        curve = [{"f": 915.0, "g": 8.0}, {"f": 150.0, "g": 2.0}]
        self.assertAlmostEqual(antenna_gain_at(curve, 50e6), 2.0)
        self.assertAlmostEqual(antenna_gain_at(curve, 2000e6), 8.0)


class TestCalDbComposition(unittest.TestCase):
    def test_sign_and_sum(self):
        # calDb = sdrOffset + antennaGain(at f) + sitingOffset
        # hackrf_clone offset -6.5; curve single point +5; siting ground -4.0
        curve = [{"f": 465.0, "g": 5.0}]
        cal = cal_db_at("hackrf_clone", curve, 465e6, "ground")
        self.assertAlmostEqual(cal, -6.5 + 5.0 - 4.0)

    def test_reference_is_zero(self):
        # rtlsdr_v3 (0) + empty curve (0) + mast (0) = 0
        self.assertAlmostEqual(cal_db_at("rtlsdr_v3", [], 465e6, "mast"), 0.0)

    def test_rooftop_positive_bias(self):
        cal = cal_db_at("rtlsdr_v3", [], 465e6, "rooftop")
        self.assertAlmostEqual(cal, 4.0)   # rooftop offset +4.0

    def test_equipment_stamp_on_row(self):
        eq = NodeEquipment(sdr_type="hackrf", antenna_curve=[{"f": 465.0, "g": 3.0}],
                           terrain="urban", siting="body_worn")
        row = {"frequency": 465e6}
        eq.stamp(row)
        # calDb = -4.0 + 3.0 + (-6.0) = -7.0
        self.assertAlmostEqual(row["calDb"], -7.0)
        self.assertAlmostEqual(row["plExp"], 3.3)       # urban exponent
        self.assertAlmostEqual(row["rssiSigmaDb"], 6.0 + 3.0)  # body_worn +3


class TestNodeConfigValidation(unittest.TestCase):
    def _ok(self, **over):
        base = {
            "lat": 37.0, "lon": -122.0, "gpsdEnabled": True,
            "sdrType": "rtlsdr_v4",
            "antennaCurve": [{"f": 465.0, "g": 5.0}],
            "terrain": "urban", "siting": "rooftop",
        }
        base.update(over)
        return base

    def test_accept_clean(self):
        clean, err = validate_node_config(self._ok())
        self.assertIsNone(err)
        self.assertEqual(clean["sdrType"], "rtlsdr_v4")
        self.assertEqual(clean["terrain"], "urban")
        self.assertEqual(clean["siting"], "rooftop")
        self.assertEqual(clean["antennaCurve"], [{"f": 465.0, "g": 5.0}])
        self.assertTrue(clean["gpsdEnabled"])

    def test_reject_non_object(self):
        clean, err = validate_node_config([1, 2, 3])
        self.assertIsNone(clean)
        self.assertIn("object", err)

    def test_reject_bad_latlon(self):
        clean, err = validate_node_config(self._ok(lat=200.0))
        self.assertIsNone(clean)
        self.assertIn("lat/lon", err)

    def test_reject_unknown_sdr(self):
        clean, err = validate_node_config(self._ok(sdrType="nope"))
        self.assertIsNone(clean)
        self.assertIn("sdrType", err)

    def test_reject_unknown_terrain(self):
        clean, err = validate_node_config(self._ok(terrain="mars"))
        self.assertIsNone(clean)
        self.assertIn("terrain", err)

    def test_reject_unknown_siting(self):
        clean, err = validate_node_config(self._ok(siting="orbit"))
        self.assertIsNone(clean)
        self.assertIn("siting", err)

    def test_reject_too_many_points(self):
        pts = [{"f": 100.0 + i, "g": 0.0} for i in range(17)]
        clean, err = validate_node_config(self._ok(antennaCurve=pts))
        self.assertIsNone(clean)
        self.assertIn("16", err)

    def test_reject_freq_out_of_range(self):
        clean, err = validate_node_config(
            self._ok(antennaCurve=[{"f": 99999.0, "g": 0.0}]))
        self.assertIsNone(clean)
        self.assertIn("frequency", err)

    def test_reject_gain_out_of_range(self):
        clean, err = validate_node_config(
            self._ok(antennaCurve=[{"f": 465.0, "g": 999.0}]))
        self.assertIsNone(clean)
        self.assertIn("gain", err)

    def test_reject_curve_not_array(self):
        clean, err = validate_node_config(self._ok(antennaCurve="nope"))
        self.assertIsNone(clean)
        self.assertIn("array", err)

    def test_legacy_flat_gain_converted(self):
        body = {"lat": 0.0, "lon": 0.0, "sdrType": "unknown",
                "terrain": "mixed", "siting": "mast", "antennaGainDb": 3.0}
        clean, err = validate_node_config(body)
        self.assertIsNone(err)
        self.assertEqual(clean["antennaCurve"], [{"f": 400.0, "g": 3.0}])


class TestEmittedRowsCarryCalibration(unittest.TestCase):
    def test_sweep_hit_row_stamped(self):
        ring = EventRing()
        pos = NodePosition(37.0, -122.0, 0.0, True)
        eq = NodeEquipment(sdr_type="rtlsdr_v4",
                           antenna_curve=[{"f": 434.0, "g": 2.0}],
                           terrain="suburban", siting="vehicle_roof")
        sw = SweepIngester(ring, pos, "sdr-0", "Sensor:x",
                           ranges=["433800000:434000000:100000"],
                           snr_threshold=12.0, equip=eq)
        base = ring.last_serial
        sw._process_line(
            "2024-06-10, 12:00:00, 433800000, 434000000, 100000, 128, "
            "-70.0, -71.0, -30.0, -69.5")
        events, _ = ring.since(base)
        self.assertEqual(len(events), 1)
        row = events[0]
        self.assertIn("calDb", row)
        self.assertIn("plExp", row)
        self.assertIn("rssiSigmaDb", row)
        self.assertAlmostEqual(row["plExp"], 2.8)          # suburban
        self.assertAlmostEqual(row["rssiSigmaDb"], 6.0 + 0.5)  # vehicle_roof

    def test_kraken_bearing_row_stamped_and_keeps_bearing(self):
        ring = EventRing()
        pos = NodePosition(0, 0, 0, False)
        eq = NodeEquipment(sdr_type="rtlsdr_v3", antenna_curve=[],
                           terrain="mixed", siting="mast")
        ing = KrakenIngester("ws://x/ws", ring, pos, "n", "s", equip=eq)
        row = ing.handle_message(json.dumps(make_doa(bearing_deg=90.0,
                                                     gps_lat=1.0, gps_lon=2.0)))
        self.assertIsNotNone(row)
        self.assertIn("calDb", row)
        self.assertIn("plExp", row)
        self.assertIn("rssiSigmaDb", row)
        # Bearing fields must be preserved on the Kraken row.
        self.assertAlmostEqual(row["raw"]["bearing_deg"], 90.0)


# ── aiohttp server auth + endpoints ─────────────────────────────────────────

@unittest.skipUnless(os.name == "posix", "POSIX file-mode semantics required")
class TestKeyFilePermissions(unittest.TestCase):
    def test_created_key_file_is_0600(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "df_kracked_sensor.json")
            key = sensor.load_or_create_key(path, None)
            self.assertTrue(key)
            self.assertTrue(os.path.isfile(path))
            mode = stat.S_IMODE(os.stat(path).st_mode)
            self.assertEqual(
                mode, 0o600,
                f"expected 0600, got {oct(mode)} — key file must be owner-only")

    def test_provided_key_file_is_0600(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "df_kracked_sensor.json")
            sensor.load_or_create_key(path, "operatorkey")
            mode = stat.S_IMODE(os.stat(path).st_mode)
            self.assertEqual(mode, 0o600, oct(mode))

    def test_loose_existing_file_is_tightened_on_load(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "df_kracked_sensor.json")
            # Simulate an older, world-readable file.
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"api_key": "legacy"}, f)
            os.chmod(path, 0o644)
            key = sensor.load_or_create_key(path, None)
            self.assertEqual(key, "legacy")
            mode = stat.S_IMODE(os.stat(path).st_mode)
            self.assertEqual(mode, 0o600, oct(mode))


from aiohttp.test_utils import AioHTTPTestCase  # noqa: E402


class TestServer(AioHTTPTestCase):
    async def get_application(self):
        self.ring = EventRing()
        pos = NodePosition(37.0, -122.0, 0.0, True)
        self.equip = NodeEquipment()
        self.ing = KrakenIngester("ws://x/ws", self.ring, pos, "kraken-0",
                                  "Sensor:t", equip=self.equip)
        self._cfgdir = tempfile.TemporaryDirectory()
        self._cfgpath = os.path.join(self._cfgdir.name, "df_kracked_sensor.json")
        self.app_obj = KujhadSensorApp("SECRETKEY", "df-test", self.ring, pos,
                                       self.ing, equip=self.equip,
                                       config_path=self._cfgpath,
                                       bind="10.0.0.5", port=9151)
        return self.app_obj.build()

    async def asyncTearDown(self):
        try:
            self._cfgdir.cleanup()
        except Exception:
            pass
        await super().asyncTearDown()

    async def _seed(self, n):
        for _ in range(n):
            s = self.ring.next_serial()
            self.ring.append(doa_result_to_event_row(
                make_doa(), s, "kraken-0", "Sensor:t", 0, 0, 0, False))

    async def test_identify_ok(self):
        resp = await self.client.get("/v1/identify",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        self.assertEqual(resp.status, 200)
        body = await resp.json()
        self.assertEqual(body["role"], "sensor")
        self.assertEqual(body["device"], "df-test")
        self.assertIn("hwProfile", body)
        self.assertEqual(set(["device", "version", "role"]) - set(body), set())

    async def test_auth_rejected_missing_key(self):
        for path in ("/v1/identify", "/v1/state", "/v1/gps", "/v1/events"):
            resp = await self.client.get(path)
            self.assertEqual(resp.status, 401, path)
            body = await resp.json()
            self.assertEqual(body["error"], "unauthorized")

    async def test_auth_rejected_wrong_key(self):
        resp = await self.client.get("/v1/state",
                                     headers={"X-Kujhad-Key": "WRONG"})
        self.assertEqual(resp.status, 401)

    async def test_state_shape(self):
        resp = await self.client.get("/v1/state",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        self.assertEqual(resp.status, 200)
        body = await resp.json()
        for key in ("scanStatus", "searchBands", "targets", "hits"):
            self.assertIn(key, body)
        self.assertEqual(body["searchBands"], [])

    async def test_events_since_cursor(self):
        base = self.ring.last_serial
        await self._seed(3)
        resp = await self.client.get(f"/v1/events?since={base}",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        body = await resp.json()
        self.assertEqual(body["lastId"], base + 3)
        self.assertEqual(len(body["events"]), 3)
        self.assertEqual(body["events"][0]["decoder"], "KRAKEN_LOB")

        resp = await self.client.get(f"/v1/events?since={base + 2}",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        body = await resp.json()
        self.assertEqual([e["serial"] for e in body["events"]], [base + 3])
        self.assertEqual(body["lastId"], base + 3)

    async def test_command_graceful_501(self):
        resp = await self.client.post(
            "/v1/command", headers={"X-Kujhad-Key": "SECRETKEY"},
            data=json.dumps({"class": "tune", "action": "set"}))
        self.assertEqual(resp.status, 501)
        body = await resp.json()
        self.assertFalse(body["ok"])

    async def test_root_public(self):
        resp = await self.client.get("/")
        self.assertEqual(resp.status, 200)

    async def test_state_reports_equipment_and_sweep_blocks(self):
        resp = await self.client.get("/v1/state",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        body = await resp.json()
        self.assertIn("equipment", body)
        self.assertIn("sdrType", body["equipment"])
        self.assertIn("antennaCurvePoints", body["equipment"])
        self.assertIn("kraken", body)

    async def test_setup_page_public(self):
        resp = await self.client.get("/setup")
        self.assertEqual(resp.status, 200)
        self.assertIn("text/html", resp.headers.get("Content-Type", ""))
        text = await resp.text()
        self.assertIn("Node Setup", text)
        self.assertIn("/v1/node-config", text)

    async def test_setup_page_embeds_all_option_tables(self):
        # All static option tables must be server-rendered into the page so
        # the dropdowns populate with ZERO authed /v1 calls.
        text = await (await self.client.get("/setup")).text()
        for p in sensor.SDR_PROFILES:
            self.assertIn(p["id"], text)
        for p in sensor.TERRAIN_PROFILES:
            self.assertIn(p["id"], text)
            self.assertIn(str(p["exponent"]), text)
        for p in sensor.SITING_PROFILES:
            self.assertIn(p["id"], text)
        for p in sensor.ANTENNA_PRESETS:
            self.assertIn(p["label"], text)
        # Base sigma for the live preview is embedded too.
        self.assertIn("baseRssiSigmaDb", text)
        self.assertIn(str(sensor.BASE_RSSI_SIGMA_DB), text)

    async def test_setup_page_needs_no_v1_calls(self):
        # The page must render fully without ever calling an authed endpoint:
        # selects are populated from the embedded blob, not from a fetch on
        # load. Verify the embedded blob (source of truth) is a valid JSON
        # object containing every option table, and that the page does not
        # auto-fetch node-config unconditionally.
        text = await (await self.client.get("/setup")).text()
        blob = text.split("const TABLES=", 1)[1].split(";", 1)[0]
        tables = json.loads(blob)
        self.assertEqual(len(tables["sdrOptions"]), len(sensor.SDR_PROFILES))
        self.assertEqual(len(tables["terrainOptions"]),
                         len(sensor.TERRAIN_PROFILES))
        self.assertEqual(len(tables["sitingOptions"]),
                         len(sensor.SITING_PROFILES))
        self.assertEqual(len(tables["antennaPresets"]),
                         len(sensor.ANTENNA_PRESETS))
        # Selects are filled from the blob synchronously (populateSelects),
        # and the authed load only fires if a key is already stored.
        self.assertIn("populateSelects()", text)
        self.assertIn("if($('key').value){loadCfg()}", text)

    async def test_node_config_requires_auth(self):
        resp = await self.client.get("/v1/node-config")
        self.assertEqual(resp.status, 401)
        resp = await self.client.post("/v1/node-config", data="{}")
        self.assertEqual(resp.status, 401)

    async def test_node_config_get_shape(self):
        resp = await self.client.get("/v1/node-config",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        self.assertEqual(resp.status, 200)
        body = await resp.json()
        for k in ("sdrOptions", "antennaPresets", "terrainOptions",
                  "sitingOptions", "sdrType", "antennaCurve", "terrain",
                  "siting", "lat", "lon", "gpsdEnabled"):
            self.assertIn(k, body)
        # Option tables must match the equipment tables.
        self.assertEqual(len(body["sdrOptions"]), len(sensor.SDR_PROFILES))

    async def test_node_config_post_accept_and_persist(self):
        payload = {
            "lat": 40.0, "lon": -75.0, "gpsdEnabled": True,
            "sdrType": "hackrf",
            "antennaCurve": [{"f": 465.0, "g": 5.0}],
            "terrain": "urban", "siting": "rooftop",
        }
        resp = await self.client.post(
            "/v1/node-config", headers={"X-Kujhad-Key": "SECRETKEY"},
            data=json.dumps(payload))
        self.assertEqual(resp.status, 200)
        body = await resp.json()
        self.assertTrue(body["ok"])
        # Applied to the live equipment.
        self.assertEqual(self.equip.sdr_type, "hackrf")
        self.assertEqual(self.equip.terrain, "urban")
        self.assertEqual(self.equip.siting, "rooftop")
        # Persisted to the config file (survives restart).
        with open(self._cfgpath, "r", encoding="utf-8") as f:
            cfg = json.load(f)
        self.assertEqual(cfg["sdrType"], "hackrf")
        self.assertEqual(cfg["terrain"], "urban")
        self.assertEqual(cfg["antennaCurve"], [{"f": 465.0, "g": 5.0}])

    async def test_node_config_post_reject_bad_400(self):
        resp = await self.client.post(
            "/v1/node-config", headers={"X-Kujhad-Key": "SECRETKEY"},
            data=json.dumps({"sdrType": "bogus", "lat": 0, "lon": 0}))
        self.assertEqual(resp.status, 400)
        body = await resp.json()
        self.assertIn("error", body)
        # Nothing partially applied.
        self.assertEqual(self.equip.sdr_type, "unknown")

    async def test_pairing_requires_auth_and_returns_key(self):
        resp = await self.client.get("/v1/pairing")
        self.assertEqual(resp.status, 401)
        resp = await self.client.get("/v1/pairing",
                                     headers={"X-Kujhad-Key": "SECRETKEY"})
        self.assertEqual(resp.status, 200)
        body = await resp.json()
        self.assertEqual(body["key"], "SECRETKEY")
        self.assertEqual(body["port"], 9151)
        self.assertIn("10.0.0.5", body["addresses"])


if __name__ == "__main__":
    unittest.main()
