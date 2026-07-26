#!/usr/bin/env python3
"""
DF-Kracked LOB sensor — a standalone Kujhad v1 fleet peer for a KrakenSDR Pi.

This service turns a KrakenSDR Raspberry Pi into a lightweight, sensor-only
peer on the Predator RF "Kujhad" fleet. It:

  * connects to the local krakensdr_doa DoA WebSocket (default
    ws://127.0.0.1:8082/ws), read-only, parsing "doa_result" frames;
  * converts each usable frame into a KRAKEN_LOB "decoder" event row that is
    byte-for-byte compatible with the rows the Predator RF controller builds
    from its own native decoders (see core/src/gui/main_window.cpp and
    decoder_modules/kraken_lob_decoder/src/main.cpp);
  * serves the Kujhad v1 HTTP+JSON protocol (X-Kujhad-Key auth) so any
    controller / phone app can pair by IP:port + key and mirror our bearings.

The controller's aggregator reads events[i].raw.bearing_deg / gps_lat /
gps_lon / timestamp_unix, so those raw keys are load-bearing and must match
the KRAKEN_LOB schema exactly.

Standalone: stdlib + aiohttp + websockets only. Does not import backend/.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import re
import signal
import socket
import sys
import time
import uuid
from collections import deque
from typing import Any, Deque, Dict, List, Optional, Tuple

try:
    import aiohttp
    from aiohttp import web
except ImportError:  # pragma: no cover - import guard for operator clarity
    sys.stderr.write(
        "FATAL: aiohttp is not installed. Run install.sh or "
        "`pip3 install aiohttp websockets`.\n"
    )
    raise

try:
    import websockets
except ImportError:  # pragma: no cover
    websockets = None  # ws client degrades gracefully; HTTP still serves.


log = logging.getLogger("df_kracked_sensor")

# ── Constants ────────────────────────────────────────────────────────────

EVENT_RING_MAX = 500
DEFAULT_PORT = 9151
DEFAULT_WS_URL = "ws://127.0.0.1:8082/ws"
# Stock krakensdr_doa has no websocket: it continuously rewrites a bearing
# file in the web root served by miniserve on :8081. With
# doa_data_format = "DF Aggregator" that file is a one-line XML blob
# (field-verified). HTTP polling of this URL is therefore the DEFAULT
# ingest mode; --ws switches to the websocket for custom builds.
DEFAULT_DOA_URL = "http://127.0.0.1:8081/DOA_value.html"
DOA_POLL_INTERVAL_S = 0.5
# Throttle: at most ~2 events/s, and coalesce identical bearings within 0.5 s.
MIN_EMIT_INTERVAL_S = 0.5
DEDUP_WINDOW_S = 0.5
DEDUP_BEARING_EPS = 0.05  # degrees; below this two bearings count as identical
CONFIG_FILENAME = "df_kracked_sensor.json"

# ── Generic-SDR power sweep (SDR-agnostic ingest) ───────────────────────────
# The sensor also works with a plain RTL-SDR or HackRF (no Kraken): it shells
# out to rtl_power / hackrf_sweep, reads their CSV, and emits "power hit" rows
# (type 'hit', detector 'sweep') for bins that stick up above the noise floor.
#
# USB vendor:product IDs → sweep tool. RTL2838/RTL2832 dongles use rtl_power;
# a HackRF One (1d50:6089) uses hackrf_sweep.
SWEEP_USB_IDS = {
    "0bda:2838": "rtl_power",   # RTL2838 (Blog V3 etc.)
    "0bda:2832": "rtl_power",   # RTL2832U
    "1d50:6089": "hackrf_sweep",  # HackRF One
}
# Default frequency ranges to sweep when --sweep-range is not given. Chosen to
# cover the common civil/ISM bands a bare RTL-SDR reaches without an upconverter
# and that carry the kind of intermittent emitters a fox-hunt cares about:
#   * 400M:470M  UHF business/amateur/PMR (100 kHz bins)
#   * 902M:928M  US 900 MHz ISM (LoRa, telemetry) (100 kHz bins)
# Format is rtl_power's '-f' range: '<low>:<high>:<binwidth>'.
DEFAULT_SWEEP_RANGES = ["400M:470M:100k", "902M:928M:100k"]
SWEEP_DEFAULT_SNR_DB = 12.0        # bin must exceed floor + this to be a hit
SWEEP_DEFAULT_INTERVAL_S = 5       # rtl_power integration time per sweep
SWEEP_MIN_EMIT_INTERVAL_S = 5.0    # per freq-bucket rate limit (anti-spam)
SWEEP_REDETECT_INTERVAL_S = 60.0   # hotplug re-probe cadence when idle
SWEEP_RESPAWN_DELAY_S = 10.0       # wait after tool exit before respawn
SWEEP_RETASK_DEBOUNCE_S = 0.5      # coalesce retask bursts (drag-to-tune)
                                   # into one rtl_power relaunch
SWEEP_KRAKEN_GRACE_S = 30.0        # wait before (re)starting sweep in auto mode
SWEEP_MAX_RANGES = 8               # config cap on configured sweep ranges
SWEEP_ROTATE_DWELL_S = 20.0        # per-range dwell before round-robin rotation
# Validation bounds for a sweep range's edge frequencies (Hz).
SWEEP_FREQ_MIN_HZ = 0.5e6          # 0.5 MHz
SWEEP_FREQ_MAX_HZ = 7000e6         # 7000 MHz
SWEEP_INTERVAL_MIN_S = 1
SWEEP_INTERVAL_MAX_S = 60
SWEEP_SNR_MIN_DB = 3.0
SWEEP_SNR_MAX_DB = 40.0
SWEEP_MODES = ("auto", "on", "off")

# ── Spectrum stream (GET /v1/spectrum, NDJSON frames) ───────────────────────
SPECTRUM_MAX_BINS = 1024           # downsample cap (same policy as the app)
SPECTRUM_KEEPALIVE_S = 5.0         # idle keepalive frame cadence
SPECTRUM_CLIENT_QUEUE_MAX = 4      # per-client frame queue; drop-oldest if full
SPECTRUM_DB_MARGIN = 3.0           # padding added to frame fftMin/Max bounds
SPECTRUM_FLOOR_DB = -150.0         # sentinel for empty buckets / keepalive floor

# ── Manual tune (POST /v1/command tune.set) ─────────────────────────────────
TUNE_DEFAULT_HALF_SPAN_HZ = 2e6    # default ±2 MHz window around the tune freq
TUNE_STEP_HZ = 10_000              # ~10 kHz bins for decent manual resolution


# ── Node equipment calibration (mirrors core/src/predator/node_equipment.h) ──
#
# The AOU grid fuses received-power DIFFERENCES between nodes, which only works
# if every node reports power on a comparable scale. Each node declares its SDR
# type, antenna gain curve, terrain and siting; every emitted hit row is stamped
# with a per-frequency correction term (calDb), a path-loss exponent (plExp) and
# an RSSI trust sigma (rssiSigmaDb). Convention (same as the C++ node): the
# coordinator computes comparable_power = strengthDb - calDb. We do NOT subtract
# locally. These tables are copied byte-for-byte from node_equipment.h.
import math  # noqa: E402  (grouped with the equipment tables it supports)

# SDR profiles: (id, label, offsetDb vs RTL-SDR Blog v3 reference).
SDR_PROFILES: List[Dict[str, Any]] = [
    {"id": "rtlsdr_v3",    "label": "RTL-SDR Blog v3 (reference)",  "offsetDb": 0.0},
    {"id": "rtlsdr_v4",    "label": "RTL-SDR Blog v4",              "offsetDb": 0.5},
    {"id": "rtlsdr_clone", "label": "Generic RTL2832 clone",       "offsetDb": -2.0},
    {"id": "nesdr",        "label": "Nooelec NESDR",               "offsetDb": -0.5},
    {"id": "hackrf",       "label": "HackRF One",                  "offsetDb": -4.0},
    {"id": "hackrf_clone", "label": "HackRF clone",                "offsetDb": -6.5},
    {"id": "airspy_mini",  "label": "Airspy Mini",                 "offsetDb": 1.5},
    {"id": "unknown",      "label": "Other / unknown",             "offsetDb": 0.0},
]

# Antenna presets: starting POINTS (freqMhz, gainDb), not whole curves.
ANTENNA_PRESETS: List[Dict[str, Any]] = [
    {"label": "Stock whip / rubber duck (0 dB, wideband)", "freqMhz": 400.0, "gainDb": 0.0},
    {"label": "VHF dipole (2 dB @ 150 MHz)",               "freqMhz": 150.0, "gainDb": 2.0},
    {"label": "GMRS 3 dB @ 465 MHz",                       "freqMhz": 465.0, "gainDb": 3.0},
    {"label": "GMRS 5 dB @ 465 MHz",                       "freqMhz": 465.0, "gainDb": 5.0},
    {"label": "900 MHz ISM 5 dB @ 915 MHz",                "freqMhz": 915.0, "gainDb": 5.0},
    {"label": "900 MHz ISM 8 dB @ 915 MHz",                "freqMhz": 915.0, "gainDb": 8.0},
    {"label": "Discone (2 dB, wideband @ 400 MHz)",        "freqMhz": 400.0, "gainDb": 2.0},
    {"label": "Yagi 7 dB @ 465 MHz",                       "freqMhz": 465.0, "gainDb": 7.0},
]

# Terrain profiles: (id, label, path-loss exponent n).
TERRAIN_PROFILES: List[Dict[str, Any]] = [
    {"id": "open_rural",   "label": "Open / rural / flat ground", "exponent": 2.2},
    {"id": "suburban",     "label": "Suburban / light clutter",   "exponent": 2.8},
    {"id": "light_forest", "label": "Light forest / parkland",    "exponent": 3.0},
    {"id": "dense_forest", "label": "Dense forest",               "exponent": 3.6},
    {"id": "urban",        "label": "City / urban",               "exponent": 3.3},
    {"id": "dense_urban",  "label": "Dense high-rise city",       "exponent": 4.0},
    {"id": "mixed",        "label": "Mixed / unknown",            "exponent": 3.0},
]

# Siting profiles: (id, label, offsetDb, sigmaExtraDb).
SITING_PROFILES: List[Dict[str, Any]] = [
    {"id": "mast",          "label": "Mast / tripod, clear (~2 m) — reference", "offsetDb": 0.0,  "sigmaExtraDb": 0.0},
    {"id": "ground",        "label": "On the ground",                           "offsetDb": -4.0, "sigmaExtraDb": 1.5},
    {"id": "body_worn",     "label": "Body-worn / carried",                     "offsetDb": -6.0, "sigmaExtraDb": 3.0},
    {"id": "vehicle_roof",  "label": "Vehicle roof",                            "offsetDb": -1.0, "sigmaExtraDb": 0.5},
    {"id": "side_building", "label": "Side of building",                        "offsetDb": -3.0, "sigmaExtraDb": 2.5},
    {"id": "rooftop",       "label": "Top of large structure / rooftop",        "offsetDb": 4.0,  "sigmaExtraDb": 1.0},
    {"id": "treetop",       "label": "Tied to top of tree",                     "offsetDb": 2.0,  "sigmaExtraDb": 1.5},
    {"id": "indoor_window", "label": "Indoors near window",                     "offsetDb": -8.0, "sigmaExtraDb": 4.0},
    {"id": "unknown",       "label": "Other / unknown",                         "offsetDb": 0.0,  "sigmaExtraDb": 1.0},
]

# Base RSSI noise sigma before siting inflation (matches the AOU default).
BASE_RSSI_SIGMA_DB = 6.0

# Curve is a max of 16 (freqMhz, gainDb) points; validation bounds:
ANTENNA_CURVE_MAX_POINTS = 16
ANTENNA_FREQ_MHZ_MIN = 0.1
ANTENNA_FREQ_MHZ_MAX = 7000.0
ANTENNA_GAIN_DB_MIN = -30.0
ANTENNA_GAIN_DB_MAX = 40.0


def sdr_offset_db(sdr_id: str) -> float:
    for p in SDR_PROFILES:
        if sdr_id == p["id"]:
            return float(p["offsetDb"])
    return 0.0


def terrain_exponent(terrain_id: str) -> float:
    for p in TERRAIN_PROFILES:
        if terrain_id == p["id"]:
            return float(p["exponent"])
    return 3.0


def siting_profile(siting_id: str) -> Dict[str, Any]:
    for p in SITING_PROFILES:
        if siting_id == p["id"]:
            return p
    return SITING_PROFILES[-1]  # "unknown"


def antenna_gain_at(curve: List[Dict[str, float]], freq_hz: float) -> float:
    """Interpolate antenna gain (dB) at freq_hz from a (freqMhz, gainDb) curve.

    Linear in log10(frequency); nearest-end value held outside the declared
    points; empty curve = 0 dB; invalid/zero frequency = 0 dB. Mirrors the C++
    predator::equipment::antennaGainAt exactly. Each point is {"f":MHz,"g":dB}.
    Never raises."""
    if not curve:
        return 0.0
    s = sorted(curve, key=lambda p: p.get("f", 0.0))
    f = freq_hz / 1e6
    if not math.isfinite(f) or f <= 0.0:
        return 0.0
    if f <= s[0]["f"]:
        return float(s[0]["g"])
    if f >= s[-1]["f"]:
        return float(s[-1]["g"])
    for i in range(1, len(s)):
        if f <= s[i]["f"]:
            f0 = math.log10(max(s[i - 1]["f"], 0.001))
            f1 = math.log10(max(s[i]["f"], 0.001))
            t = (math.log10(f) - f0) / (f1 - f0) if f1 > f0 else 0.0
            t = max(0.0, min(1.0, t))
            return float(s[i - 1]["g"] + t * (s[i]["g"] - s[i - 1]["g"]))
    return float(s[-1]["g"])


def cal_db_at(sdr_id: str, curve: List[Dict[str, float]], freq_hz: float,
              siting_id: str = "mast") -> float:
    """calDb = sdrOffset + antennaGainAt + sitingOffset (at the hit frequency).
    Mirrors predator::equipment::calDbAt. Never raises."""
    a = antenna_gain_at(curve, freq_hz)
    if not math.isfinite(a):
        a = 0.0
    return sdr_offset_db(sdr_id) + a + float(siting_profile(siting_id)["offsetDb"])


class NodeEquipment:
    """Live node calibration config, editable via /v1/node-config and persisted
    to the sensor's JSON config file. Rows are stamped from a snapshot of this."""

    def __init__(self, sdr_type: str = "unknown",
                 antenna_curve: Optional[List[Dict[str, float]]] = None,
                 terrain: str = "mixed", siting: str = "mast"):
        self.sdr_type = sdr_type
        self.antenna_curve: List[Dict[str, float]] = list(antenna_curve or [])
        self.terrain = terrain
        self.siting = siting

    def cal_db(self, freq_hz: float) -> float:
        return cal_db_at(self.sdr_type, self.antenna_curve, freq_hz, self.siting)

    def pl_exp(self) -> float:
        return terrain_exponent(self.terrain)

    def rssi_sigma_db(self) -> float:
        return BASE_RSSI_SIGMA_DB + float(siting_profile(self.siting)["sigmaExtraDb"])

    def stamp(self, row: Dict[str, Any]) -> None:
        """Stamp calDb (at the row's own frequency), plExp and rssiSigmaDb onto
        an event row. Same convention as the C++ node — no local subtraction."""
        try:
            freq_hz = _num(row.get("frequency"), 0.0)
        except Exception:  # noqa: BLE001 - defensive; never break emission
            freq_hz = 0.0
        row["calDb"] = self.cal_db(freq_hz)
        row["plExp"] = self.pl_exp()
        row["rssiSigmaDb"] = self.rssi_sigma_db()

    def summary(self) -> Dict[str, Any]:
        return {
            "sdrType": self.sdr_type,
            "antennaCurvePoints": len(self.antenna_curve),
            "terrain": self.terrain,
            "siting": self.siting,
        }

    def to_config(self) -> Dict[str, Any]:
        return {
            "sdrType": self.sdr_type,
            "antennaCurve": [
                {"f": float(p["f"]), "g": float(p["g"])} for p in self.antenna_curve
            ],
            "terrain": self.terrain,
            "siting": self.siting,
        }


