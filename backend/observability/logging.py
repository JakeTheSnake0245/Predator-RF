"""
Structured logging — one line per event, JSON when LOG_FORMAT=json.

Why opt-in: humans tailing the console want flat text; ops dashboards
ingesting via Loki / Splunk / journald want JSON. Don't make either
camp suffer the other's format.

Adds three context fields automatically when present in the LogRecord
'extra=' dict: mission_id, track_id, node_id. Use them like:
    logger.info("event", extra={"mission_id": m, "track_id": t})
"""
from __future__ import annotations

import json
import logging
import time
from typing import Any, Dict


class JsonFormatter(logging.Formatter):
    """Emit one log line as a JSON object. Preserves any 'extra='
    fields (specifically mission_id/track_id/node_id) so log
    aggregators can index them."""

    _KNOWN_CTX = ("mission_id", "track_id", "node_id", "emitter_id",
                  "frequency", "phase")

    def format(self, record: logging.LogRecord) -> str:
        out: Dict[str, Any] = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%S",
                                 time.gmtime(record.created))
                  + f".{int(record.msecs):03d}Z",
            "level": record.levelname,
            "logger": record.name,
            "msg": record.getMessage(),
        }
        for k in self._KNOWN_CTX:
            v = getattr(record, k, None)
            if v is not None:
                out[k] = v
        if record.exc_info:
            out["exc_info"] = self.formatException(record.exc_info)
        return json.dumps(out, default=str, separators=(",", ":"))


def configure_logging(level: str = "INFO", fmt: str = "text"):
    """Install one root handler with the chosen formatter. Idempotent —
    safe to call from main() and from test fixtures."""
    root = logging.getLogger()
    root.setLevel(getattr(logging, level.upper(), logging.INFO))
    # Wipe any pre-existing handlers so a reconfigure call doesn't
    # double-log every line.
    for h in list(root.handlers):
        root.removeHandler(h)
    h = logging.StreamHandler()
    if fmt.lower() == "json":
        h.setFormatter(JsonFormatter())
    else:
        h.setFormatter(logging.Formatter(
            "%(asctime)s [%(levelname)s] %(name)s: %(message)s"))
    root.addHandler(h)
