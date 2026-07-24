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

    # Uncertainty: bearing_std_deg or doa_std_deg, default 10.
    if "bearing_std_deg" in msg and _is_num(msg["bearing_std_deg"]):
        bearing_std = float(msg["bearing_std_deg"])
    elif "doa_std_deg" in msg and _is_num(msg["doa_std_deg"]):
        bearing_std = float(msg["doa_std_deg"])
    else:
        bearing_std = 10.0

    confidence = _num(msg.get("confidence"), 0.5)
    confidence = max(0.0, min(1.0, confidence))
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

    def since(self, since: int) -> Tuple[List[Dict[str, Any]], int]:
        """Return (events oldest-first with serial > since, lastId)."""
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
        return out, last_id


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
                 node_id: str, source_label: str):
        self.url = url
        self.ring = ring
        self.pos = pos
        self.node_id = node_id
        self.source_label = source_label
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


# ── Kujhad v1 HTTP server (aiohttp) ─────────────────────────────────────────


class KujhadSensorApp:
    def __init__(self, api_key: str, device_name: str, ring: EventRing,
                 pos: NodePosition, ingester: Optional[KrakenIngester],
                 advertise: str = ""):
        self.api_key = api_key
        self.device_name = device_name
        self.ring = ring
        self.pos = pos
        self.ingester = ingester
        self.advertise = advertise

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
        # Minimal-but-valid mission shape so a sensor-only node does not
        # break the controller's Mission UI. Empty arrays are fine.
        body = {
            "centerFreq": 0.0,
            "playing": connected,
            "missionMode": 0,
            "scanRunning": connected,
            "scanStatus": "KRAKEN_LOB sensor online" if connected
            else "waiting for Kraken DoA feed",
            "scanPaused": False,
            "searchBands": [],
            "targets": [],
            "excludes": [],
            "hits": [],
            "thresholdDb": 0.0,
            "dwellMs": 0,
            "quickScanDelayMs": 0,
            "quickScanDurationMs": 0,
            "recordAudio": False,
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

    async def handle_command(self, request: "web.Request") -> "web.Response":
        # This is a sensor-only node: it accepts no commands. Return a
        # graceful error JSON (not a crash) so the controller degrades
        # cleanly if it ever POSTs a command.
        if not self._authorized(request):
            return self._unauthorized()
        return web.json_response(
            {"ok": False, "error": "sensor node accepts no commands"},
            status=501,
        )

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
        app.router.add_post("/v1/command", self.handle_command)
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


def load_or_create_key(path: str, provided: Optional[str]) -> str:
    if provided:
        # Persist the operator-supplied key so it survives restarts.
        _save_config(path, {"api_key": provided})
        return provided
    # Reuse an existing persisted key if present.
    if os.path.isfile(path):
        try:
            # Tighten perms on an existing file that was created looser
            # (e.g. by an older version that used a world-readable umask).
            _tighten_perms(path)
            with open(path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
            key = cfg.get("api_key")
            if isinstance(key, str) and key:
                return key
        except (OSError, ValueError):
            pass
    key = uuid.uuid4().hex  # 32 hex chars, same shape as kujhadGenerateApiKey
    _save_config(path, {"api_key": key})
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
    p.add_argument("--advertise", default="",
                   help="advertised address hint returned in /v1/identify")
    p.add_argument("--log-level", default="INFO",
                   help="logging level (DEBUG/INFO/WARNING)")
    return p.parse_args(argv)


def build_runtime(args: argparse.Namespace) -> Tuple[
        KujhadSensorApp, KrakenIngester, NodePosition, EventRing, str, str]:
    """Wire up the components (no I/O started). Returns pieces for run()/tests."""
    cfg_path = script_dir_config_path()
    key = load_or_create_key(cfg_path, args.key)
    device_name = args.name or socket.gethostname() or "df-kracked-sensor"
    node_id = args.node_id or device_name
    source_label = f"Sensor:{device_name}"

    have_fix = args.lat is not None and args.lon is not None
    pos = NodePosition(
        lat=float(args.lat) if args.lat is not None else 0.0,
        lon=float(args.lon) if args.lon is not None else 0.0,
        heading=float(args.heading),
        have_fix=have_fix,
    )

    ring = EventRing(EVENT_RING_MAX)
    ingester = KrakenIngester(args.ws or args.doa_url, ring, pos, node_id,
                              source_label)
    app = KujhadSensorApp(key, device_name, ring, pos, ingester,
                          advertise=args.advertise)
    return app, ingester, pos, ring, key, device_name


async def run(args: argparse.Namespace) -> None:
    app_obj, ingester, pos, ring, key, device_name = build_runtime(args)

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
    if args.gpsd:
        tasks.append(asyncio.create_task(gpsd_poll_loop(pos)))

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