def validate_node_config(body: Any) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    """Validate a POST /v1/node-config payload against the equipment tables.

    Returns (clean, None) on success or (None, error) on failure. NEVER applies
    partially — the caller applies only on success. Mirrors the C++ applier's
    checks: lat/lon range, curve ≤16 points, freq 0.1..7000 MHz, gain -30..40 dB,
    sdrType/terrain/siting must exist in the tables. `clean` carries normalised
    fields: lat, lon, gpsdEnabled, sdrType, antennaCurve, terrain, siting."""
    if not isinstance(body, dict):
        return None, "body must be a JSON object"

    lat = _num(body.get("lat"), 0.0)
    lon = _num(body.get("lon"), 0.0)
    if lat < -90.0 or lat > 90.0 or lon < -180.0 or lon > 180.0:
        return None, "lat/lon out of range"

    # Antenna gain curve: array of {f: MHz, g: dB}. Legacy flat antennaGainDb is
    # accepted and converted to a one-point curve (parity with C++).
    curve: List[Dict[str, float]] = []
    if "antennaCurve" in body:
        raw = body.get("antennaCurve")
        if not isinstance(raw, list):
            return None, "antennaCurve must be an array"
        if len(raw) > ANTENNA_CURVE_MAX_POINTS:
            return None, "antennaCurve: max 16 points"
        for p in raw:
            if not isinstance(p, dict):
                return None, "antennaCurve entries must be objects"
            f = _num(p.get("f"), 0.0)
            g = _num(p.get("g"), 0.0)
            if not (ANTENNA_FREQ_MHZ_MIN <= f <= ANTENNA_FREQ_MHZ_MAX):
                return None, "antennaCurve: frequency out of range (0.1..7000 MHz)"
            if not (ANTENNA_GAIN_DB_MIN <= g <= ANTENNA_GAIN_DB_MAX):
                return None, "antennaCurve: gain out of range (-30..40 dB)"
            curve.append({"f": f, "g": g})
    else:
        ant = _num(body.get("antennaGainDb"), 0.0)
        if not (ANTENNA_GAIN_DB_MIN <= ant <= ANTENNA_GAIN_DB_MAX):
            return None, "antenna gain out of range (-30..40 dB)"
        if ant != 0.0:
            curve.append({"f": 400.0, "g": ant})

    sdr = body.get("sdrType", "unknown")
    if not isinstance(sdr, str) or not any(sdr == p["id"] for p in SDR_PROFILES):
        return None, "unknown sdrType"

    terrain = body.get("terrain", "mixed")
    if not isinstance(terrain, str) or not any(
            terrain == p["id"] for p in TERRAIN_PROFILES):
        return None, "unknown terrain"

    siting = body.get("siting", "mast")
    if not isinstance(siting, str) or not any(
            siting == p["id"] for p in SITING_PROFILES):
        return None, "unknown siting"

    clean = {
        "lat": lat,
        "lon": lon,
        "gpsdEnabled": bool(body.get("gpsdEnabled", False)),
        "sdrType": sdr,
        "antennaCurve": curve,
        "terrain": terrain,
        "siting": siting,
    }

    # ── Sweep control (all optional; only validated/applied when present) ──
    if "sweepMode" in body:
        mode = body.get("sweepMode")
        if not isinstance(mode, str) or mode not in SWEEP_MODES:
            return None, "sweepMode must be one of on/off/auto"
        clean["sweepMode"] = mode

    if "sweepRanges" in body:
        raw = body.get("sweepRanges")
        if not isinstance(raw, list):
            return None, "sweepRanges must be an array"
        if len(raw) == 0:
            return None, "sweepRanges must have at least one range"
        if len(raw) > SWEEP_MAX_RANGES:
            return None, f"sweepRanges: max {SWEEP_MAX_RANGES} ranges"
        norm: List[str] = []
        for r in raw:
            n = validate_sweep_range(r)
            if n is None:
                return None, f"invalid sweep range: {r!r}"
            norm.append(n)
        clean["sweepRanges"] = norm

    if "sweepIntervalS" in body:
        iv = _num(body.get("sweepIntervalS"), 0.0)
        if not (SWEEP_INTERVAL_MIN_S <= iv <= SWEEP_INTERVAL_MAX_S):
            return None, (f"sweepIntervalS out of range "
                          f"({SWEEP_INTERVAL_MIN_S}..{SWEEP_INTERVAL_MAX_S})")
        clean["sweepIntervalS"] = int(iv)

    if "sweepSnrDb" in body:
        snr = _num(body.get("sweepSnrDb"), 0.0)
        if not (SWEEP_SNR_MIN_DB <= snr <= SWEEP_SNR_MAX_DB):
            return None, (f"sweepSnrDb out of range "
                          f"({SWEEP_SNR_MIN_DB:.0f}..{SWEEP_SNR_MAX_DB:.0f})")
        clean["sweepSnrDb"] = float(snr)

    return clean, None


# ── doa_result → KRAKEN_LOB event row ──────────────────────────────────────


def doa_result_to_event_row(
    msg: Dict[str, Any],
    serial: int,
    node_id: str,
    source_label: str,
    fallback_lat: float,
    fallback_lon: float,
    fallback_heading: float,
    have_fix: bool,
) -> Optional[Dict[str, Any]]:
    """Convert a krakensdr_doa 'doa_result' frame into a Kujhad event row.

    Returns None when the frame is not a usable DoA result (mirrors the
    C++ decoder_ingest.h validation: bearing in [0,360), a GPS position
    present). The returned row matches the shape main_window.cpp builds for
    native decoders, with the KRAKEN_LOB raw schema the controller reads.
    """
    if not isinstance(msg, dict):
        return None
    if msg.get("type") != "doa_result":
        return None

    # Bearing: accept doa_max_deg (native) or bearing_deg.
    bearing = None
    if "doa_max_deg" in msg and _is_num(msg["doa_max_deg"]):
        bearing = float(msg["doa_max_deg"])
    elif "bearing_deg" in msg and _is_num(msg["bearing_deg"]):
        bearing = float(msg["bearing_deg"])
    if bearing is None or bearing < 0.0 or bearing >= 360.0:
        return None

    # Frequency: frequency_hz (native) wins, else freq_hz alias.
    if "frequency_hz" in msg and _is_num(msg["frequency_hz"]):
        freq_hz = float(msg["frequency_hz"])
    else:
        freq_hz = _num(msg.get("freq_hz"), 0.0)

    confidence = _num(msg.get("confidence"), 0.5)
    confidence = max(0.0, min(1.0, confidence))

    # Uncertainty: an explicit bearing_std_deg / doa_std_deg wins. When the
    # feed doesn't carry one (the DF Aggregator XML only reports CONF), derive
    # it from confidence so the controller's wedge width is a live trust
    # indicator: conf 1.0 → 4° (tight sliver), conf 0.0 → 20° (wide fan).
    if "bearing_std_deg" in msg and _is_num(msg["bearing_std_deg"]):
        bearing_std = float(msg["bearing_std_deg"])
    elif "doa_std_deg" in msg and _is_num(msg["doa_std_deg"]):
        bearing_std = float(msg["doa_std_deg"])
    else:
        bearing_std = 20.0 - 16.0 * confidence
    power_dbfs = _num(msg.get("power_dbfs"), 0.0)
    snr_db = _num(msg.get("snr_db"), 0.0)

    # GPS position: prefer the frame's own fix, else the node's fixed site.
    gps_lat = msg.get("gps_lat")
    gps_lon = msg.get("gps_lon")
    frame_has_fix = _is_num(gps_lat) and _is_num(gps_lon)
    if frame_has_fix:
        lat = float(gps_lat)
        lon = float(gps_lon)
        gps_fix = True
    elif have_fix:
        lat = float(fallback_lat)
        lon = float(fallback_lon)
        gps_fix = True
    else:
        # No usable position at all — controller's LOB math needs one.
        return None

    # A crosscut needs a non-(0,0) origin; reject the null island.
    if lat == 0.0 and lon == 0.0:
        return None

    heading = _num(msg.get("heading_deg"), fallback_heading)
    ts_unix = _num(msg.get("timestamp_unix"), 0.0)
    if ts_unix <= 0.0:
        ts_unix = time.time()

    nid = msg.get("node_id") or node_id

    event_id = str(uuid.uuid4())
    # The controller reads row["time"] via readJsonString and renders it
    # verbatim in the event log / map popups (see currentTimestamp() in
    # main_window.cpp). It must be a formatted LOCAL-time string in the
    # same "%Y-%m-%d %H:%M:%S" shape the controller's own rows use — an
    # int epoch would render as "?". The machine-readable time stays in
    # raw.timestamp_unix.
    time_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts_unix))

    raw = {
        "bearing_deg": bearing,
        "bearing_std_deg": bearing_std,
        "confidence": confidence,
        "power_dbfs": power_dbfs,
        "snr_db": snr_db,
        "gps_lat": lat,
        "gps_lon": lon,
        "heading_deg": heading,
        "freq_hz": freq_hz,
        "timestamp_unix": ts_unix,
        "node_id": nid,
    }

    row = {
        "time": time_str,
        "eventId": event_id,
        "type": "decoder",
        "frequency": freq_hz,
        "label": nid or "KRAKEN_LOB",
        "strengthDb": power_dbfs,
        "decoder": "KRAKEN_LOB",
        "hitState": "decoded",
        "protocol": "DOA",
        "networkId": nid or "Unknown",
        "talkgroup": "Unknown",
        "radioId": "Unknown",
        "hasAudio": False,
        "hasData": True,
        "source": source_label,
        "gpsFix": gps_fix,
        "lat": lat,
        "lon": lon,
        "raw": raw,
        "serial": serial,
    }
    return row


# ── DF Aggregator XML → doa_result-shaped messages ─────────────────────────
#
# Example frame (single line, one <DATA> block per active VFO):
#   <DATA><STATION_ID>STATIC</STATION_ID><TIME>1784858533581</TIME>
#   <GPS_TIME>0</GPS_TIME><FREQUENCY>854.182592</FREQUENCY>
#   <LOCATION><LATITUDE>39.1928</LATITUDE><LONGITUDE>-76.7241</LONGITUDE>
#   <HEADING>180</HEADING></LOCATION><DOA>181.0</DOA><PWR>59.7</PWR>
#   <CONF>122</CONF><LATENCY>436</LATENCY>...
#
# TIME is epoch ms, FREQUENCY is MHz, DOA is the array-relative bearing
# (the aggregator is expected to add HEADING — that's why HEADING is in
# the frame at all). CONF is the krakensdr confidence metric (roughly
# 0-99+, uncalibrated).

_DFA_TAG = re.compile(r"<(STATION_ID|TIME|FREQUENCY|LATITUDE|LONGITUDE|HEADING|DOA|PWR|CONF)>([^<]*)</\1>")


def parse_df_aggregator_xml(text: str, doa_is_true: bool = False) -> List[Dict[str, Any]]:
    """Parse a DF Aggregator DOA_value.html blob into doa_result-shaped
    dicts (one per <DATA> block) consumable by doa_result_to_event_row.

    True bearing = (DOA + HEADING) mod 360 unless doa_is_true is set
    (some builds write an already-heading-corrected DOA).
    Blocks without a parseable DOA are skipped. Never raises.
    """
    out: List[Dict[str, Any]] = []
    if not isinstance(text, str):
        return out
    for block in text.split("<DATA>"):
        if "DOA" not in block:
            continue
        fields = {m.group(1): m.group(2).strip() for m in _DFA_TAG.finditer(block)}
        try:
            doa = float(fields["DOA"])
        except (KeyError, ValueError):
            continue
        def _f(key: str, default: float = 0.0) -> float:
            try:
                return float(fields.get(key, ""))
            except ValueError:
                return default
        heading = _f("HEADING", 0.0)
        bearing = doa if doa_is_true else (doa + heading)
        bearing %= 360.0
        msg: Dict[str, Any] = {
            "type": "doa_result",
            "bearing_deg": bearing,
            "heading_deg": heading,
            "frequency_hz": _f("FREQUENCY") * 1e6,
            "power_dbfs": _f("PWR"),
            # CONF is uncalibrated (can exceed 100); clamp into [0,1].
            "confidence": max(0.0, min(1.0, _f("CONF") / 100.0)),
            "timestamp_unix": _f("TIME") / 1000.0,
        }
        lat, lon = _f("LATITUDE"), _f("LONGITUDE")
        if lat != 0.0 or lon != 0.0:
            msg["gps_lat"] = lat
            msg["gps_lon"] = lon
        sid = fields.get("STATION_ID")
        if sid and sid not in ("", "NOCALL"):
            msg["station_id"] = sid
        out.append(msg)
    return out


