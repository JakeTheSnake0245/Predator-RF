"""Observability: metrics registry render + structured logging."""
import io
import json
import logging
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from backend.observability.metrics import MetricsRegistry
from backend.observability.logging import JsonFormatter, configure_logging


class MetricsRegistryTests(unittest.TestCase):
    def test_counter_accumulates(self):
        m = MetricsRegistry()
        m.counter("evs", 1.0, labels={"node": "n1"})
        m.counter("evs", 2.0, labels={"node": "n1"})
        m.counter("evs", 1.0, labels={"node": "n2"})
        out = m.render()
        self.assertIn('evs{node="n1"} 3', out)
        self.assertIn('evs{node="n2"} 1', out)

    def test_gauge_overwrites(self):
        m = MetricsRegistry()
        m.gauge("active_tracks", 5)
        m.gauge("active_tracks", 9)
        self.assertIn("active_tracks 9", m.render())

    def test_render_includes_help_and_type_lines(self):
        m = MetricsRegistry()
        m.counter("foo_total", 1, help_text="counts foos")
        out = m.render()
        self.assertIn("# HELP foo_total counts foos", out)
        self.assertIn("# TYPE foo_total counter", out)

    def test_uptime_always_emitted(self):
        m = MetricsRegistry()
        out = m.render()
        self.assertIn("backend_uptime_seconds", out)

    def test_label_value_with_quotes_is_escaped(self):
        m = MetricsRegistry()
        m.gauge("x", 1, labels={"name": 'node "alpha"'})
        out = m.render()
        self.assertIn(r'name="node \"alpha\""', out)


class JsonFormatterTests(unittest.TestCase):
    def test_renders_basic_record_as_json(self):
        f = JsonFormatter()
        rec = logging.LogRecord(
            name="test", level=logging.INFO, pathname="x", lineno=1,
            msg="hello %s", args=("world",), exc_info=None)
        out = f.format(rec)
        d = json.loads(out)
        self.assertEqual(d["msg"], "hello world")
        self.assertEqual(d["level"], "INFO")
        self.assertEqual(d["logger"], "test")
        self.assertIn("ts", d)

    def test_extra_context_fields_propagate(self):
        f = JsonFormatter()
        rec = logging.LogRecord(
            name="x", level=logging.INFO, pathname="x", lineno=1,
            msg="m", args=(), exc_info=None)
        rec.mission_id = "m-123"
        rec.track_id = "t-456"
        rec.node_id = "n1"
        d = json.loads(f.format(rec))
        self.assertEqual(d["mission_id"], "m-123")
        self.assertEqual(d["track_id"], "t-456")
        self.assertEqual(d["node_id"], "n1")


class ConfigureLoggingTests(unittest.TestCase):
    def test_idempotent_reconfigure_doesnt_double_log(self):
        configure_logging(level="INFO", fmt="text")
        configure_logging(level="INFO", fmt="json")
        # Single handler after two configure calls
        self.assertEqual(len(logging.getLogger().handlers), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
