"""
CoCAggregator — connects to one or more upstream Predator-RF backends
via their /api/v1/events/stream SSE feed and re-publishes each event
into the local pipeline.

Use case
--------
A TOC workstation that aggregates SIGINT from multiple field stations
(each running its own Predator-RF backend with its own Kujhad fleet).
The CoC station gets a unified fused track picture, persistence log,
and TAK output without itself owning any C++ sensor nodes.

Operating modes
---------------
* COC_MODE_ENABLED=true, FLEET_NODES empty   → pure CoC workstation.
  No local fleet; ingests only upstream events.
* COC_MODE_ENABLED=true, FLEET_NODES set     → hybrid. Has its own
  local nodes AND aggregates from peers. Useful for a primary TOC
  with attached SDRs that also pulls in remote stations.
* COC_MODE_ENABLED=false (default)           → field station — talks
  only to its own Kujhad nodes.

Tagging
-------
Every aggregated event gets `_upstream` set to the source URL so the
operator can tell which station originated it, and so the TrackManager
can de-duplicate observations of the same emitter heard by both the
local fleet and a peer.

Network
-------
This module uses raw `asyncio` + `urllib`-style line streaming to avoid
adding aiohttp as a hard requirement (it's already used by Kujhad
client, but we want this layer to be importable on a stripped-down
host for offline testing). When aiohttp IS available it's used; when
it's not, we fall back to a stdlib http.client thread.

For unit tests, `feed_event(ev_dict)` is a synchronous entry point
that bypasses the network entirely.
"""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Awaitable, Callable, List, Optional

logger = logging.getLogger(__name__)

EventCallback = Callable[[dict], None]


class CoCAggregator:
    def __init__(self,
                 upstream_urls: List[str],
                 on_event: Optional[EventCallback] = None,
                 *,
                 reconnect_delay_s: float = 5.0,
                 spawn: Optional[Callable[[Awaitable], asyncio.Task]] = None):
        self.upstream_urls = [u.rstrip("/") for u in upstream_urls if u]
        self._on_event = on_event
        self.reconnect_delay_s = float(reconnect_delay_s)
        self._spawn = spawn or asyncio.create_task
        self._stop = False
        self._tasks: List[asyncio.Task] = []
        # Counters for /metrics + tests
        self.events_received = 0
        self.events_per_upstream: dict = {}
        self.connect_attempts = 0
        self.connect_failures = 0

    def on_event(self, fn: EventCallback) -> None:
        self._on_event = fn

    def feed_event(self, ev: dict, source: str = "<test>") -> None:
        """Synchronous entry point. The SSE consumer (or tests) calls
        this with each parsed event. Tags the event with `_upstream`
        and dispatches to the on_event callback."""
        self.events_received += 1
        self.events_per_upstream[source] = \
            self.events_per_upstream.get(source, 0) + 1
        ev = dict(ev)
        ev.setdefault("_upstream", source)
        if self._on_event is not None:
            try:
                self._on_event(ev)
            except Exception as exc:
                logger.warning("CoCAggregator: on_event raised: %s", exc)

    async def start(self) -> None:
        """Spawn one consumer task per upstream URL."""
        if not self.upstream_urls:
            logger.info("CoCAggregator: no upstream URLs — nothing to do")
            return
        logger.info("CoCAggregator: starting %d upstream consumer(s)",
                    len(self.upstream_urls))
        for url in self.upstream_urls:
            t = self._spawn(self._consume_upstream(url))
            self._tasks.append(t)

    async def stop(self) -> None:
        self._stop = True
        for t in self._tasks:
            t.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()

    async def _consume_upstream(self, base_url: str) -> None:
        """Consume an SSE stream from one upstream backend, with
        reconnect-on-failure. Each `data: {...}` line becomes one
        feed_event call."""
        sse_url = f"{base_url}/api/v1/events/stream"
        while not self._stop:
            self.connect_attempts += 1
            try:
                await self._read_sse_aiohttp(sse_url, base_url)
            except asyncio.CancelledError:
                return
            except Exception as exc:
                self.connect_failures += 1
                logger.warning("CoCAggregator: upstream %s failed: %s "
                               "— retrying in %.1fs",
                               sse_url, exc, self.reconnect_delay_s)
            if self._stop:
                return
            await asyncio.sleep(self.reconnect_delay_s)

    async def _read_sse_aiohttp(self, sse_url: str, source: str) -> None:
        """Stream SSE events. Uses aiohttp when present, otherwise
        raises ImportError so the caller can fall back."""
        try:
            import aiohttp  # type: ignore
        except ImportError:
            raise ImportError(
                "aiohttp not installed; CoC aggregator network IO requires "
                "aiohttp. (Tests use feed_event() directly and don't need it.)"
            )
        timeout = aiohttp.ClientTimeout(total=None, sock_read=60)
        async with aiohttp.ClientSession(timeout=timeout) as session:
            async with session.get(sse_url) as resp:
                resp.raise_for_status()
                buf = b""
                async for chunk in resp.content.iter_any():
                    if self._stop:
                        return
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.strip()
                        if not line.startswith(b"data:"):
                            continue
                        payload = line[len(b"data:"):].strip()
                        if not payload:
                            continue
                        try:
                            ev = json.loads(payload.decode("utf-8"))
                        except (json.JSONDecodeError, UnicodeDecodeError):
                            continue
                        self.feed_event(ev, source=source)

    def stats(self) -> dict:
        return {
            "upstream_count": len(self.upstream_urls),
            "events_received": self.events_received,
            "connect_attempts": self.connect_attempts,
            "connect_failures": self.connect_failures,
            "events_per_upstream": dict(self.events_per_upstream),
        }
