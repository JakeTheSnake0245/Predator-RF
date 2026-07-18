"""
PhoneGPSSource — pull the coordinator kit's GPS from its paired phone.

Every kit — including the coordinator's — is an RPi paired with an
Android phone. Field-node GPS already flows through KujhadClient's
per-node `/v1/gps` poll, but the coordinator's own co-located sensor
node (the RPi running this backend) has no GPS of its own. This module
polls the paired phone's Kujhad `GET /v1/gps` endpoint (same wire shape
KujhadClient uses: {hasFix, lat, lon, accuracy}) and feeds the fix into
the local SensorNodeTrust so the coordinator participates in TDOA with
a live GPS age and appears at its true position on the dashboard map.

Fallback contract (never a silent fake fix):
  - Phone fix       → location_gps + accuracy + updated_ns, gps_source="phone".
  - Phone lost/no-fix → the last phone fix is retained with its honest
    (growing) age — the existing gps_max_age_s gates exclude it from
    TDOA automatically. After `fallback_after_s` without a fresh fix,
    the node reverts to the manual/static location (if configured) with
    gps_source="manual" and location_gps_updated_ns=0, or gps_source=""
    with the position cleared when no manual location exists.

Works whether the paired phone is also a registered fleet node or is
only serving as the coordinator's GPS puck — this poller talks straight
to the phone's Kujhad endpoint and never registers it as a fleet node.
"""
from __future__ import annotations

import asyncio
import logging
import time
from typing import Awaitable, Callable, Optional, Tuple

from backend.models.sensor_node import SensorNodeTrust

logger = logging.getLogger(__name__)

# Optional async fetch override for tests: returns the parsed /v1/gps
# JSON dict, or None on unreachable/error.
FetchFn = Callable[[], Awaitable[Optional[dict]]]