def _is_num(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _num(v: Any, default: float) -> float:
    return float(v) if _is_num(v) else default


# ── Event ring with monotonic serials ──────────────────────────────────────


class EventRing:
    """Bounded ring of KRAKEN_LOB rows with monotonically increasing serials.

    /v1/events?since=<serial> returns rows whose serial > since, oldest-first,
    plus lastId = the max serial we hold (or `since` if none newer). This
    mirrors the C++ device server's cursor semantics exactly.
    """

    def __init__(self, maxlen: int = EVENT_RING_MAX):
        self._rows: Deque[Dict[str, Any]] = deque(maxlen=maxlen)
        # Serial base = boot time in ms. Serials must stay monotonically
        # increasing ACROSS sensor restarts: controllers poll with
        # since=<last seen serial> and never lower their cursor, so a
        # restart that reset serials to 0 would silently mute every new
        # event until the count exceeded the old cursor (field-hit bug).
        self._serial = int(time.time() * 1000)

    def next_serial(self) -> int:
        self._serial += 1
        return self._serial

    def append(self, row: Dict[str, Any]) -> None:
        self._rows.append(row)

    @property
    def last_serial(self) -> int:
        return self._serial

    # Bearings are perishable: on a fresh cursor (peer add / app restart the
    # controller polls since=0) do NOT replay the whole ring — a 500-row
    # backlog flood thrashes the controller's event log and plots long-stale
    # wedges as fresh. Return only the newest rows; lastId still advances to
    # the head so the cursor skips the rest.
    SINCE_MAX_ROWS = 50

    def since(self, since: int) -> Tuple[List[Dict[str, Any]], int]:
        """Return (newest ≤SINCE_MAX_ROWS events with serial > since,
        oldest-first, lastId)."""
        out: List[Dict[str, Any]] = []
        last_id = since
        # deque is oldest→newest already; the controller appends in order.
        for row in self._rows:
            serial = row.get("serial", 0)
            if not isinstance(serial, int) or serial <= 0:
                continue
            if serial <= since:
                continue
            out.append(row)
            if serial > last_id:
                last_id = serial
        if len(out) > self.SINCE_MAX_ROWS:
            out = out[-self.SINCE_MAX_ROWS:]
        return out, last_id

    def recent_hits(self, window_s: float, now: float,
                    limit: int = 64) -> List[Dict[str, Any]]:
        """Return spectrum-overlay markers {frequency,state,markerSlot,name}
        for rows emitted within the last window_s seconds. Only rows carrying a
        usable frequency are included (bearing rows without one are skipped).
        Newest-first, capped at `limit`. Never raises."""
        out: List[Dict[str, Any]] = []
        cutoff = now - window_s
        for row in reversed(self._rows):
            try:
                ts = row.get("raw", {}).get("timestamp_unix")
                if not isinstance(ts, (int, float)) or ts < cutoff:
                    continue
                freq = row.get("frequency")
                if not isinstance(freq, (int, float)) or freq <= 0:
                    continue
                out.append({
                    "frequency": float(freq),
                    "state": row.get("hitState", "auto") or "auto",
                    "markerSlot": -1,
                    "name": row.get("label", "") or "",
                })
                if len(out) >= limit:
                    break
            except Exception:  # noqa: BLE001 - overlay is best-effort
                continue
        return out


# ── Node position (fixed site or gpsd) ──────────────────────────────────────


class NodePosition:
    def __init__(self, lat: float, lon: float, heading: float, have_fix: bool):
        self.lat = lat
        self.lon = lon
        self.heading = heading
        self.have_fix = have_fix
        self.accuracy = 0.0

    def snapshot(self) -> Dict[str, Any]:
        return {
            "hasFix": self.have_fix,
            "lat": self.lat,
            "lon": self.lon,
            "accuracy": self.accuracy,
        }


async def gpsd_poll_loop(pos: NodePosition, host: str = "127.0.0.1",
                         port: int = 2947) -> None:
    """Poll gpsd (JSON protocol) and update pos in place. Degrades gracefully:
    if gpsd is unreachable or errors, we retry with backoff and never crash."""
    backoff = 2.0
    while True:
        reader = writer = None
        try:
            reader, writer = await asyncio.open_connection(host, port)
            writer.write(b'?WATCH={"enable":true,"json":true};\n')
            await writer.drain()
            backoff = 2.0
            log.info("gpsd: connected to %s:%d", host, port)
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    obj = json.loads(line.decode("utf-8", "replace"))
                except (ValueError, UnicodeDecodeError):
                    continue
                if obj.get("class") == "TPV":
                    lat = obj.get("lat")
                    lon = obj.get("lon")
                    if _is_num(lat) and _is_num(lon):
                        pos.lat = float(lat)
                        pos.lon = float(lon)
                        pos.have_fix = True
                        if _is_num(obj.get("track")):
                            pos.heading = float(obj["track"])
                        if _is_num(obj.get("eph")):
                            pos.accuracy = float(obj["eph"])
        except (OSError, asyncio.TimeoutError) as e:
            log.warning("gpsd: unreachable (%s); retry in %.0fs", e, backoff)
        finally:
            if writer is not None:
                try:
                    writer.close()
                except Exception:
                    pass
        await asyncio.sleep(backoff)
        backoff = min(backoff * 1.5, 30.0)


# ── Kraken DoA WebSocket ingester ───────────────────────────────────────────


class KrakenIngester:
    """Read-only WebSocket client for the krakensdr_doa DoA feed.

    Auto-reconnects with backoff, keeps state so the HTTP layer can report
    connection health, and applies throttle/dedup before publishing rows.
    """

    def __init__(self, url: str, ring: EventRing, pos: NodePosition,
                 node_id: str, source_label: str,
                 equip: Optional["NodeEquipment"] = None):
        self.url = url
        self.ring = ring
        self.pos = pos
        self.node_id = node_id
        self.source_label = source_label
        self.equip = equip
        self.connected = False
        self.events_received = 0
        self.events_emitted = 0
        self._last_emit_t = 0.0
        self._last_bearing: Optional[float] = None
        self._last_bearing_t = 0.0

    def _should_emit(self, bearing: float, now: float) -> bool:
        # Coalesce identical bearings inside the dedup window.
        if (self._last_bearing is not None
                and abs(bearing - self._last_bearing) <= DEDUP_BEARING_EPS
                and (now - self._last_bearing_t) < DEDUP_WINDOW_S):
            return False
        # Rate cap: at most one every MIN_EMIT_INTERVAL_S.
        if (now - self._last_emit_t) < MIN_EMIT_INTERVAL_S:
            return False
        return True

    def handle_message(self, text: str) -> Optional[Dict[str, Any]]:
        """Parse one WS text frame; publish a row if usable + not throttled.
        Returns the row that was published, or None. Pure enough to unit-test
        (does not touch the socket)."""
        try:
            msg = json.loads(text)
        except (ValueError, TypeError):
            return None
        if not isinstance(msg, dict) or msg.get("type") != "doa_result":
            return None  # ignore status/config frames silently
        return self.handle_doa_msg(msg)

    def handle_doa_msg(self, msg: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        """Throttle/dedup + publish one doa_result-shaped dict (from either
        the websocket or the DF Aggregator HTTP poller)."""
        self.events_received += 1

        # Adopt the Kraken's own per-frame GPS as the node position so
        # /v1/gps reports a fix without --lat/--lon or gpsd on the Pi.
        # Runs BEFORE throttling: even coalesced frames refresh the fix.
        f_lat, f_lon = msg.get("gps_lat"), msg.get("gps_lon")
        if _is_num(f_lat) and _is_num(f_lon) and not (f_lat == 0.0 and f_lon == 0.0):
            self.pos.lat = float(f_lat)
            self.pos.lon = float(f_lon)
            if _is_num(msg.get("heading_deg")):
                self.pos.heading = float(msg["heading_deg"])
            self.pos.have_fix = True

        bearing = msg.get("doa_max_deg")
        if not _is_num(bearing):
            bearing = msg.get("bearing_deg")
        if not _is_num(bearing):
            return None
        now = time.time()
        if not self._should_emit(float(bearing), now):
            self._last_bearing = float(bearing)
            self._last_bearing_t = now
            return None

        serial = self.ring.next_serial()
        row = doa_result_to_event_row(
            msg, serial, self.node_id, self.source_label,
            self.pos.lat, self.pos.lon, self.pos.heading, self.pos.have_fix,
        )
        if row is None:
            return None
        # Stamp calibration (calDb at the row's own freq, plExp, rssiSigmaDb)
        # onto the bearing row — it keeps its bearing fields untouched.
        if self.equip is not None:
            self.equip.stamp(row)
        self.ring.append(row)
        self.events_emitted += 1
        self._last_emit_t = now
        self._last_bearing = float(bearing)
        self._last_bearing_t = now
        return row

    async def run_http(self, stop: asyncio.Event, url: str,
                       doa_is_true: bool = False,
                       interval_s: float = DOA_POLL_INTERVAL_S) -> None:
        """Poll a DF Aggregator DOA_value.html and publish new frames.

        A frame is 'new' when its TIME (timestamp_unix) advances — the DoA
        software rewrites the file continuously, so an unchanged TIME means
        no new measurement (e.g. squelch closed / recalibrating).
        """
        last_ts = 0.0
        failures = 0
        session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=4.0))
        try:
            log.info("kraken: polling %s every %.1fs (DF Aggregator XML)",
                     url, interval_s)
            while not stop.is_set():
                try:
                    async with session.get(url) as resp:
                        text = await resp.text()
                    if resp.status != 200:
                        raise RuntimeError(f"HTTP {resp.status}")
                    if not self.connected:
                        log.info("kraken: CONNECTED (HTTP poll)")
                    self.connected = True
                    failures = 0
                    for msg in parse_df_aggregator_xml(text, doa_is_true):
                        ts = _num(msg.get("timestamp_unix"), 0.0)
                        if ts > last_ts:
                            last_ts = ts
                            self.handle_doa_msg(msg)
                except asyncio.CancelledError:
                    raise
                except Exception as e:  # noqa: BLE001 - keep polling
                    failures += 1
                    if self.connected or failures == 1:
                        log.warning("kraken: poll failed (%s)", e)
                    self.connected = False
                try:
                    await asyncio.wait_for(stop.wait(), timeout=interval_s)
                except asyncio.TimeoutError:
                    pass
        finally:
            self.connected = False
            await session.close()

    async def run(self, stop: asyncio.Event) -> None:
        if websockets is None:
            log.error("websockets library not available; Kraken ingest disabled")
            return
        backoff = 1.0
        while not stop.is_set():
            try:
                log.info("kraken: connecting to %s", self.url)
                async with websockets.connect(
                    self.url, ping_interval=20, ping_timeout=20,
                    max_size=2 ** 20, open_timeout=10,
                ) as ws:
                    if not self.connected:
                        log.info("kraken: CONNECTED")
                    self.connected = True
                    backoff = 1.0
                    async for message in ws:
                        if stop.is_set():
                            break
                        if isinstance(message, bytes):
                            message = message.decode("utf-8", "replace")
                        self.handle_message(message)
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001 - keep serving on any WS error
                if self.connected:
                    log.warning("kraken: DISCONNECTED (%s)", e)
                else:
                    log.debug("kraken: connect failed (%s)", e)
            self.connected = False
            if stop.is_set():
                break
            log.info("kraken: reconnecting in %.1fs", backoff)
            try:
                await asyncio.wait_for(stop.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2.0, 30.0)


# ── Generic-SDR power sweep ingester ────────────────────────────────────────


def detect_sweep_tool() -> Optional[Tuple[str, str]]:
    """Probe attached USB SDRs via `lsusb` and return (tool, "vid:pid") for the
    first recognised dongle, or None when nothing usable is attached.

    Never raises: on any lsusb failure (not installed, no permissions) it
    returns None so the caller simply re-probes later (hotplug)."""
    try:
        import subprocess
        out = subprocess.run(
            ["lsusb"], capture_output=True, text=True, timeout=3,
        )
    except Exception:  # noqa: BLE001 - lsusb missing / errored → treat as none
        return None
    if out.returncode != 0 or not out.stdout:
        return None
    text = out.stdout.lower()
    for vidpid, tool in SWEEP_USB_IDS.items():
        if vidpid in text:
            return tool, vidpid
    return None


# ── Hardware visibility (read-only; never claims the SDR) ───────────────────
#
# Known SDR USB ids for the /setup HARDWARE panel. Superset of SWEEP_USB_IDS
# plus Kraken/Airspy/Lime so the operator sees what is plugged in even when it
# is not something the sweep ingester itself drives. name is a human label.
SDR_USB_NAMES: Dict[str, str] = {
    "0bda:2838": "RTL2838 (RTL-SDR / KrakenSDR channel)",
    "0bda:2832": "RTL2832U (RTL-SDR)",
    "1d50:6089": "HackRF One",
    "1d50:60a1": "Airspy",
    "1d50:6108": "LimeSDR Mini",
    "0403:601f": "LimeSDR (FTDI)",
    "1d0f:5250": "LimeSDR",
    "2cf0:5250": "LimeSDR",
}
# USB ids that count as an RTL dongle for the "KrakenSDR = 4-5x RTL" heuristic.
_RTL_USB_IDS = ("0bda:2838", "0bda:2832")

# Kernel modules that grab RTL dongles for DVB/TV and prevent SDR use.
RTL_CONFLICT_MODULES = (
    "dvb_usb_rtl28xxu",
    "rtl2832",
    "rtl2830",
    "rtl8xxxu",
)


def parse_lsusb_devices(text: Optional[str]) -> Optional[List[Dict[str, Any]]]:
    """Parse `lsusb` stdout, filtering to known SDR USB ids. Returns a list of
    {usbId, name, count[, krakenLikely]} or None when text is None (lsusb
    unavailable). Pure/testable: takes the text, never runs a subprocess."""
    if text is None:
        return None
    low = text.lower()
    counts: Dict[str, int] = {}
    for vidpid in SDR_USB_NAMES:
        # lsusb prints "ID 0bda:2838 ..." once per attached device.
        n = low.count(" " + vidpid) + (
            1 if low.startswith(vidpid) else 0)
        # Robust fallback: count raw occurrences if the spaced form missed.
        if n == 0:
            n = low.count(vidpid)
        if n > 0:
            counts[vidpid] = n
    devices: List[Dict[str, Any]] = []
    rtl_total = 0
    for vidpid, n in counts.items():
        dev: Dict[str, Any] = {
            "usbId": vidpid,
            "name": SDR_USB_NAMES[vidpid],
            "count": n,
        }
        if vidpid in _RTL_USB_IDS:
            rtl_total += n
        devices.append(dev)
    # KrakenSDR presents as 4-5 identical RTL2838 (coherent channels).
    if rtl_total >= 4:
        for dev in devices:
            if dev["usbId"] in _RTL_USB_IDS:
                dev["krakenLikely"] = True
    return devices


def read_lsusb_text() -> Optional[str]:
    """Return `lsusb` stdout, or None if lsusb is missing/errors. Never raises,
    never claims a device (lsusb only enumerates the bus)."""
    try:
        import subprocess
        out = subprocess.run(
            ["lsusb"], capture_output=True, text=True, timeout=3,
        )
    except Exception:  # noqa: BLE001 - lsusb missing / errored → unavailable
        return None
    if out.returncode != 0 or not out.stdout:
        return None
    return out.stdout


def parse_module_conflicts(modules_text: Optional[str]) -> List[Dict[str, str]]:
    """Given /proc/modules text, return a list of loaded kernel modules that
    steal RTL dongles, each {module, hint}. Pure/testable. Never raises."""
    conflicts: List[Dict[str, str]] = []
    if not modules_text:
        return conflicts
    loaded = set()
    for line in modules_text.splitlines():
        name = line.split(" ", 1)[0].strip()
        if name:
            loaded.add(name)
    for mod in RTL_CONFLICT_MODULES:
        if mod in loaded:
            conflicts.append({
                "module": mod,
                "hint": ("DVB kernel module loaded \u2014 run the installer's "
                         "blacklist step or: sudo rmmod " + mod),
            })
    return conflicts


def read_proc_modules() -> Optional[str]:
    """Read /proc/modules without a subprocess. None if unreadable. Never
    raises."""
    try:
        with open("/proc/modules", "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def _parse_freq_hz(token: str) -> Optional[float]:
    """Parse an rtl_power-style frequency token ('400M', '100k', '928000000')
    into Hz. Returns None on garbage. Never raises."""
    if not isinstance(token, str):
        return None
    t = token.strip()
    if not t:
        return None
    mult = 1.0
    suffix = t[-1].lower()
    if suffix == "k":
        mult, t = 1e3, t[:-1]
    elif suffix == "m":
        mult, t = 1e6, t[:-1]
    elif suffix == "g":
        mult, t = 1e9, t[:-1]
    try:
        return float(t) * mult
    except ValueError:
        return None


def validate_sweep_range(rng: Any) -> Optional[str]:
    """Validate one rtl_power-style range string '<low>:<high>[:<step>]'.

    Returns a normalised range string on success, or None on any problem.
    Rules: low<high; both edges within SWEEP_FREQ_MIN_HZ..SWEEP_FREQ_MAX_HZ;
    step is optional but, if present, must parse and be >0. Never raises."""
    if not isinstance(rng, str):
        return None
    parts = rng.strip().split(":")
    if len(parts) not in (2, 3):
        return None
    low = _parse_freq_hz(parts[0])
    high = _parse_freq_hz(parts[1])
    if low is None or high is None:
        return None
    if not (SWEEP_FREQ_MIN_HZ <= low <= SWEEP_FREQ_MAX_HZ):
        return None
    if not (SWEEP_FREQ_MIN_HZ <= high <= SWEEP_FREQ_MAX_HZ):
        return None
    if low >= high:
        return None
    step_tok = None
    if len(parts) == 3:
        step = _parse_freq_hz(parts[2])
        if step is None or step <= 0:
            return None
        step_tok = parts[2].strip()
    # Re-emit the original tokens (trimmed) so the stored form is canonical.
    if step_tok is not None:
        return f"{parts[0].strip()}:{parts[1].strip()}:{step_tok}"
    return f"{parts[0].strip()}:{parts[1].strip()}"


def _median(values: List[float]) -> float:
    """Median of a non-empty list (pure stdlib; no numpy)."""
    s = sorted(values)
    n = len(s)
    mid = n // 2
    if n % 2:
        return s[mid]
    return (s[mid - 1] + s[mid]) / 2.0


def parse_sweep_csv_line(line: str) -> Optional[Dict[str, Any]]:
    """Parse one CSV line from rtl_power OR hackrf_sweep into a normalised dict.

    Both tools share the shape:
        date, time, hz_low, hz_high, hz_step, samples, db, db, ...
    (rtl_power writes 'YYYY-MM-DD, HH:MM:SS'; hackrf_sweep writes the same date
    /time columns). Returns:
        {'hz_low','hz_high','hz_step','samples','dbs':[...]}
    or None if the line is a comment / header / unparseable. Never raises."""
    if not isinstance(line, str):
        return None
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    parts = [p.strip() for p in line.split(",")]
    if len(parts) < 7:
        return None
    try:
        hz_low = float(parts[2])
        hz_high = float(parts[3])
        hz_step = float(parts[4])
        samples = float(parts[5])
    except ValueError:
        return None
    dbs: List[float] = []
    for p in parts[6:]:
        if not p:
            continue
        try:
            dbs.append(float(p))
        except ValueError:
            # hackrf_sweep never pads, rtl_power sometimes trails an empty
            # field; skip anything non-numeric rather than aborting the line.
            continue
    if not dbs:
        return None
    return {
        "hz_low": hz_low,
        "hz_high": hz_high,
        "hz_step": hz_step,
        "samples": samples,
        "dbs": dbs,
    }


def sweep_line_to_hits(parsed: Dict[str, Any], snr_threshold: float
                       ) -> List[Dict[str, Any]]:
    """Turn one parsed sweep row into zero or more power-hit descriptors.

    Noise floor = median of the row's dB bins. Any bin exceeding
    floor + snr_threshold is a hit. Returns a list of
    {'freq_hz','db','snr_db'} (bin center frequency). Never raises."""
    dbs = parsed.get("dbs") or []
    if not dbs:
        return []
    hz_low = float(parsed["hz_low"])
    hz_step = float(parsed["hz_step"])
    floor = _median(dbs)
    hits: List[Dict[str, Any]] = []
    for i, db in enumerate(dbs):
        snr = db - floor
        if snr < snr_threshold:
            continue
        # Bin center: low edge + (i + 0.5) * step.
        freq_hz = hz_low + (i + 0.5) * hz_step
        hits.append({"freq_hz": freq_hz, "db": db, "snr_db": snr})
    return hits


def sweep_hit_to_event_row(
    hit: Dict[str, Any],
    serial: int,
    node_id: str,
    source_label: str,
    lat: float,
    lon: float,
    heading: float,
    have_fix: bool,
    ts_unix: float,
) -> Dict[str, Any]:
    """Build a Kujhad 'hit' event row for a bare power detection.

    Mirrors doa_result_to_event_row's field names/types but with type 'hit'
    and detector 'sweep' — the coordinator's fleet ingest accepts 'hit'
    (KujhadFleetManager._kujhad_event_to_rf, _RF_EVENT_TYPES). No bearing:
    a plain sweep can't produce one. Includes freqHz / strengthDb / snrDb."""
    freq_hz = float(hit["freq_hz"])
    strength_db = float(hit["db"])
    snr_db = float(hit["snr_db"])
    time_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts_unix))
    event_id = str(uuid.uuid4())

    raw = {
        "freq_hz": freq_hz,
        "power_dbfs": strength_db,
        "snr_db": snr_db,
        "gps_lat": lat,
        "gps_lon": lon,
        "heading_deg": heading,
        "timestamp_unix": ts_unix,
        "node_id": node_id,
    }

    row = {
        "time": time_str,
        "eventId": event_id,
        "type": "hit",
        "frequency": freq_hz,
        "freqHz": freq_hz,
        "label": node_id or "SWEEP",
        "strengthDb": strength_db,
        "snrDb": snr_db,
        "detector": "sweep",
        "decoder": "SWEEP",
        "hitState": "auto",
        "protocol": "POWER",
        "networkId": node_id or "Unknown",
        "talkgroup": "Unknown",
        "radioId": "Unknown",
        "hasAudio": False,
        "hasData": False,
        "source": source_label,
        "gpsFix": bool(have_fix),
        "lat": lat,
        "lon": lon,
        "raw": raw,
        "serial": serial,
    }
    return row


