"""
CoCAggregator tests — verify the workstation-mode SSE aggregator
re-publishes events into the local pipeline with proper tagging.
Uses feed_event() for synchronous tests; network IO is exercised
manually in integration only.
"""
from __future__ import annotations

import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.coc.aggregator import CoCAggregator


class CoCAggregatorTests(unittest.IsolatedAsyncioTestCase):
    def test_feed_event_tags_with_upstream_source(self):
        seen: list = []
        agg = CoCAggregator(upstream_urls=["http://alpha:8000"],
                            on_event=lambda ev: seen.append(ev))
        agg.feed_event({"emitter_id": "abc", "frequency": 462e6},
                       source="http://alpha:8000")
        self.assertEqual(len(seen), 1)
        self.assertEqual(seen[0]["_upstream"], "http://alpha:8000",
            "every aggregated event must be tagged with its origin")
        self.assertEqual(seen[0]["emitter_id"], "abc")

    def test_existing_upstream_tag_is_preserved(self):
        """If an event already has _upstream (e.g. relayed through
        multiple CoC layers), don't clobber the original origin."""
        seen: list = []
        agg = CoCAggregator(upstream_urls=["http://hub"], on_event=seen.append)
        agg.feed_event({"emitter_id": "abc", "_upstream": "field-station-1"},
                       source="http://hub")
        self.assertEqual(seen[0]["_upstream"], "field-station-1")

    def test_callback_exception_does_not_break_aggregator(self):
        def boom(_ev):
            raise RuntimeError("callback failure")
        agg = CoCAggregator(upstream_urls=["http://x"], on_event=boom)
        # Must not raise — aggregator must keep running across consumer
        # bugs, otherwise one bad upstream could DOS the whole CoC station
        agg.feed_event({"emitter_id": "x"}, source="http://x")
        self.assertEqual(agg.events_received, 1)

    def test_stats_track_per_upstream_counts(self):
        agg = CoCAggregator(upstream_urls=["http://a", "http://b"],
                             on_event=lambda _: None)
        agg.feed_event({"x": 1}, source="http://a")
        agg.feed_event({"x": 2}, source="http://a")
        agg.feed_event({"x": 3}, source="http://b")
        s = agg.stats()
        self.assertEqual(s["events_received"], 3)
        self.assertEqual(s["events_per_upstream"], {"http://a": 2, "http://b": 1})

    def test_no_upstreams_start_is_noop(self):
        async def run():
            agg = CoCAggregator(upstream_urls=[], on_event=lambda _: None)
            await agg.start()  # must not raise
            await agg.stop()   # must not raise
        asyncio.get_event_loop().run_until_complete(run())

    async def test_on_event_can_be_set_after_construction(self):
        seen: list = []
        agg = CoCAggregator(upstream_urls=["http://a"])
        agg.on_event(lambda ev: seen.append(ev))
        agg.feed_event({"a": 1}, source="http://a")
        self.assertEqual(len(seen), 1)

    async def test_stop_is_idempotent_with_no_started_tasks(self):
        agg = CoCAggregator(upstream_urls=["http://a"], on_event=lambda _: None)
        # Never called start() → _tasks empty → stop must not blow up
        await agg.stop()
        await agg.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