class PhoneGPSSource:
    """Polls a paired phone's Kujhad /v1/gps and updates a local node."""

    def __init__(self, node: SensorNodeTrust,
                 host: str, port: int = 5259, api_key: str = "",
                 tls: bool = False,
                 poll_interval_s: float = 2.0,
                 fallback_after_s: float = 300.0,
                 manual_location: Optional[Tuple[float, float]] = None,
                 manual_accuracy_m: float = 100.0,
                 stamp_contact: bool = False,
                 fetch: Optional[FetchFn] = None):
        self.node = node
        self.host = host
        self.port = int(port)
        self.api_key = api_key
        self.tls = bool(tls)
        self.poll_interval_s = max(0.5, float(poll_interval_s))
        self.fallback_after_s = float(fallback_after_s)
        self.manual_location = manual_location
        self.manual_accuracy_m = float(manual_accuracy_m)
        # When the local node has no KujhadClient of its own (phone is
        # a pure GPS puck), a successful phone poll is the only liveness
        # signal for the coordinator kit — stamp last_contact_ns so the
        # dashboard doesn't show the coordinator as permanently offline.
        self.stamp_contact = bool(stamp_contact)
        self._fetch = fetch
        self._running = False
        self._task: Optional[asyncio.Task] = None
        self._session = None  # aiohttp.ClientSession
        # ns timestamp of the last *fresh phone* fix we applied. Separate
        # from node.location_gps_updated_ns so an operator-set manual
        # location can't be mistaken for a live fix.
        self._last_phone_fix_ns: int = 0
        self.phone_reachable: bool = False

        # Seed the manual location immediately so a phone that never
        # comes up still leaves the coordinator on the map (marked
        # manual, updated_ns=0 → honestly excluded from TDOA).
        if self.node.location_gps is None and self.manual_location:
            self._apply_manual()

    @property
    def base_url(self) -> str:
        scheme = "https" if self.tls else "http"
        return f"{scheme}://{self.host}:{self.port}"

    # ── Lifecycle ─────────────────────────────────────────────────────
    async def start(self):
        if self._fetch is None:
            try:
                import aiohttp
            except ImportError:
                raise RuntimeError(
                    "aiohttp is required for PhoneGPSSource. "
                    "Install it: pip install aiohttp")
            self._session = aiohttp.ClientSession(
                headers={"X-Kujhad-Key": self.api_key})
        self._running = True
        self._task = asyncio.create_task(
            self._poll_loop(), name=f"phone_gps_{self.node.node_id}")
        logger.info("PhoneGPSSource started for node %s ← %s "
                    "(poll %.1fs, fallback after %.0fs, manual=%s)",
                    self.node.node_id, self.base_url,
                    self.poll_interval_s, self.fallback_after_s,
                    "set" if self.manual_location else "unset")

    async def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
            self._task = None
        if self._session:
            await self._session.close()
            self._session = None

    async def _poll_loop(self):
        try:
            while self._running:
                try:
                    await self.poll_once()
                except asyncio.CancelledError:
                    break
                except Exception as exc:
                    logger.warning("PhoneGPSSource %s: poll error: %s",
                                   self.node.node_id, exc)
                await asyncio.sleep(self.poll_interval_s)
        except asyncio.CancelledError:
            pass

    # ── One poll cycle (unit-testable) ────────────────────────────────
    async def poll_once(self, now_ns: Optional[int] = None):
        gps = await self._fetch_gps()
        self.apply(gps, now_ns=now_ns)

    async def _fetch_gps(self) -> Optional[dict]:
        if self._fetch is not None:
            return await self._fetch()
        try:
            async with self._session.get(
                    f"{self.base_url}/v1/gps", timeout=5.0) as resp:
                if resp.status != 200:
                    return None
                data = await resp.json()
                return data if isinstance(data, dict) else None
        except Exception as exc:
            logger.debug("PhoneGPSSource %s: fetch failed: %s",
                         self.node.node_id, exc)
            return None

    def apply(self, gps: Optional[dict], now_ns: Optional[int] = None):
        """Apply one /v1/gps response (or None = unreachable) to the node."""
        now = now_ns if now_ns is not None else time.time_ns()
        self.phone_reachable = gps is not None
        if gps is not None and self.stamp_contact:
            self.node.last_contact_ns = now

        if gps and gps.get("hasFix"):
            try:
                lat = float(gps.get("lat", 0.0))
                lon = float(gps.get("lon", 0.0))
                acc = float(gps.get("accuracy", 10.0))
            except (TypeError, ValueError):
                return
            self.node.location_gps = (lat, lon)
            self.node.location_accuracy_m = acc
            self.node.location_gps_updated_ns = now
            self.node.gps_source = "phone"
            self._last_phone_fix_ns = now
            return

        # No fix / unreachable. Retain the last phone fix with an honest
        # growing age until the fallback window expires, then revert to
        # manual (or clear).
        if self._last_phone_fix_ns:
            age_s = (now - self._last_phone_fix_ns) / 1e9
            if age_s <= self.fallback_after_s:
                return  # keep last fix; staleness gates handle the rest
        # Fallback window expired (or we never had a phone fix).
        if self.manual_location:
            self._apply_manual()
        elif self.node.gps_source == "phone":
            # Stale phone fix and no manual fallback — clear the source
            # tag so the UI shows the position is no longer live. The
            # coords remain as "last known" but updated_ns stays honest.
            self.node.gps_source = ""

    def _apply_manual(self):
        self.node.location_gps = self.manual_location
        self.node.location_accuracy_m = self.manual_accuracy_m
        # NEVER stamp updated_ns for a manual location — the TDOA
        # gps-age gate must exclude static positions that nobody is
        # actively verifying. 0 = "never refreshed".
        self.node.location_gps_updated_ns = 0
        self.node.gps_source = "manual"

    def status(self) -> dict:
        return {
            "phone_url": self.base_url,
            "phone_reachable": self.phone_reachable,
            "gps_source": self.node.gps_source or None,
            "last_phone_fix_ns": self._last_phone_fix_ns or None,
        }
