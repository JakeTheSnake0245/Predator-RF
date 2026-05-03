"""
Tiny stdlib-only Prometheus-text-format metrics registry.

Why hand-rolled instead of prometheus_client: the RPi/sensor nodes are
the deployment target and we already fight the dep budget on those.
The text format is trivially parseable; we lose histogram quantiles
(we use sum+count which Prometheus scrapes fine) but gain zero deps.

Counters and gauges only — no histograms. If you need a histogram,
record the sum+count via two separate metrics; that's how Prometheus
quantile estimation works under the hood anyway.
"""
from __future__ import annotations

import threading
import time
from collections import defaultdict
from typing import Dict, Iterable, Optional, Tuple


class MetricsRegistry:
    def __init__(self):
        self._lock = threading.Lock()
        # value keyed by (name, frozenset(label_items))
        self._counters: Dict[Tuple[str, frozenset], float] = defaultdict(float)
        self._gauges:   Dict[Tuple[str, frozenset], float] = {}
        self._help:     Dict[str, str] = {}
        self._type:     Dict[str, str] = {}
        self._started_ns = time.time_ns()

    def counter(self, name: str, value: float = 1.0,
                labels: Optional[Dict[str, str]] = None,
                help_text: str = ""):
        key = (name, frozenset((labels or {}).items()))
        with self._lock:
            self._counters[key] += value
            self._help.setdefault(name, help_text)
            self._type[name] = "counter"

    def gauge(self, name: str, value: float,
              labels: Optional[Dict[str, str]] = None,
              help_text: str = ""):
        key = (name, frozenset((labels or {}).items()))
        with self._lock:
            self._gauges[key] = value
            self._help.setdefault(name, help_text)
            self._type[name] = "gauge"

    def render(self) -> str:
        """Render the full registry in Prometheus text exposition format."""
        lines = []
        # Stable ordering helps `diff` against scrape outputs
        names = sorted(set(n for (n, _) in self._counters)
                       | set(n for (n, _) in self._gauges))
        for name in names:
            if name in self._help and self._help[name]:
                lines.append(f"# HELP {name} {self._help[name]}")
            if name in self._type:
                lines.append(f"# TYPE {name} {self._type[name]}")
            for (n, lbls), v in self._counters.items():
                if n != name:
                    continue
                lines.append(self._fmt(name, dict(lbls), v))
            for (n, lbls), v in self._gauges.items():
                if n != name:
                    continue
                lines.append(self._fmt(name, dict(lbls), v))
        # Always emit process uptime so a scraper can compute restarts
        lines.append("# HELP backend_uptime_seconds Process uptime")
        lines.append("# TYPE backend_uptime_seconds gauge")
        lines.append(self._fmt("backend_uptime_seconds", {},
                               (time.time_ns() - self._started_ns) / 1e9))
        return "\n".join(lines) + "\n"

    @staticmethod
    def _fmt(name: str, labels: Dict[str, str], v: float) -> str:
        if not labels:
            return f"{name} {v:g}"
        parts = ",".join(f'{k}="{_esc(str(val))}"'
                         for k, val in sorted(labels.items()))
        return f"{name}{{{parts}}} {v:g}"


def _esc(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


# Process-singleton registry. Modules call `metrics.counter(...)` etc.
metrics = MetricsRegistry()