def downsample_max(raw: List[float], target_bins: int) -> List[float]:
    """Downsample a dB array to <=target_bins using MAX bucketing so narrow
    peaks survive (averaging would smear narrowband emitters into the noise —
    same policy as the app, main_window.cpp:3226-3240). Never raises."""
    src = len(raw)
    if src == 0:
        return []
    n = target_bins
    if n < 1:
        n = 1
    if n > SPECTRUM_MAX_BINS:
        n = SPECTRUM_MAX_BINS
    if n > src:
        n = src
    out: List[float] = []
    step = src / float(n)
    for i in range(n):
        a = int(i * step)
        b = int((i + 1) * step)
        if b > src:
            b = src
        if b <= a:
            b = a + 1
        m = -math.inf
        for k in range(a, b):
            v = raw[k]
            if v > m:
                m = v
        if not math.isfinite(m):
            m = SPECTRUM_FLOOR_DB
        out.append(float(m))
    return out


def build_spectrum_frame(segments: List[Dict[str, Any]], serial: int,
                         ts_ms: int, hits: Optional[List[Dict[str, Any]]] = None,
                         search_bands: Optional[List[Dict[str, Any]]] = None,
                         target_bins: int = SPECTRUM_MAX_BINS
                         ) -> Dict[str, Any]:
    """Stitch the CSV segments of one completed sweep pass into a single Kujhad
    spectrum frame.

    `segments` are parse_sweep_csv_line() dicts ({hz_low,hz_high,hz_step,dbs}).
    They are sorted by hz_low and their dB bins concatenated in frequency order,
    then max-bucketed down to <=target_bins. centerFreq = pass midpoint,
    bandwidth = pass span, fftMin/MaxDb from the data with a small margin.
    Returns a frame dict ready to json.dump. Never raises."""
    segs = [s for s in (segments or []) if s and s.get("dbs")]
    segs.sort(key=lambda s: float(s.get("hz_low", 0.0)))
    raw: List[float] = []
    lo_hz = None
    hi_hz = None
    for s in segs:
        dbs = s.get("dbs") or []
        raw.extend(float(x) for x in dbs)
        sl = float(s.get("hz_low", 0.0))
        sh = float(s.get("hz_high", sl))
        lo_hz = sl if lo_hz is None else min(lo_hz, sl)
        hi_hz = sh if hi_hz is None else max(hi_hz, sh)
    if lo_hz is None:
        lo_hz = 0.0
    if hi_hz is None:
        hi_hz = 0.0
    bins = downsample_max(raw, target_bins)
    if bins:
        fft_min = min(bins) - SPECTRUM_DB_MARGIN
        fft_max = max(bins) + SPECTRUM_DB_MARGIN
    else:
        fft_min, fft_max = SPECTRUM_FLOOR_DB, 0.0
    return {
        "serial": serial,
        "tsMs": int(ts_ms),
        "centerFreq": (lo_hz + hi_hz) / 2.0,
        "bandwidth": max(0.0, hi_hz - lo_hz),
        "fftMinDb": fft_min,
        "fftMaxDb": fft_max,
        "bins": bins,
        "hits": hits or [],
        "searchBands": search_bands or [],
        "targets": [],
        "excludes": [],
    }


def keepalive_spectrum_frame(serial: int, ts_ms: int,
                             search_bands: Optional[List[Dict[str, Any]]] = None
                             ) -> Dict[str, Any]:
    """An empty-bins frame emitted every ~5s while the sweep is idle / has no
    hardware, so the app shows 'no data' rather than a dead socket."""
    return {
        "serial": serial,
        "tsMs": int(ts_ms),
        "centerFreq": 0.0,
        "bandwidth": 0.0,
        "fftMinDb": SPECTRUM_FLOOR_DB,
        "fftMaxDb": 0.0,
        "bins": [],
        "hits": [],
        "searchBands": search_bands or [],
        "targets": [],
        "excludes": [],
    }


class SpectrumHub:
    """Fan-out of spectrum frames to any number of concurrent NDJSON stream
    clients. Each client gets a bounded asyncio.Queue; when a slow client's
    queue is full the OLDEST frame is dropped so the ingest loop never blocks.
    A monotonic serial is stamped on every published frame."""

    def __init__(self) -> None:
        self._subs: "set[asyncio.Queue]" = set()
        self._serial = 0

    def next_serial(self) -> int:
        self._serial += 1
        return self._serial

    def subscribe(self) -> "asyncio.Queue":
        q: "asyncio.Queue" = asyncio.Queue(maxsize=SPECTRUM_CLIENT_QUEUE_MAX)
        self._subs.add(q)
        return q

    def unsubscribe(self, q: "asyncio.Queue") -> None:
        self._subs.discard(q)

    @property
    def client_count(self) -> int:
        return len(self._subs)

    def publish(self, frame: Dict[str, Any]) -> None:
        """Push a frame to every subscriber, dropping the oldest queued frame
        for any client whose queue is full (slow client). Never raises."""
        for q in list(self._subs):
            try:
                q.put_nowait(frame)
            except asyncio.QueueFull:
                try:
                    q.get_nowait()      # drop oldest
                except Exception:       # noqa: BLE001
                    pass
                try:
                    q.put_nowait(frame)
                except Exception:       # noqa: BLE001
                    pass


class SweepIngester:
    """SDR-agnostic power-sweep ingester for a plain RTL-SDR / HackRF.

    Auto-detects the dongle by USB id, shells out to rtl_power / hackrf_sweep,
    parses the streamed CSV, estimates a per-sweep noise floor, and appends a
    power-hit row to the SAME EventRing for every bin sticking up above the
    floor (rate-limited per frequency bucket, mirroring KrakenIngester).

    Defensive posture: the tool exiting (dongle yanked) is normal — it logs,
    waits, re-detects, and respawns. It never raises out of run()."""

    def __init__(self, ring: EventRing, pos: NodePosition, node_id: str,
                 source_label: str, ranges: List[str],
                 interval_s: int = SWEEP_DEFAULT_INTERVAL_S,
                 snr_threshold: float = SWEEP_DEFAULT_SNR_DB,
                 equip: Optional["NodeEquipment"] = None,
                 mode: str = "auto",
                 spectrum: Optional["SpectrumHub"] = None):
        self.ring = ring
        self.pos = pos
        self.node_id = node_id
        self.source_label = source_label
        self.equip = equip
        self.ranges = ranges or list(DEFAULT_SWEEP_RANGES)
        self.interval_s = interval_s
        self.snr_threshold = snr_threshold
        self.mode = mode if mode in SWEEP_MODES else "auto"
        self.spectrum = spectrum      # optional SpectrumHub for /v1/spectrum
        # Status (read by /v1/state):
        self.tool: Optional[str] = None
        self.usb_id: Optional[str] = None
        self.running = False          # a sweep subprocess is alive
        self.hardware_present = False
        self.hits_emitted = 0
        self.active_range: Optional[str] = None   # which range is scanning now
        self._proc: Optional[asyncio.subprocess.Process] = None
        self._rotate_index = 0        # round-robin cursor across self.ranges
        # Live-retask signal: set by retask() to tear down the current tool
        # process so the run loop relaunches with new ranges/interval/threshold.
        self._retask = asyncio.Event()
        # Debounce timer for coalescing rapid retask bursts (drag-to-tune).
        self._debounce_handle: Optional["asyncio.TimerHandle"] = None
        # Per frequency-bucket rate limiter (bucket width == bin step).
        self._last_bucket_emit: Dict[int, float] = {}
        # Spectrum pass accumulator: CSV segments of the current sweep pass,
        # keyed by hz_low. A pass completes when a segment we've already seen
        # reappears (rtl_power/hackrf_sweep loop back to the start).
        self._pass_segments: Dict[int, Dict[str, Any]] = {}
        # Operator-supplied band names keyed by normalized range string
        # (set via mission.setSearchBands); falls back to "sweep".
        self.band_names: Dict[str, str] = {}

    def retask(self, *, mode: Optional[str] = None,
               ranges: Optional[List[str]] = None,
               interval_s: Optional[int] = None,
               snr_threshold: Optional[float] = None) -> bool:
        """Apply new sweep settings live (from POST /v1/node-config). Updates
        the fields and, if anything that affects the running subprocess changed,
        raises the retask signal so run() tears down and relaunches with the new
        command. Returns True if a relaunch was signalled. Never raises."""
        changed = False
        relaunch = False
        if mode is not None and mode in SWEEP_MODES and mode != self.mode:
            self.mode = mode
            changed = True
            relaunch = True
        if ranges:
            new = list(ranges)
            if new != self.ranges:
                self.ranges = new
                self._rotate_index = 0
                # Names are keyed by range string; drop entries for ranges
                # that no longer exist so a later reconfiguration reusing a
                # different window never inherits a stale operator name.
                # (mission.setSearchBands rewrites the map right after this.)
                self.band_names = {r: n for r, n in self.band_names.items()
                                   if r in new}
                changed = True
                relaunch = True
        if interval_s is not None and int(interval_s) != self.interval_s:
            self.interval_s = int(interval_s)
            changed = True
            relaunch = True
        if snr_threshold is not None and float(snr_threshold) != self.snr_threshold:
            # SNR is applied per parsed line, so no relaunch needed for it.
            self.snr_threshold = float(snr_threshold)
            changed = True
        if changed:
            log.info("sweep: retasked to mode=%s ranges=%s interval=%ss snr=%.0fdB",
                     self.mode, ",".join(self.ranges), self.interval_s,
                     self.snr_threshold)
        if relaunch:
            self._schedule_relaunch()
        return relaunch

    def _schedule_relaunch(self) -> None:
        """Debounce the relaunch signal: a spectrum drag fires a tune.set per
        gesture update, and killing + respawning rtl_power for each one both
        lags the retune and hammers the dongle with inrush current (observed
        knocking marginal USB power offline). Coalesce bursts so only one
        relaunch happens once retasks go quiet for the debounce window. Falls
        back to an immediate signal when no event loop is running (tests /
        sync callers)."""
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            self._retask.set()
            return
        if self._debounce_handle is not None:
            self._debounce_handle.cancel()
        self._debounce_handle = loop.call_later(
            SWEEP_RETASK_DEBOUNCE_S, self._retask.set)

    def _next_range(self) -> str:
        """Return the current range for this dwell and advance the cursor.
        Round-robins across all configured ranges so every range is scanned."""
        if not self.ranges:
            self.ranges = list(DEFAULT_SWEEP_RANGES)
        idx = self._rotate_index % len(self.ranges)
        rng = self.ranges[idx]
        self._rotate_index = (idx + 1) % len(self.ranges)
        self.active_range = rng
        return rng

    def status(self) -> Dict[str, Any]:
        if self.running:
            state = "running"
        elif self.hardware_present:
            state = "stopped"
        else:
            state = "no-hardware"
        return {
            "tool": self.tool,
            "usbId": self.usb_id,
            "state": state,
            "mode": self.mode,
            "ranges": list(self.ranges),
            "activeRange": self.active_range,
            "intervalS": self.interval_s,
            "snrThresholdDb": self.snr_threshold,
            "hitsEmitted": self.hits_emitted,
        }

    def _should_emit(self, freq_hz: float, bucket_hz: float, now: float
                     ) -> bool:
        """Rate-limit per ~frequency bucket the way KrakenIngester rate-limits
        per bearing: at most one hit per bucket every SWEEP_MIN_EMIT_INTERVAL_S.
        Bucket width defaults to the bin step so adjacent bins of the same
        emitter don't each fire every sweep."""
        if bucket_hz <= 0:
            bucket_hz = 1.0
        bucket = int(freq_hz // bucket_hz)
        last = self._last_bucket_emit.get(bucket, 0.0)
        if (now - last) < SWEEP_MIN_EMIT_INTERVAL_S:
            return False
        self._last_bucket_emit[bucket] = now
        # Cheap unbounded-growth guard: prune stale buckets occasionally.
        if len(self._last_bucket_emit) > 4096:
            cutoff = now - SWEEP_MIN_EMIT_INTERVAL_S
            self._last_bucket_emit = {
                b: t for b, t in self._last_bucket_emit.items() if t >= cutoff
            }
        return True

    def _build_cmd(self, tool: str, rng: str) -> List[str]:
        """Build the subprocess argv for the detected tool for ONE range.
        rtl_power takes '-f low:high:binwidth'; hackrf_sweep takes
        '-f MHzlow:MHzhigh' (MHz-only). Neither tool accepts multiple ranges in
        one process, so the run loop round-robins: it launches one process per
        range for a dwell window, then relaunches with the next range."""
        if tool == "rtl_power":
            # rtl_power -f low:high:binwidth -i <int> -  (CSV → stdout)
            return ["rtl_power", "-f", rng,
                    "-i", str(self.interval_s), "-"]
        # hackrf_sweep -f MHzlow:MHzhigh  (CSV → stdout). Convert the range's
        # low/high edges to whole MHz.
        parts = rng.split(":")
        low_hz = _parse_freq_hz(parts[0]) if parts else None
        high_hz = _parse_freq_hz(parts[1]) if len(parts) > 1 else None
        if low_hz is None or high_hz is None:
            low_hz, high_hz = 400e6, 470e6
        low_mhz = int(low_hz // 1e6)
        high_mhz = int(high_hz // 1e6)
        if high_mhz <= low_mhz:
            high_mhz = low_mhz + 1
        return ["hackrf_sweep", "-f", f"{low_mhz}:{high_mhz}"]

    def _process_line(self, line: str) -> None:
        """Parse one CSV line and emit any hits (rate-limited), and accumulate
        the row into the current spectrum pass. Never raises."""
        parsed = parse_sweep_csv_line(line)
        if parsed is None:
            return
        bucket_hz = float(parsed.get("hz_step") or 0.0)
        now = time.time()
        for hit in sweep_line_to_hits(parsed, self.snr_threshold):
            if not self._should_emit(hit["freq_hz"], bucket_hz, now):
                continue
            serial = self.ring.next_serial()
            row = sweep_hit_to_event_row(
                hit, serial, self.node_id, self.source_label,
                self.pos.lat, self.pos.lon, self.pos.heading,
                self.pos.have_fix, now,
            )
            if self.equip is not None:
                self.equip.stamp(row)
            self.ring.append(row)
            self.hits_emitted += 1
        # Feed the spectrum accumulator (independent of the hit threshold).
        self._accumulate_spectrum(parsed, now)

    def _accumulate_spectrum(self, parsed: Dict[str, Any], now: float) -> None:
        """Collect CSV segments of the current sweep pass, keyed by hz_low. When
        a segment we've already collected reappears (the tool has looped back to
        the start of the range), flush the accumulated pass as one frame and
        start a new pass with this segment. Never raises."""
        if self.spectrum is None:
            return
        try:
            key = int(round(float(parsed.get("hz_low", 0.0))))
        except Exception:  # noqa: BLE001
            return
        if key in self._pass_segments and self._pass_segments:
            # Loop-back detected → the previous pass is complete. Flush it.
            self._flush_spectrum_pass(now)
        self._pass_segments[key] = parsed

    def _flush_spectrum_pass(self, now: float) -> None:
        """Build and publish a spectrum frame from the accumulated pass, then
        reset the accumulator. Never raises."""
        if self.spectrum is None or not self._pass_segments:
            self._pass_segments = {}
            return
        try:
            frame = build_spectrum_frame(
                list(self._pass_segments.values()),
                self.spectrum.next_serial(),
                int(now * 1000),
                hits=self.ring.recent_hits(10.0, now),
                search_bands=self._search_bands(),
            )
            self.spectrum.publish(frame)
        except Exception as e:  # noqa: BLE001 - spectrum is best-effort
            log.debug("sweep: spectrum flush failed (%s)", e)
        finally:
            self._pass_segments = {}

    def _search_bands(self) -> List[Dict[str, Any]]:
        """Configured sweep ranges as spectrum searchBands overlays."""
        bands: List[Dict[str, Any]] = []
        for rng in self.ranges:
            parts = str(rng).split(":")
            lo = _parse_freq_hz(parts[0]) if parts else None
            hi = _parse_freq_hz(parts[1]) if len(parts) > 1 else None
            if lo is None or hi is None:
                continue
            bands.append({"start": lo, "stop": hi, "enabled": True,
                          "name": self.band_names.get(str(rng), "sweep")})
        return bands

    async def _run_tool(self, tool: str, rng: str,
                        stop: asyncio.Event) -> None:
        """Spawn one sweep subprocess for ONE range and pump its stdout until it
        exits or stop is set. Never raises: subprocess errors are logged."""
        cmd = self._build_cmd(tool, rng)
        log.info("sweep: launching %s", " ".join(cmd))
        # A new range's pass starts fresh — never stitch bins across ranges.
        self._pass_segments = {}
        try:
            self._proc = await asyncio.create_subprocess_exec(
                *cmd,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
            )
        except FileNotFoundError:
            log.error("sweep: %s not installed; cannot sweep", tool)
            self._proc = None
            self.running = False
            return
        except Exception as e:  # noqa: BLE001 - never crash the service
            log.warning("sweep: failed to launch %s (%s)", tool, e)
            self._proc = None
            self.running = False
            return
        self.running = True
        assert self._proc.stdout is not None
        try:
            while not stop.is_set():
                try:
                    raw = await self._proc.stdout.readline()
                except (asyncio.CancelledError, GeneratorExit):
                    raise
                except Exception as e:  # noqa: BLE001
                    log.warning("sweep: read error (%s)", e)
                    break
                if not raw:
                    break  # tool exited (EOF)
                try:
                    line = raw.decode("utf-8", "replace")
                    self._process_line(line)
                except Exception as e:  # noqa: BLE001 - one bad line ≠ crash
                    log.debug("sweep: line parse failed (%s)", e)
        finally:
            self.running = False
            await self._terminate_proc()

    async def _terminate_proc(self) -> None:
        """Stop the sweep subprocess if it is still alive. Never raises."""
        proc = self._proc
        self._proc = None
        if proc is None:
            return
        try:
            if proc.returncode is None:
                proc.terminate()
                try:
                    await asyncio.wait_for(proc.wait(), timeout=5.0)
                except asyncio.TimeoutError:
                    proc.kill()
                    await proc.wait()
        except ProcessLookupError:
            pass
        except Exception as e:  # noqa: BLE001
            log.debug("sweep: terminate error (%s)", e)

    async def stop_now(self) -> None:
        """Externally stop the running subprocess (e.g. Kraken took over)."""
        self.running = False
        await self._terminate_proc()

    def _gate_open(self, gate: Optional["asyncio.Event"]) -> bool:
        """Whether the sweep is allowed to run right now, combining the live
        mode with the auto-gate. mode 'off' never runs; 'on' ignores the gate;
        'auto' honours the gate (paused while the Kraken WS is connected)."""
        if self.mode == "off":
            return False
        if self.mode == "on":
            return True
        # auto
        return gate is None or gate.is_set()

    async def run(self, stop: asyncio.Event,
                  gate: Optional["asyncio.Event"] = None) -> None:
        """Main loop: detect hardware, run one range at a time (round-robin),
        respawn on exit, and relaunch promptly on live retask.

        `gate`, when supplied, must be SET for the sweep to run in 'auto' mode
        (paused while the Kraken WS is connected). The live `self.mode`
        ('auto'/'on'/'off') gates on top of it. Never raises out of this loop."""
        while not stop.is_set():
            try:
                # Consume any pending retask so a fresh launch uses new settings.
                if self._retask.is_set():
                    self._retask.clear()

                if not self._gate_open(gate):
                    if self.running:
                        await self.stop_now()
                    self.active_range = None
                    await self._sleep_or_stop(stop, 2.0)
                    continue

                found = detect_sweep_tool()
                if found is None:
                    if self.hardware_present:
                        log.info("sweep: SDR removed; waiting for hardware")
                    self.hardware_present = False
                    self.tool = None
                    self.usb_id = None
                    self.active_range = None
                    await self._sleep_or_stop(stop, SWEEP_REDETECT_INTERVAL_S)
                    continue

                tool, usb_id = found
                if not self.hardware_present:
                    log.info("sweep: detected %s (%s)", tool, usb_id)
                self.hardware_present = True
                self.tool = tool
                self.usb_id = usb_id

                # Pick the next range in the rotation and run it for a dwell
                # window (or until the tool exits / stop / gate closes / retask).
                rng = self._next_range()
                run_task = asyncio.ensure_future(
                    self._run_tool(tool, rng, stop))
                dwell = SWEEP_ROTATE_DWELL_S if len(self.ranges) > 1 else None
                await self._await_tool_or_gate(run_task, stop, gate, dwell)

                if stop.is_set():
                    break
                if not self._gate_open(gate):
                    continue  # paused; loop will idle at the top
                if self._retask.is_set():
                    continue  # relaunch immediately with new settings
                # If there are multiple ranges we rotated intentionally: loop
                # straight on to the next range with no respawn delay. A single
                # range that ended means the tool exited (dongle hiccup) → wait.
                if len(self.ranges) > 1:
                    continue
                log.info("sweep: tool exited; re-detecting in %.0fs",
                         SWEEP_RESPAWN_DELAY_S)
                await self._sleep_or_stop(stop, SWEEP_RESPAWN_DELAY_S)
            except asyncio.CancelledError:
                await self.stop_now()
                raise
            except Exception as e:  # noqa: BLE001 - never crash the service
                log.warning("sweep: loop error (%s); retrying", e)
                await self.stop_now()
                await self._sleep_or_stop(stop, SWEEP_RESPAWN_DELAY_S)
        await self.stop_now()

    async def _await_tool_or_gate(self, run_task: "asyncio.Future",
                                  stop: asyncio.Event,
                                  gate: Optional["asyncio.Event"],
                                  dwell: Optional[float] = None) -> None:
        """Wait for the tool task to finish, or for stop / gate-close /
        rotation-dwell-elapsed / live-retask to require tearing it down early.
        Never raises."""
        deadline = (time.time() + dwell) if dwell else None
        while not run_task.done():
            waiters = [asyncio.ensure_future(stop.wait()),
                       asyncio.ensure_future(self._retask.wait())]
            done, pending = await asyncio.wait(
                {run_task, *waiters},
                timeout=1.0,
                return_when=asyncio.FIRST_COMPLETED,
            )
            for w in waiters:
                w.cancel()
            teardown = (stop.is_set()
                        or not self._gate_open(gate)
                        or self._retask.is_set()
                        or (deadline is not None and time.time() >= deadline))
            if teardown:
                if not run_task.done():
                    run_task.cancel()
                    try:
                        await run_task
                    except asyncio.CancelledError:
                        pass
                    except Exception:  # noqa: BLE001
                        pass
                await self.stop_now()
                return
        # Surface (but never re-raise) a tool-task exception.
        try:
            exc = run_task.exception()
            if exc is not None:
                log.warning("sweep: tool task error (%r)", exc)
        except (asyncio.CancelledError, asyncio.InvalidStateError):
            pass

    async def _sleep_or_stop(self, stop: asyncio.Event, timeout: float) -> None:
        try:
            await asyncio.wait_for(stop.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass


# ── Node setup portal HTML (mirrors kujhadNodeSetupHtml in kujhad_fleet.h) ───


def node_setup_html() -> str:
    """Self-contained node commissioning page served at GET /setup.

    No external assets. Sections mirror the C++ portal: API key, pairing,
    position/gpsd, SDR profile select, antenna curve point editor with presets,
    terrain select, siting select, and a live preview line (calDb at a probe
    frequency + path-loss exponent n + RSSI sigma). Option DOM is built with
    textContent (never innerHTML) so user/config data can't inject markup.

    The static option tables (SDR/preset/terrain/siting, incl. the numeric
    offsets/exponents/sigmas the live preview needs) are server-rendered into
    the page as a JSON blob so all selects populate and the preview works with
    ZERO authed /v1 calls. Only "Load current"/"Save"/pairing hit the authed
    API; a 401 there is reported next to the buttons and never blanks the
    dropdowns. The tables are the single source of truth (json.dumps of the
    Python lists) — no numbers are duplicated in JS."""
    tables_json = json.dumps({
        "sdrOptions": SDR_PROFILES,
        "antennaPresets": ANTENNA_PRESETS,
        "terrainOptions": TERRAIN_PROFILES,
        "sitingOptions": SITING_PROFILES,
        "baseRssiSigmaDb": BASE_RSSI_SIGMA_DB,
        "sweepModes": list(SWEEP_MODES),
    })
    return (
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<title>Predator RF \u2014 Node Setup</title>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>"
"body{background:#05080a;color:#c8d8e0;font-family:'JetBrains Mono',Consolas,monospace;font-size:13px;margin:0;padding:14px;max-width:560px}"
"h1{color:#3fd17d;letter-spacing:.18em;font-size:13px;margin:0 0 12px;text-transform:uppercase}"
"h2{color:#4ad8e8;font-size:12px;text-transform:uppercase;letter-spacing:.1em;margin:18px 0 6px;border-bottom:1px solid #1f3540;padding-bottom:3px}"
"input,button,select{background:#0f171c;color:#c8d8e0;border:1px solid #2a4a5a;padding:6px 8px;font-family:inherit;font-size:12px;box-sizing:border-box}"
"input,select{width:100%}"
"button{cursor:pointer;color:#3fd17d;width:auto}"
"button:hover{border-color:#3fd17d}"
"label{display:block;color:#7a95a3;margin:8px 0 2px;font-size:11px;text-transform:uppercase;letter-spacing:.06em}"
".row{display:flex;gap:8px}.row>div{flex:1}"
"#msg{margin-top:10px;min-height:16px}.ok{color:#3fd17d}.err{color:#e86a5a}"
"pre{background:#0f171c;border:1px solid #2a4a5a;padding:8px;white-space:pre-wrap;word-break:break-all;font-size:11px}"
".hint{color:#5a7482;font-size:11px;margin-top:2px}"
"</style></head><body>"
"<h1>Predator RF \u2014 Node Setup</h1>"
"<label>API key</label><input id=key type=password placeholder='from this node config (api_key)'>"
"<div class=hint>Stored only in this browser. Find it in this node's df_kracked_sensor.json.</div>"
"<h2>Pairing</h2>"
"<button onclick='loadPairing()'>Show peer code</button> <button onclick='copyPairing()'>Copy</button>"
"<pre id=pairing>\u2014</pre>"
"<div class=hint>Paste this JSON into the controller's manual-pair form.</div>"
"<h2>Position</h2>"
"<div class=row><div><label>Latitude</label><input id=lat type=number step=any></div>"
"<div><label>Longitude</label><input id=lon type=number step=any></div></div>"
"<label><input id=gpsd type=checkbox style='width:auto'> Pull live position from gpsd (USB GPS dongle on this box)</label>"
"<div class=hint>With gpsd enabled, a live fix overrides the static coordinates; the static values remain the fallback.</div>"
"<h2>Hardware</h2>"
"<button onclick='loadHw()'>Refresh</button>"
"<div id=hwdevices class=hint>probing\u2026</div>"
"<div id=hwconflicts></div>"
"<div id=hwactivity class=hint></div>"
"<h2>Sweep</h2>"
"<div class=hint>What this node's plain SDR scans for power hits. Saved changes apply live \u2014 no restart. All configured ranges are scanned round-robin.</div>"
"<label>Mode</label><select id=sweepmode></select>"
"<div class=hint>auto = sweep only when the Kraken feed is offline; on = always sweep; off = never sweep.</div>"
"<label>Ranges (low MHz / high MHz / step kHz)</label>"
"<div id=sweepranges></div>"
"<p><button onclick='addRange(400,470,100)'>+ Range</button>"
" <button onclick='addRange(400,470,100)'>UHF 400-470</button>"
" <button onclick='addRange(902,928,100)'>US 900 ISM 902-928</button>"
" <button onclick='addRange(144,148,50)'>2 m VHF 144-148</button>"
" <button onclick='addRange(118,137,50)'>Air 118-137</button></p>"
"<div class=row><div><label>Integration (s)</label><input id=sweepint type=number step=1 min=1 max=60 value=5></div>"
"<div><label>SNR threshold (dB)</label><input id=sweepsnr type=number step=1 min=3 max=40 value=12></div></div>"
"<div class=hint>Integration 1-60 s; SNR 3-40 dB above the estimated noise floor.</div>"
"<h2>Equipment</h2>"
"<label>SDR attached</label><select id=sdr></select>"
"<div id=sdrhint class=hint></div>"
"<label>Antenna gain curve (dB at MHz)</label>"
"<div class=hint>Gain is band-dependent: a 5 dB GMRS whip is NOT 5 dB at 900 MHz ISM. Add one point per band you hunt; hits are corrected at their own frequency (log-f interpolation between points, nearest point held beyond the ends).</div>"
"<div id=points></div>"
"<p><select id=antpreset style='width:60%'></select> <button onclick='addPreset()'>Add preset point</button> <button onclick='addPoint(400,0)'>+ Blank point</button></p>"
"<h2>Environment</h2>"
"<label>Terrain around this node</label><select id=terrain></select>"
"<div class=hint>Sets how fast signal decays with distance in the ranging math (path-loss exponent). Dense city or forest kills signal much faster than open ground.</div>"
"<label>Antenna placement</label><select id=siting></select>"
"<div class=hint>Mounting bias: body-worn or ground-level antennas read low and erratically; rooftop or treetop read high. Corrected and de-weighted accordingly.</div>"
"<label>Preview correction at (MHz)</label><input id=prevf type=number step=any value=465>"
"<div class=hint>Net RSSI correction at that frequency: <span id=cal>0.0</span> dB (SDR offset + interpolated antenna gain + placement bias). Path-loss n: <span id=plexp>3.0</span>, RSSI trust sigma: <span id=psig>6.0</span> dB.</div>"
"<p><button onclick='loadCfg()'>Load current</button> <button onclick='saveCfg()'>Save</button></p>"
"<div id=msg></div>"
"<script>"
"const $=id=>document.getElementById(id);"
"$('key').value=localStorage.getItem('kujhadKey')||'';"
"$('key').addEventListener('change',()=>localStorage.setItem('kujhadKey',$('key').value));"
"function hdrs(){return{'X-Kujhad-Key':$('key').value,'Content-Type':'application/json'}}"
"function msg(t,ok){const m=$('msg');m.textContent=t;m.className=ok?'ok':'err'}"
"const TABLES=" + tables_json + ";"
"const sdrOptions=TABLES.sdrOptions;const presets=TABLES.antennaPresets;"
"const terrainOptions=TABLES.terrainOptions;const sitingOptions=TABLES.sitingOptions;"
"const BASE_SIGMA=TABLES.baseRssiSigmaDb;"
"function curvePoints(){const out=[];for(const row of $('points').children){"
"const f=parseFloat(row.children[0].value),g=parseFloat(row.children[1].value);"
"if(isFinite(f)&&f>0&&isFinite(g))out.push({f:f,g:g})}return out}"
"function gainAt(pts,fMhz){if(!pts.length)return 0;const s=pts.slice().sort((a,b)=>a.f-b.f);"
"if(fMhz<=s[0].f)return s[0].g;if(fMhz>=s[s.length-1].f)return s[s.length-1].g;"
"for(let i=1;i<s.length;i++){if(fMhz<=s[i].f){const f0=Math.log10(s[i-1].f),f1=Math.log10(s[i].f);"
"const t=f1>f0?(Math.log10(fMhz)-f0)/(f1-f0):0;return s[i-1].g+t*(s[i].g-s[i-1].g)}}return s[s.length-1].g}"
"function recalc(){const o=sdrOptions.find(s=>s.id===$('sdr').value);"
"const f=parseFloat($('prevf').value);"
"const a=(isFinite(f)&&f>0)?gainAt(curvePoints(),f):0;"
"const t=terrainOptions.find(x=>x.id===$('terrain').value);"
"const st=sitingOptions.find(x=>x.id===$('siting').value);"
"const c=(o?o.offsetDb:0)+a+(st?st.offsetDb:0);$('cal').textContent=c.toFixed(1);"
"$('plexp').textContent=(t?t.exponent:3).toFixed(1);"
"$('psig').textContent=(BASE_SIGMA+(st?st.sigmaExtraDb:0)).toFixed(1)}"
"function populateSelects(){"
"$('sdr').replaceChildren(...sdrOptions.map(s=>{const o=document.createElement('option');"
"o.value=s.id;o.textContent=s.label+' ('+(s.offsetDb>=0?'+':'')+s.offsetDb+' dB)';return o}));"
"$('antpreset').replaceChildren(...presets.map((p,i)=>{const o=document.createElement('option');"
"o.value=String(i);o.textContent=p.label;return o}));"
"$('terrain').replaceChildren(...terrainOptions.map(t=>{const o=document.createElement('option');"
"o.value=t.id;o.textContent=t.label+' (n='+t.exponent+')';return o}));"
"$('siting').replaceChildren(...sitingOptions.map(s=>{const o=document.createElement('option');"
"o.value=s.id;o.textContent=s.label+' ('+(s.offsetDb>=0?'+':'')+s.offsetDb+' dB)';return o}));"
"$('sweepmode').replaceChildren(...(TABLES.sweepModes||[]).map(m=>{const o=document.createElement('option');"
"o.value=m;o.textContent=m;return o}));"
"$('sdr').value='unknown';$('terrain').value='mixed';$('siting').value='mast';$('sweepmode').value='auto';}"
"function fmtMhz(hz){const m=hz/1e6;return(Math.round(m*1000)/1000)+'M'}"
"function fmtKhz(hz){return(Math.round(hz/1e3))+'k'}"
"function addRange(loMhz,hiMhz,stepKhz){if($('sweepranges').children.length>=8)return;"
"const row=document.createElement('div');row.className='row';"
"const lo=document.createElement('input');lo.type='number';lo.step='any';lo.value=loMhz;lo.placeholder='low MHz';"
"const hi=document.createElement('input');hi.type='number';hi.step='any';hi.value=hiMhz;hi.placeholder='high MHz';"
"const st=document.createElement('input');st.type='number';st.step='any';st.value=stepKhz;st.placeholder='step kHz';"
"const del=document.createElement('button');del.textContent='X';del.onclick=()=>row.remove();"
"row.append(lo,hi,st,del);$('sweepranges').appendChild(row)}"
"function sweepRangeStrings(){const out=[];for(const row of $('sweepranges').children){"
"const lo=parseFloat(row.children[0].value),hi=parseFloat(row.children[1].value),st=parseFloat(row.children[2].value);"
"if(!(isFinite(lo)&&isFinite(hi)))continue;"
"let s=fmtMhz(lo*1e6)+':'+fmtMhz(hi*1e6);if(isFinite(st)&&st>0)s+=':'+fmtKhz(st*1e3);out.push(s)}return out}"
"function parseTok(t){if(!t)return null;t=String(t).trim();let m=1;const c=t.slice(-1).toLowerCase();"
"if(c==='k'){m=1e3;t=t.slice(0,-1)}else if(c==='m'){m=1e6;t=t.slice(0,-1)}else if(c==='g'){m=1e9;t=t.slice(0,-1)}"
"const v=parseFloat(t);return isFinite(v)?v*m:null}"
"function loadRanges(arr){$('sweepranges').replaceChildren();"
"for(const s of (arr||[])){const p=String(s).split(':');const lo=parseTok(p[0]),hi=parseTok(p[1]),st=p.length>2?parseTok(p[2]):null;"
"if(lo!=null&&hi!=null)addRange(lo/1e6,hi/1e6,st!=null?st/1e3:100)}}"
"function addPoint(f,g){if($('points').children.length>=16)return;"
"const row=document.createElement('div');row.className='row';"
"const fi=document.createElement('input');fi.type='number';fi.step='any';fi.value=f;fi.placeholder='MHz';"
"const gi=document.createElement('input');gi.type='number';gi.step='0.5';gi.value=g;gi.placeholder='dB';"
"const del=document.createElement('button');del.textContent='X';del.onclick=()=>{row.remove();recalc()};"
"fi.addEventListener('input',recalc);gi.addEventListener('input',recalc);"
"row.append(fi,gi,del);$('points').appendChild(row);recalc()}"
"function addPreset(){const p=presets[parseInt($('antpreset').value)];if(p)addPoint(p.freqMhz,p.gainDb)}"
"async function loadCfg(){if(!$('key').value){msg('Enter API key and press Load current.',false);return}"
"try{const r=await fetch('/v1/node-config',{headers:hdrs()});"
"if(r.status===401)throw new Error('key rejected');"
"if(!r.ok)throw new Error('HTTP '+r.status);const j=await r.json();"
"$('lat').value=(j.lat!=null?j.lat:'');$('lon').value=(j.lon!=null?j.lon:'');"
"$('gpsd').checked=!!j.gpsdEnabled;"
"$('sdr').value=j.sdrType||'unknown';"
"$('terrain').value=j.terrain||'mixed';$('siting').value=j.siting||'mast';"
"$('points').replaceChildren();"
"for(const p of (j.antennaCurve||[]))addPoint(p.f,p.g);"
"$('sweepmode').value=j.sweepMode||'auto';"
"loadRanges(j.sweepRanges);"
"if(j.sweepIntervalS!=null)$('sweepint').value=j.sweepIntervalS;"
"if(j.sweepSnrDb!=null)$('sweepsnr').value=j.sweepSnrDb;"
"recalc();"
"msg('Loaded.'+(j.gpsdFix?' gpsd fix: '+j.gpsdLat.toFixed(5)+', '+j.gpsdLon.toFixed(5):''),true)}"
"catch(e){msg('Load failed: '+e.message,false)}}"
"$('sdr').addEventListener('change',recalc);$('prevf').addEventListener('input',recalc);"
"$('terrain').addEventListener('change',recalc);$('siting').addEventListener('change',recalc);"
"async function saveCfg(){if(!$('key').value){msg('Enter API key first.',false);return}"
"try{const body={lat:parseFloat($('lat').value)||0,lon:parseFloat($('lon').value)||0,"
"gpsdEnabled:$('gpsd').checked,sdrType:$('sdr').value,antennaCurve:curvePoints(),"
"terrain:$('terrain').value,siting:$('siting').value,"
"sweepMode:$('sweepmode').value,sweepRanges:sweepRangeStrings(),"
"sweepIntervalS:parseInt($('sweepint').value),sweepSnrDb:parseFloat($('sweepsnr').value)};"
"const r=await fetch('/v1/node-config',{method:'POST',headers:hdrs(),body:JSON.stringify(body)});"
"if(r.status===401)throw new Error('key rejected');"
"const j=await r.json();if(!r.ok||j.error)throw new Error(j.error||('HTTP '+r.status));"
"msg('Saved. Sweep retasked; hits carry the correction.',true);loadHw()}"
"catch(e){msg('Save failed: '+e.message,false)}}"
"async function loadPairing(){if(!$('key').value){msg('Enter API key first.',false);return}"
"try{const r=await fetch('/v1/pairing',{headers:hdrs()});"
"if(r.status===401)throw new Error('key rejected');"
"if(!r.ok)throw new Error('HTTP '+r.status);const j=await r.json();"
"$('pairing').textContent=JSON.stringify(j);msg('Pairing code loaded.',true)}"
"catch(e){msg('Pairing failed: '+e.message,false)}}"
"function copyPairing(){const t=$('pairing').textContent;if(t&&t!=='\\u2014'){navigator.clipboard&&navigator.clipboard.writeText(t);msg('Copied.',true)}}"
"const USB2PROFILE={'1d50:6089':'hackrf','1d50:60a1':'airspy_mini','0bda:2838':'rtlsdr_v3','0bda:2832':'rtlsdr_v3'};"
"function suggestSdr(devices){const h=$('sdrhint');h.textContent='';"
"if(!Array.isArray(devices))return;"
"if($('sdr').value&&$('sdr').value!=='unknown')return;"
"const cands=[];for(const d of devices){if(d.krakenLikely){cands.push(['kraken','KrakenSDR']);continue}"
"const p=USB2PROFILE[d.usbId];if(p)cands.push([p,d.name])}"
"if(cands.length!==1)return;const[pid,nm]=cands[0];"
"const opt=sdrOptions.find(s=>s.id===pid);"
"if(pid==='kraken'){h.textContent='Detected: KrakenSDR (4+ RTL channels) \\u2014 this node should run the Kraken feed.';return}"
"if(opt)h.textContent='Detected: '+nm+\" \\u2014 select '\"+opt.label+\"' profile?\"}"
"function renderHw(j){"
"const dv=$('hwdevices');"
"if(j.devices===null){dv.textContent=j.note||'lsusb unavailable.';dv.className='hint'}"
"else if(!j.devices.length){dv.textContent='no SDR detected \\u2014 plug one in, the sensor re-probes every 60 s.';dv.className='hint'}"
"else{const parts=j.devices.map(d=>d.name+' ('+d.usbId+(d.count>1?' x'+d.count:'')+(d.krakenLikely?', KrakenSDR-like':'')+')');"
"dv.textContent='Detected: '+parts.join('; ');dv.className='hint'}"
"const cf=$('hwconflicts');cf.replaceChildren();"
"for(const c of (j.conflicts||[])){const p=document.createElement('div');p.className='err';"
"p.textContent='\\u26a0 '+c.module+' loaded \\u2014 '+c.hint;cf.appendChild(p)}"
"const act=$('hwactivity');let line='idle: no active RF capture';"
"const sw=j.sweep;"
"if(sw&&sw.state==='running'){line='sweeping '+((sw.ranges||[]).join(', '))+' via '+sw.tool+', '+sw.hitsEmitted+' hits'}"
"else if(j.kraken&&j.kraken.connected){line='Kraken connected \\u2014 bearings'}"
"else if(sw&&sw.state==='stopped'){line='sweep idle (hardware present, not sweeping)'}"
"else if(sw&&sw.state==='no-hardware'){line='idle: no hardware'}"
"act.textContent='Activity: '+line;"
"suggestSdr(j.devices)}"
"async function loadHw(){try{const r=await fetch('/v1/hardware');"
"if(!r.ok)throw new Error('HTTP '+r.status);renderHw(await r.json())}"
"catch(e){$('hwdevices').textContent='Hardware probe failed: '+e.message;$('hwdevices').className='err'}}"
"populateSelects();recalc();loadHw();"
"if($('key').value){loadCfg()}else{msg('Enter API key and press Load current to fetch this node\\u2019s saved config.',false)}"
"</script></body></html>")


# ── Kujhad v1 HTTP server (aiohttp) ─────────────────────────────────────────


class KujhadSensorApp:
    def __init__(self, api_key: str, device_name: str, ring: EventRing,
                 pos: NodePosition, ingester: Optional[KrakenIngester],
                 advertise: str = "",
                 sweep: Optional["SweepIngester"] = None,
                 equip: Optional["NodeEquipment"] = None,
                 config_path: str = "",
                 bind: str = "0.0.0.0", port: int = DEFAULT_PORT,
                 gpsd_enabled: bool = False,
                 spectrum: Optional["SpectrumHub"] = None):
        self.api_key = api_key
        self.device_name = device_name
        self.ring = ring
        self.pos = pos
        self.ingester = ingester
        self.advertise = advertise
        self.sweep = sweep
        self.equip = equip if equip is not None else NodeEquipment()
        self.config_path = config_path
        self.bind = bind
        self.port = port
        self.gpsd_enabled = gpsd_enabled
        # Spectrum fan-out hub (shared with the SweepIngester). If the sweep
        # ingester carries one, reuse it so its published frames reach clients.
        if spectrum is not None:
            self.spectrum = spectrum
        elif sweep is not None and getattr(sweep, "spectrum", None) is not None:
            self.spectrum = sweep.spectrum
        else:
            self.spectrum = SpectrumHub()
        # Pre-tune snapshot so a tune.set can be undone by scan.start.
        self._pretune_ranges: Optional[List[str]] = None
        self._pretune_mode: Optional[str] = None
        # Operator band names from mission.setSearchBands, keyed by the
        # normalized sweep range each band mapped to, so /v1/state and the
        # spectrum overlay can echo the controller's names back instead of
        # the generic "sweep" label.
        self._band_names: Dict[str, str] = {}
        # Last command applied (for /v1/state + Hardware activity visibility).
        self.last_command: Optional[str] = None

    # -- auth --
    def _authorized(self, request: "web.Request") -> bool:
        # Header name is X-Kujhad-Key (case-insensitive); aiohttp headers
        # are already case-insensitive.
        key = request.headers.get("X-Kujhad-Key")
        if not self.api_key:
            return False
        return key == self.api_key

    @staticmethod
    def _unauthorized() -> "web.Response":
        return web.json_response({"error": "unauthorized"}, status=401)

    # -- handlers --
    async def handle_identify(self, request: "web.Request") -> "web.Response":
        if not self._authorized(request):
            return self._unauthorized()
        body = {
            "device": self.device_name,
            "version": "DF-Kracked Sensor 1.0 (Kujhad v1)",
            "role": "sensor",
            "api": 1,
            "rxOnly": True,
            "advertise": self.advertise,
            "hwProfile": {
                "source": "KrakenSDR",
                "decoder": "KRAKEN_LOB",
                "remoteFoxHunt": False,
            },
            "remoteFoxHunt": False,
        }
        return web.json_response(body)

    async def handle_state(self, request: "web.Request") -> "web.Response":
        if not self._authorized(request):
            return self._unauthorized()
        connected = bool(self.ingester and self.ingester.connected)
        sweep_status = self.sweep.status() if self.sweep else None
        sweep_running = bool(sweep_status
                             and sweep_status.get("state") == "running")
        online = connected or sweep_running
        if connected:
            scan_status = "KRAKEN_LOB sensor online"
        elif sweep_running:
            scan_status = ("sweep active (%s)"
                           % (sweep_status.get("tool") or "sdr"))
        else:
            scan_status = "waiting for SDR / Kraken DoA feed"
        # Minimal-but-valid mission shape so a sensor-only node does not
        # break the controller's Mission UI. Empty arrays are fine.
        body = {
            "centerFreq": 0.0,
            "playing": online,
            "missionMode": 0,
            "scanRunning": online,
            "scanStatus": scan_status,
            "scanPaused": False,
            # Echo the sweep's configured ranges (with any operator names
            # from mission.setSearchBands) so a controller's Mission tab
            # shows this node's bands instead of a blank list — and so a
            # just-sent setSearchBands visibly round-trips.
            "searchBands": (self.sweep._search_bands()
                            if self.sweep else []),
            "targets": [],
            "excludes": [],
            "hits": [],
            "thresholdDb": 0.0,
            "dwellMs": 0,
            "quickScanDelayMs": 0,
            "quickScanDurationMs": 0,
            "recordAudio": False,
            # Sensor-specific status blocks (ignored by the mission UI but
            # available to operators / diagnostics).
            "kraken": {"connected": connected},
            "sweep": sweep_status,
            # Equipment calibration summary (sdr id, curve point count,
            # terrain, siting) so a controller/operator can see how this
            # node's hits are corrected.
            "equipment": self.equip.summary(),
            # Command / spectrum visibility.
            "lastCommand": self.last_command,
            "spectrumClients": self.spectrum.client_count,
        }
        return web.json_response(body)

    # NOTE: /v1/gps serves self.pos, which the ingester keeps in sync with
    # the Kraken's own per-frame GPS (see handle_doa_msg) so phones show a
    # fix without needing --lat/--lon or gpsd on the Pi.
    async def handle_gps(self, request: "web.Request") -> "web.Response":
        if not self._authorized(request):
            return self._unauthorized()
        return web.json_response(self.pos.snapshot())

    async def handle_events(self, request: "web.Request") -> "web.Response":
        if not self._authorized(request):
            return self._unauthorized()
        since_raw = request.query.get("since", "0")
        try:
            since = int(since_raw)
        except (ValueError, TypeError):
            since = 0
        if since < 0:
            since = 0
        events, last_id = self.ring.since(since)
        return web.json_response({"events": events, "lastId": last_id})

    async def handle_spectrum(self, request: "web.Request") -> "web.Response":
        """Authed long-lived chunked NDJSON stream of spectrum frames.

        One JSON object per line per frame. Frames are produced by the sweep
        ingester (one per completed sweep pass); when the sweep is idle / has no
        hardware a keepalive frame (empty bins) is sent every ~5s so the app
        shows 'no data' rather than a dead socket. Each client has a bounded
        queue (drop-oldest) so a slow client never blocks the ingest loop.
        Multiple concurrent clients are supported."""
        if not self._authorized(request):
            return self._unauthorized()
        resp = web.StreamResponse(status=200, headers={
            "Content-Type": "application/x-ndjson",
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Access-Control-Allow-Origin": "*",
        })
        await resp.prepare(request)
        q = self.spectrum.subscribe()
        try:
            while True:
                try:
                    frame = await asyncio.wait_for(
                        q.get(), timeout=SPECTRUM_KEEPALIVE_S)
                except asyncio.TimeoutError:
                    # No frame within the keepalive window → the sweep is idle.
                    frame = keepalive_spectrum_frame(
                        self.spectrum.next_serial(),
                        int(time.time() * 1000),
                        search_bands=(self.sweep._search_bands()
                                      if self.sweep else []),
                    )
                line = (json.dumps(frame) + "\n").encode("utf-8")
                await resp.write(line)
        except (asyncio.CancelledError, ConnectionResetError):
            pass
        except Exception as e:  # noqa: BLE001 - client dropped / write error
            log.debug("spectrum: stream ended (%s)", e)
        finally:
            self.spectrum.unsubscribe(q)
            try:
                await resp.write_eof()
            except Exception:  # noqa: BLE001
                pass
        return resp

    async def handle_command(self, request: "web.Request") -> "web.Response":
        """Authed command endpoint. Body {class, action, args}. RX-only: any
        tx.* class (and foxbeacon) is rejected. Implemented: identify, tune.set,
        scan.start/stop, mission.setSearchBands. Everything else → ok:false."""
        if not self._authorized(request):
            return self._unauthorized()
        try:
            raw = await request.text()
            body = json.loads(raw) if raw.strip() else {}
        except (ValueError, TypeError):
            return web.json_response(
                {"ok": False, "error": "invalid JSON body"}, status=400)
        if not isinstance(body, dict):
            return web.json_response(
                {"ok": False, "error": "body must be a JSON object"},
                status=400)
        cls = str(body.get("class", ""))
        action = str(body.get("action", ""))
        args = body.get("args") if isinstance(body.get("args"), dict) else {}

        # Hard RX-only gate: never implement tx.*/foxbeacon on this sensor.
        if cls == "tx" or cls.startswith("tx.") or cls == "foxbeacon":
            log.info("command rejected (tx/foxbeacon): %s.%s", cls, action)
            return web.json_response(
                {"ok": False, "error": "tx commands disabled (RX-only build)"},
                status=403)

        ok, err = self._dispatch_command(cls, action, args)
        if ok:
            self.last_command = f"{cls}.{action}"
            log.info("command applied: %s.%s", cls, action)
            return web.json_response({"ok": True})
        log.info("command rejected: %s.%s (%s)", cls, action, err)
        status = 400 if err == "unsupported on this sensor" else 200
        return web.json_response({"ok": False, "error": err}, status=status)

    def _dispatch_command(self, cls: str, action: str,
                          args: Dict[str, Any]) -> Tuple[bool, str]:
        """Apply one command. Returns (ok, error). Never raises."""
        try:
            if cls == "identify":
                return True, ""

            if cls == "tune" and action == "set":
                if self.sweep is None:
                    return False, "sweep unavailable on this node"
                freq = _num(args.get("frequencyHz"), 0.0)
                if freq <= 0:
                    return False, "frequencyHz required"
                if not (SWEEP_FREQ_MIN_HZ <= freq <= SWEEP_FREQ_MAX_HZ):
                    return False, "frequencyHz out of hardware range"
                half = _num(args.get("halfSpanHz"), TUNE_DEFAULT_HALF_SPAN_HZ)
                if half <= 0:
                    half = TUNE_DEFAULT_HALF_SPAN_HZ
                low = max(SWEEP_FREQ_MIN_HZ, freq - half)
                high = min(SWEEP_FREQ_MAX_HZ, freq + half)
                if high <= low:
                    return False, "tune window collapses at hardware edge"
                rng = "%d:%d:%d" % (int(low), int(high), TUNE_STEP_HZ)
                if validate_sweep_range(rng) is None:
                    return False, "computed tune range invalid"
                # Remember pre-tune config so scan.start can restore it.
                if self._pretune_ranges is None:
                    self._pretune_ranges = list(self.sweep.ranges)
                    self._pretune_mode = self.sweep.mode
                self.sweep.retask(mode="on", ranges=[rng])
                self._persist_config()
                return True, ""

            if cls == "scan" and action == "start":
                if self.sweep is None:
                    return False, "sweep unavailable on this node"
                # Restore the configured ranges/mode if a manual tune is active.
                if self._pretune_ranges is not None:
                    self.sweep.retask(
                        mode=self._pretune_mode or "auto",
                        ranges=self._pretune_ranges)
                    self._pretune_ranges = None
                    self._pretune_mode = None
                else:
                    # No tune pending: just ensure the sweep is running.
                    if self.sweep.mode == "off":
                        self.sweep.retask(mode="auto")
                self._persist_config()
                return True, ""

            if cls == "scan" and action == "stop":
                if self.sweep is None:
                    return False, "sweep unavailable on this node"
                self.sweep.retask(mode="off")
                self._persist_config()
                return True, ""

            if cls == "mission" and action == "setSearchBands":
                if self.sweep is None:
                    return False, "sweep unavailable on this node"
                bands = args.get("bands")
                if not isinstance(bands, list):
                    return False, "bands array required"
                ranges: List[str] = []
                for b in bands:
                    if not isinstance(b, dict):
                        return False, "band entries must be objects"
                    if not b.get("enabled", True):
                        continue
                    start = _num(b.get("start"), 0.0)
                    stop = _num(b.get("stop"), 0.0)
                    rng = "%d:%d:%d" % (int(start), int(stop), TUNE_STEP_HZ)
                    norm = validate_sweep_range(rng)
                    if norm is None:
                        return False, "invalid band: start/stop out of range"
                    ranges.append(norm)
                if not ranges:
                    return False, "no enabled bands"
                if len(ranges) > SWEEP_MAX_RANGES:
                    return False, f"max {SWEEP_MAX_RANGES} bands"
                # Remember operator names so /v1/state + spectrum overlays
                # echo them back (ranges[i] pairs with the i-th enabled band).
                names: Dict[str, str] = {}
                idx = 0
                for b in bands:
                    if not (isinstance(b, dict) and b.get("enabled", True)):
                        continue
                    name = b.get("name")
                    if isinstance(name, str) and name.strip():
                        names[ranges[idx]] = name.strip()
                    idx += 1
                self._band_names = names
                if self.sweep is not None:
                    self.sweep.band_names = names
                # A fresh mission definition supersedes any manual tune.
                self._pretune_ranges = None
                self._pretune_mode = None
                self.sweep.retask(mode="auto", ranges=ranges)
                self._persist_config()
                return True, ""

            if cls in ("tune", "scan", "mission"):
                return False, f"unknown {cls} action"

            return False, "unsupported on this sensor"
        except Exception as e:  # noqa: BLE001 - never crash the endpoint
            log.warning("command: dispatch error (%s)", e)
            return False, "internal error applying command"

    # -- node commissioning portal --
    async def handle_setup(self, request: "web.Request") -> "web.Response":
        # Public page (no auth); every /v1 call it makes carries X-Kujhad-Key.
        return web.Response(text=node_setup_html(), content_type="text/html")

    async def handle_node_config_get(self, request: "web.Request"
                                     ) -> "web.Response":
        if not self._authorized(request):
            return self._unauthorized()
        e = self.equip
        body: Dict[str, Any] = {
            "lat": self.pos.lat,
            "lon": self.pos.lon,
            "gpsdEnabled": bool(self.gpsd_enabled),
            "sdrType": e.sdr_type,
            "antennaCurve": [
                {"f": float(p["f"]), "g": float(p["g"])}
                for p in e.antenna_curve
            ],
            "terrain": e.terrain,
            "siting": e.siting,
            # Live gpsd fix (if any) so the portal can show it.
            "gpsdFix": bool(self.pos.have_fix),
            "gpsdLat": self.pos.lat,
            "gpsdLon": self.pos.lon,
            # Sweep control (current live values).
            "sweepMode": self.sweep.mode if self.sweep else "off",
            "sweepRanges": (list(self.sweep.ranges) if self.sweep
                            else list(DEFAULT_SWEEP_RANGES)),
            "sweepIntervalS": (self.sweep.interval_s if self.sweep
                               else SWEEP_DEFAULT_INTERVAL_S),
            "sweepSnrDb": (self.sweep.snr_threshold if self.sweep
                           else SWEEP_DEFAULT_SNR_DB),
            # Option tables for the page's <select>/preset controls.
            "sdrOptions": SDR_PROFILES,
            "antennaPresets": ANTENNA_PRESETS,
            "terrainOptions": TERRAIN_PROFILES,
            "sitingOptions": SITING_PROFILES,
            "sweepModeOptions": list(SWEEP_MODES),
            "sweepBounds": {
                "maxRanges": SWEEP_MAX_RANGES,
                "freqMinMhz": SWEEP_FREQ_MIN_HZ / 1e6,
                "freqMaxMhz": SWEEP_FREQ_MAX_HZ / 1e6,
                "intervalMinS": SWEEP_INTERVAL_MIN_S,
                "intervalMaxS": SWEEP_INTERVAL_MAX_S,
                "snrMinDb": SWEEP_SNR_MIN_DB,
                "snrMaxDb": SWEEP_SNR_MAX_DB,
            },
        }
        return web.json_response(body)

    async def handle_node_config_post(self, request: "web.Request"
                                      ) -> "web.Response":
        if not self._authorized(request):
            return self._unauthorized()
        try:
            raw = await request.text()
            body = json.loads(raw) if raw.strip() else {}
        except (ValueError, TypeError):
            return web.json_response({"error": "invalid JSON body"}, status=400)
        clean, err = validate_node_config(body)
        if clean is None:
            # Reject bad payloads with 400; NEVER partially apply.
            return web.json_response({"error": err or "invalid config"},
                                     status=400)
        # Apply atomically only after full validation.
        self.pos.lat = clean["lat"]
        self.pos.lon = clean["lon"]
        # A non-null static position is a usable fix for LOB math.
        if not (clean["lat"] == 0.0 and clean["lon"] == 0.0):
            self.pos.have_fix = True
        self.gpsd_enabled = clean["gpsdEnabled"]
        self.equip.sdr_type = clean["sdrType"]
        self.equip.antenna_curve = clean["antennaCurve"]
        self.equip.terrain = clean["terrain"]
        self.equip.siting = clean["siting"]
        # Sweep settings (only those present in the payload) — apply live to the
        # running ingester so the change takes effect without a service restart.
        if self.sweep is not None and any(
                k in clean for k in ("sweepMode", "sweepRanges",
                                     "sweepIntervalS", "sweepSnrDb")):
            try:
                self.sweep.retask(
                    mode=clean.get("sweepMode"),
                    ranges=clean.get("sweepRanges"),
                    interval_s=clean.get("sweepIntervalS"),
                    snr_threshold=clean.get("sweepSnrDb"),
                )
            except Exception as e:  # noqa: BLE001 - never fail the POST on this
                log.warning("sweep: retask failed (%s)", e)
        self._persist_config()
        return web.json_response({"ok": True})

    def _persist_config(self) -> None:
        """Merge the live equipment/position config into the existing JSON
        config file (preserving the api_key), chmod-600. Best-effort."""
        if not self.config_path:
            return
        cfg: Dict[str, Any] = {}
        if os.path.isfile(self.config_path):
            try:
                with open(self.config_path, "r", encoding="utf-8") as f:
                    cfg = json.load(f) or {}
            except (OSError, ValueError):
                cfg = {}
        if self.api_key:
            cfg["api_key"] = self.api_key
        cfg["lat"] = self.pos.lat
        cfg["lon"] = self.pos.lon
        cfg["gpsdEnabled"] = bool(self.gpsd_enabled)
        cfg.update(self.equip.to_config())
        if self.sweep is not None:
            cfg["sweepMode"] = self.sweep.mode
            cfg["sweepRanges"] = list(self.sweep.ranges)
            cfg["sweepIntervalS"] = int(self.sweep.interval_s)
            cfg["sweepSnrDb"] = float(self.sweep.snr_threshold)
        _save_config(self.config_path, cfg)

    async def handle_pairing(self, request: "web.Request") -> "web.Response":
        if not self._authorized(request):
            return self._unauthorized()
        # Same payload an operator would type into manual-pair: address(es)
        # + key. Requires the key to fetch (knowing the key is what the code
        # grants, so this leaks nothing new).
        if self.bind not in ("0.0.0.0", "::", ""):
            ips = [self.bind]
        else:
            ips = [ip for _, ip, _ in enumerate_overlay_ips()] or ["127.0.0.1"]
        body = {
            "device": self.device_name,
            "key": self.api_key,
            "port": self.port,
            "addresses": ips,
            "peerCode": f"{ips[0]}:{self.port}",
        }
        return web.json_response(body)

    async def handle_hardware(self, request: "web.Request") -> "web.Response":
        # PUBLIC (no auth) so the /setup page can render it before a key is
        # entered — same lesson as the option dropdowns; tailnet-firewalled.
        # Read-only and cheap: NEVER claims the SDR or disturbs a running
        # sweep/Kraken. Every branch is defensive; never raises.
        body: Dict[str, Any] = {}
        # 1) Attached USB SDRs (from lsusb; None when lsusb is unavailable).
        try:
            usb_text = read_lsusb_text()
        except Exception:  # noqa: BLE001 - stay defensive
            usb_text = None
        try:
            devices = parse_lsusb_devices(usb_text)
        except Exception:  # noqa: BLE001
            devices = None
        body["devices"] = devices
        if devices is None:
            body["note"] = ("lsusb not available on this node \u2014 cannot "
                            "enumerate USB SDRs (the sensor still runs).")
        # 2) RTL-stealing kernel modules (from /proc/modules; no subprocess).
        try:
            body["conflicts"] = parse_module_conflicts(read_proc_modules())
        except Exception:  # noqa: BLE001
            body["conflicts"] = []
        # 3) What the sensor is DOING with the hardware right now.
        try:
            body["sweep"] = self.sweep.status() if self.sweep else None
        except Exception:  # noqa: BLE001
            body["sweep"] = None
        try:
            body["kraken"] = {
                "connected": bool(self.ingester and self.ingester.connected),
            }
        except Exception:  # noqa: BLE001
            body["kraken"] = {"connected": False}
        return web.json_response(body)

    async def handle_root(self, request: "web.Request") -> "web.Response":
        # Public route, no auth — a tiny status page for humans.
        connected = bool(self.ingester and self.ingester.connected)
        html = (
            "<!doctype html><html><head><meta charset=utf-8>"
            "<title>DF-Kracked LOB Sensor</title></head><body "
            "style='background:#05080a;color:#c8d8e0;font-family:monospace;"
            "padding:16px'>"
            "<h1 style='color:#3fd17d'>DF-Kracked LOB Sensor</h1>"
            f"<p>device: {self.device_name}</p>"
            f"<p>role: sensor (KRAKEN_LOB)</p>"
            f"<p>kraken feed: {'CONNECTED' if connected else 'offline'}</p>"
            f"<p>events held: {self.ring.last_serial}</p>"
            "<p>Pair from the Predator RF app with this node's IP:port and "
            "the API key printed on the sensor console.</p>"
            "<p><a style='color:#4ad8e8' href='/setup'>Node setup / "
            "commissioning &rarr;</a></p>"
            "</body></html>"
        )
        return web.Response(text=html, content_type="text/html")

    def build(self) -> "web.Application":
        app = web.Application()
        app.router.add_get("/", self.handle_root)
        app.router.add_get("/index.html", self.handle_root)
        app.router.add_get("/v1/identify", self.handle_identify)
        app.router.add_get("/v1/state", self.handle_state)
        app.router.add_get("/v1/gps", self.handle_gps)
        app.router.add_get("/v1/events", self.handle_events)
        app.router.add_get("/v1/spectrum", self.handle_spectrum)
        app.router.add_post("/v1/command", self.handle_command)
        # Node commissioning portal.
        app.router.add_get("/setup", self.handle_setup)
        app.router.add_get("/v1/hardware", self.handle_hardware)
        app.router.add_get("/v1/node-config", self.handle_node_config_get)
        app.router.add_post("/v1/node-config", self.handle_node_config_post)
        app.router.add_get("/v1/pairing", self.handle_pairing)
        return app


# ── Interface enumeration & pairing block ───────────────────────────────────


def enumerate_overlay_ips() -> List[Tuple[str, str, int]]:
    """Return (ifname, ipv4, score) for non-loopback IPv4 interfaces, best
    first. Scoring mirrors kujhad_fleet.h: ZeroTier/Tailscale first, then
    RFC1918 LAN. Uses `ip -j addr` when available, else socket fallback."""
    candidates: List[Tuple[str, str, int]] = []
    seen = set()

    def score(name: str, addr: str) -> int:
        s = 0
        if name.startswith("zt"):
            s += 100
        if "tailscale" in name or addr.startswith("100."):
            s += 90
        if name.startswith("head") or name.startswith("ts"):
            s += 80  # Headscale/Tailscale style overlays
        if (addr.startswith("10.") or addr.startswith("192.168.")
                or addr.startswith("172.")):
            s += 10
        return s

    # Preferred: parse `ip -j addr` (Linux).
    try:
        import subprocess
        out = subprocess.run(
            ["ip", "-j", "addr"], capture_output=True, text=True, timeout=3,
        )
        if out.returncode == 0 and out.stdout.strip():
            for iface in json.loads(out.stdout):
                name = iface.get("ifname", "")
                for a in iface.get("addr_info", []):
                    if a.get("family") != "inet":
                        continue
                    addr = a.get("local", "")
                    if not addr or addr.startswith("127."):
                        continue
                    if addr in seen:
                        continue
                    seen.add(addr)
                    candidates.append((name, addr, score(name, addr)))
    except Exception:  # noqa: BLE001 - fall through to socket method
        pass

    # Fallback: primary outbound IP via a UDP socket (no packets sent).
    if not candidates:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("10.255.255.255", 1))
            addr = s.getsockname()[0]
            s.close()
            if addr and not addr.startswith("127."):
                candidates.append(("", addr, score("", addr)))
        except Exception:  # noqa: BLE001
            pass

    candidates.sort(key=lambda c: c[2], reverse=True)
    return candidates


def print_pairing_block(bind: str, port: int, key: str) -> None:
    ips: List[str]
    if bind not in ("0.0.0.0", "::", ""):
        ips = [bind]
    else:
        ips = [ip for _, ip, _ in enumerate_overlay_ips()]
        if not ips:
            ips = ["127.0.0.1"]
    line = "=" * 60
    print("\n" + line, flush=True)
    print("  DF-KRACKED LOB SENSOR — PAIRING", flush=True)
    print(line, flush=True)
    print("  In the Predator RF app: Kujhad → Add Peer, then enter", flush=True)
    print("  the IP:Port and API key below (header X-Kujhad-Key).", flush=True)
    print("", flush=True)
    for ip in ips:
        print(f"  PEER CODE:  {ip}:{port}  key={key}", flush=True)
    print("", flush=True)
    print(f"  API KEY  : {key}", flush=True)
    print(f"  PORT     : {port}", flush=True)
    print(line + "\n", flush=True)


# ── Config persistence ──────────────────────────────────────────────────────


def script_dir_config_path() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        CONFIG_FILENAME)


def load_config(path: str) -> Dict[str, Any]:
    """Read the persisted JSON config (api_key + equipment/position defaults),
    tightening its perms if needed. Returns {} on any error. Never raises."""
    if not os.path.isfile(path):
        return {}
    try:
        _tighten_perms(path)
        with open(path, "r", encoding="utf-8") as f:
            cfg = json.load(f)
        return cfg if isinstance(cfg, dict) else {}
    except (OSError, ValueError):
        return {}


def load_or_create_key(path: str, provided: Optional[str]) -> str:
    # Preserve any existing config (equipment/position) when we rewrite the
    # file — never clobber it down to just the key.
    cfg = load_config(path)
    if provided:
        cfg["api_key"] = provided
        _save_config(path, cfg)
        return provided
    key = cfg.get("api_key")
    if isinstance(key, str) and key:
        return key
    key = uuid.uuid4().hex  # 32 hex chars, same shape as kujhadGenerateApiKey
    cfg["api_key"] = key
    _save_config(path, cfg)
    return key


def _tighten_perms(path: str) -> None:
    """Ensure the config file is not group/other-readable (mode 0600).

    The file holds the shared API key, so a world-readable file would leak
    the secret to any local user. Idempotent; no-op on platforms without
    chmod semantics."""
    try:
        mode = os.stat(path).st_mode & 0o777
        if mode != 0o600:
            os.chmod(path, 0o600)
    except OSError as e:  # pragma: no cover - best-effort hardening
        log.warning("could not tighten perms on %s: %s", path, e)


def _save_config(path: str, cfg: Dict[str, Any]) -> None:
    """Write config atomically with mode 0600.

    Writes to a temp file created with O_WRONLY|O_CREAT|O_TRUNC and 0o600 so
    the secret is never briefly world-readable, then os.replace()s it into
    place (atomic on POSIX)."""
    data = json.dumps(cfg, indent=2)
    tmp = f"{path}.{os.getpid()}.tmp"
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(data)
        finally:
            # Guard against umask having widened the mode on some platforms.
            try:
                os.chmod(tmp, 0o600)
            except OSError:
                pass
        os.replace(tmp, path)
    except OSError as e:
        log.warning("could not persist config to %s: %s", path, e)
        try:
            if os.path.exists(tmp):
                os.remove(tmp)
        except OSError:
            pass


# ── Main ────────────────────────────────────────────────────────────────────


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="DF-Kracked LOB sensor — Kujhad v1 fleet peer for KrakenSDR")
    p.add_argument("--bind", default="0.0.0.0",
                   help="HTTP bind address (default 0.0.0.0)")
    p.add_argument("--port", type=int, default=DEFAULT_PORT,
                   help=f"HTTP port (default {DEFAULT_PORT})")
    p.add_argument("--key", default=None,
                   help="API key; if omitted, generated & persisted next to sensor.py")
    p.add_argument("--name", default=None,
                   help="device name advertised on /v1/identify (default hostname)")
    p.add_argument("--ws", default=None,
                   help="Kraken DoA websocket URL (custom builds only, e.g. "
                        f"{DEFAULT_WS_URL}). Default ingest is HTTP polling "
                        "of --doa-url — stock krakensdr_doa has no websocket.")
    p.add_argument("--doa-url", default=DEFAULT_DOA_URL,
                   help="DF Aggregator bearing file to poll "
                        f"(default {DEFAULT_DOA_URL}; requires "
                        "doa_data_format='DF Aggregator' in the DoA settings)")
    p.add_argument("--doa-is-true", action="store_true",
                   help="treat the XML DOA field as an already-true bearing "
                        "instead of adding HEADING (use if plotted LOBs are "
                        "consistently off by the array heading)")
    p.add_argument("--node-id", default=None,
                   help="node_id label for events (default derived from name)")
    p.add_argument("--lat", type=float, default=None, help="fixed site latitude")
    p.add_argument("--lon", type=float, default=None, help="fixed site longitude")
    p.add_argument("--heading", type=float, default=0.0,
                   help="platform heading, deg (default 0)")
    p.add_argument("--gpsd", action="store_true",
                   help="poll gpsd on localhost:2947 for live position")
    p.add_argument("--sweep", choices=["on", "off", "auto"], default="auto",
                   help="generic-SDR power sweep (plain RTL-SDR/HackRF). "
                        "'auto' (default): run the sweep ONLY while the Kraken "
                        "WS ingester is NOT connected (a Kraken enumerates as "
                        "several RTL dongles that rtl_power would fight); "
                        "'on': always sweep; 'off': never sweep. NOTE: sweep "
                        "settings (--sweep/--sweep-range/-interval/-snr) are "
                        "DEFAULTS only — values saved from the /setup browser "
                        "portal (persisted in df_kracked_sensor.json) WIN and "
                        "are applied live without a restart.")
    p.add_argument("--sweep-range", action="append", default=None,
                   dest="sweep_range",
                   help="frequency range to sweep, repeatable, in rtl_power "
                        "form '<low>:<high>:<binwidth>' (e.g. '400M:470M:100k'). "
                        "Defaults to " + ", ".join(DEFAULT_SWEEP_RANGES)
                        + " if omitted.")
    p.add_argument("--sweep-interval", type=int,
                   default=SWEEP_DEFAULT_INTERVAL_S, dest="sweep_interval",
                   help="rtl_power integration seconds per sweep (default "
                        f"{SWEEP_DEFAULT_INTERVAL_S})")
    p.add_argument("--sweep-snr", type=float, default=SWEEP_DEFAULT_SNR_DB,
                   dest="sweep_snr",
                   help="dB above the estimated noise floor a bin must exceed "
                        f"to emit a hit (default {SWEEP_DEFAULT_SNR_DB:.0f})")
    p.add_argument("--advertise", default="",
                   help="advertised address hint returned in /v1/identify")
    p.add_argument("--log-level", default="INFO",
                   help="logging level (DEBUG/INFO/WARNING)")
    return p.parse_args(argv)


def _equipment_from_config(cfg: Dict[str, Any]) -> NodeEquipment:
    """Build a NodeEquipment from persisted config, validating each field
    against the tables (falls back to safe defaults on anything invalid)."""
    equip = NodeEquipment()
    sdr = cfg.get("sdrType")
    if isinstance(sdr, str) and any(sdr == p["id"] for p in SDR_PROFILES):
        equip.sdr_type = sdr
    terrain = cfg.get("terrain")
    if isinstance(terrain, str) and any(
            terrain == p["id"] for p in TERRAIN_PROFILES):
        equip.terrain = terrain
    siting = cfg.get("siting")
    if isinstance(siting, str) and any(
            siting == p["id"] for p in SITING_PROFILES):
        equip.siting = siting
    curve = cfg.get("antennaCurve")
    if isinstance(curve, list):
        clean: List[Dict[str, float]] = []
        for p in curve[:ANTENNA_CURVE_MAX_POINTS]:
            if not isinstance(p, dict):
                continue
            f = _num(p.get("f"), 0.0)
            g = _num(p.get("g"), 0.0)
            if (ANTENNA_FREQ_MHZ_MIN <= f <= ANTENNA_FREQ_MHZ_MAX
                    and ANTENNA_GAIN_DB_MIN <= g <= ANTENNA_GAIN_DB_MAX):
                clean.append({"f": f, "g": g})
        equip.antenna_curve = clean
    return equip


def build_runtime(args: argparse.Namespace) -> Tuple[
        KujhadSensorApp, KrakenIngester, Optional[SweepIngester],
        NodePosition, EventRing, str, str]:
    """Wire up the components (no I/O started). Returns pieces for run()/tests."""
    cfg_path = script_dir_config_path()
    key = load_or_create_key(cfg_path, args.key)
    cfg = load_config(cfg_path)
    device_name = args.name or socket.gethostname() or "df-kracked-sensor"
    node_id = args.node_id or device_name
    source_label = f"Sensor:{device_name}"

    # Position: CLI flags win; config-file values act as defaults.
    if args.lat is not None:
        lat = float(args.lat)
    else:
        lat = _num(cfg.get("lat"), 0.0)
    if args.lon is not None:
        lon = float(args.lon)
    else:
        lon = _num(cfg.get("lon"), 0.0)
    have_fix = not (lat == 0.0 and lon == 0.0)
    pos = NodePosition(
        lat=lat,
        lon=lon,
        heading=float(args.heading),
        have_fix=have_fix,
    )
    # gpsd: CLI flag wins over persisted default.
    gpsd_enabled = bool(args.gpsd) or bool(cfg.get("gpsdEnabled", False))

    equip = _equipment_from_config(cfg)

    ring = EventRing(EVENT_RING_MAX)
    ingester = KrakenIngester(args.ws or args.doa_url, ring, pos, node_id,
                              source_label, equip=equip)

    # Sweep settings: config-file values (set via the /setup portal POST) win;
    # CLI flags act as the defaults when the config file has no sweep block.
    sweep_mode = args.sweep
    cfg_mode = cfg.get("sweepMode")
    if isinstance(cfg_mode, str) and cfg_mode in SWEEP_MODES:
        sweep_mode = cfg_mode

    cli_ranges = list(getattr(args, "sweep_range", None) or DEFAULT_SWEEP_RANGES)
    sweep_ranges = cli_ranges
    cfg_ranges = cfg.get("sweepRanges")
    if isinstance(cfg_ranges, list) and cfg_ranges:
        norm = [validate_sweep_range(r) for r in cfg_ranges]
        norm = [r for r in norm if r]
        if norm:
            sweep_ranges = norm[:SWEEP_MAX_RANGES]

    sweep_interval = int(getattr(args, "sweep_interval",
                                 SWEEP_DEFAULT_INTERVAL_S))
    cfg_iv = cfg.get("sweepIntervalS")
    if isinstance(cfg_iv, (int, float)) and (
            SWEEP_INTERVAL_MIN_S <= cfg_iv <= SWEEP_INTERVAL_MAX_S):
        sweep_interval = int(cfg_iv)

    sweep_snr = float(getattr(args, "sweep_snr", SWEEP_DEFAULT_SNR_DB))
    cfg_snr = cfg.get("sweepSnrDb")
    if isinstance(cfg_snr, (int, float)) and (
            SWEEP_SNR_MIN_DB <= cfg_snr <= SWEEP_SNR_MAX_DB):
        sweep_snr = float(cfg_snr)

    # A SweepIngester is always constructed (so mode can be toggled live via the
    # portal without a restart). mode 'off' simply idles it. The shared
    # SpectrumHub lets its per-pass frames fan out to /v1/spectrum clients.
    spectrum = SpectrumHub()
    sweep: Optional[SweepIngester] = SweepIngester(
        ring, pos, node_id, source_label,
        ranges=sweep_ranges,
        interval_s=sweep_interval,
        snr_threshold=sweep_snr,
        equip=equip,
        mode=sweep_mode,
        spectrum=spectrum,
    )

    app = KujhadSensorApp(key, device_name, ring, pos, ingester,
                          advertise=args.advertise, sweep=sweep,
                          equip=equip, config_path=cfg_path,
                          bind=args.bind, port=args.port,
                          gpsd_enabled=gpsd_enabled, spectrum=spectrum)
    return app, ingester, sweep, pos, ring, key, device_name


async def _sweep_auto_gate_loop(sweep: SweepIngester, ingester: KrakenIngester,
                                gate: asyncio.Event, stop: asyncio.Event
                                ) -> None:
    """Drive the sweep gate for --sweep auto: keep it OPEN while the Kraken WS
    is NOT connected, CLOSED while it is. Opening is delayed by a grace period
    so a briefly-flapping Kraken doesn't cause rtl_power to race it. Never
    raises out of the loop."""
    grace_deadline: Optional[float] = None
    while not stop.is_set():
        try:
            connected = bool(ingester.connected)
            now = time.time()
            if connected:
                grace_deadline = None
                if gate.is_set():
                    log.info("sweep: Kraken connected — pausing sweep")
                    gate.clear()
            else:
                if gate.is_set():
                    grace_deadline = None
                else:
                    if grace_deadline is None:
                        grace_deadline = now + SWEEP_KRAKEN_GRACE_S
                        log.info("sweep: Kraken not connected — starting sweep "
                                 "in %.0fs", SWEEP_KRAKEN_GRACE_S)
                    elif now >= grace_deadline:
                        grace_deadline = None
                        gate.set()
        except Exception as e:  # noqa: BLE001 - never crash
            log.debug("sweep: gate loop error (%s)", e)
        try:
            await asyncio.wait_for(stop.wait(), timeout=2.0)
        except asyncio.TimeoutError:
            pass


async def run(args: argparse.Namespace) -> None:
    app_obj, ingester, sweep, pos, ring, key, device_name = build_runtime(args)

    stop = asyncio.Event()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            loop.add_signal_handler(sig, stop.set)
        except (NotImplementedError, RuntimeError):  # pragma: no cover
            pass

    aio_app = app_obj.build()
    runner = web.AppRunner(aio_app, access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, args.bind, args.port)
    await site.start()
    log.info("kujhad sensor '%s' listening on %s:%d", device_name,
             args.bind, args.port)

    print_pairing_block(args.bind, args.port, key)

    def _ingest_crashed(t: "asyncio.Task") -> None:
        # A dead ingest task must be LOUD and fatal: with it silently gone
        # the sensor keeps serving /v1/* with zero events, which looks like
        # "connected but no LOBs" from the phone (field-hit failure mode).
        if t.cancelled():
            return
        exc = t.exception()
        if exc is not None:
            log.error("kraken ingest task crashed: %r — shutting down "
                      "(systemd will restart us)", exc)
            stop.set()

    if args.ws:
        ingest_task = asyncio.create_task(ingester.run(stop))
    else:
        ingest_task = asyncio.create_task(ingester.run_http(
            stop, args.doa_url, doa_is_true=args.doa_is_true))
    ingest_task.add_done_callback(_ingest_crashed)
    tasks = [ingest_task]
    if app_obj.gpsd_enabled:
        tasks.append(asyncio.create_task(gpsd_poll_loop(pos)))

    # Generic-SDR sweep. In 'auto' the gate is driven by the Kraken WS
    # connection state (see _sweep_auto_gate_loop). In 'on' the gate is
    # permanently open. A dead sweep task must NOT be fatal (unlike the
    # Kraken ingest task): the sweep is best-effort and self-heals.
    if sweep is not None:
        # The auto-gate loop always runs: it opens/closes the gate based on the
        # Kraken WS connection. The ingester's live mode ('auto'/'on'/'off')
        # decides whether it actually honours the gate, so the operator can flip
        # modes from the /setup portal without a restart.
        gate = asyncio.Event()  # starts closed; gate loop opens it
        tasks.append(asyncio.create_task(
            _sweep_auto_gate_loop(sweep, ingester, gate, stop)))

        def _sweep_done(t: "asyncio.Task") -> None:
            if t.cancelled():
                return
            exc = t.exception()
            if exc is not None:
                log.error("sweep task exited unexpectedly: %r "
                          "(sweep disabled; sensor keeps serving)", exc)

        sweep_task = asyncio.create_task(sweep.run(stop, gate))
        sweep_task.add_done_callback(_sweep_done)
        tasks.append(sweep_task)

    await stop.wait()
    log.info("shutting down…")
    for t in tasks:
        t.cancel()
    for t in tasks:
        try:
            await t
        except asyncio.CancelledError:
            pass
    await runner.cleanup()
    log.info("stopped cleanly")


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, str(args.log_level).upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:  # pragma: no cover
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
