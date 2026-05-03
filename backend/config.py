"""
Backend configuration — read from environment variables or .env file.
"""
import os
from dataclasses import dataclass, field
from typing import List


def _env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)

def _env_int(key: str, default: int) -> int:
    try:
        return int(os.environ.get(key, default))
    except (ValueError, TypeError):
        return default

def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (ValueError, TypeError):
        return default

def _env_bool(key: str, default: bool = False) -> bool:
    val = os.environ.get(key, str(default)).lower()
    return val in ('1', 'true', 'yes', 'on')


@dataclass
class BackendConfig:
    # ── API server ─────────────────────────────────────────────────────────
    api_host: str = field(default_factory=lambda: _env("API_HOST", "0.0.0.0"))
    api_port: int = field(default_factory=lambda: _env_int("API_PORT", 8000))
    api_workers: int = field(default_factory=lambda: _env_int("API_WORKERS", 1))

    # ── Fusion engine ──────────────────────────────────────────────────────
    track_maintenance_interval_s: float = field(
        default_factory=lambda: _env_float("TRACK_MAINTENANCE_S", 10.0))
    track_merge_interval_s: float = field(
        default_factory=lambda: _env_float("TRACK_MERGE_S", 30.0))
    min_confidence_threshold: float = field(
        default_factory=lambda: _env_float("MIN_CONFIDENCE", 0.3))

    # ── Baseline learning ──────────────────────────────────────────────────
    baseline_learning_window_hours: float = field(
        default_factory=lambda: _env_float("BASELINE_WINDOW_H", 24.0))
    baseline_prune_interval_hours: float = field(
        default_factory=lambda: _env_float("BASELINE_PRUNE_H", 6.0))

    # ── Kujhad fleet ──────────────────────────────────────────────────────
    # Comma-separated list of node specs: "id@host:port:key:hardware"
    # e.g. FLEET_NODES=node1@192.168.1.10:5259:mykey:hackrf,node2@192.168.1.11:5259:key2:rtlsdr
    fleet_nodes_csv: str = field(
        default_factory=lambda: _env("FLEET_NODES", ""))

    # ── Logging ────────────────────────────────────────────────────────────
    log_level: str = field(
        default_factory=lambda: _env("LOG_LEVEL", "INFO").upper())

    # ── TDOA ──────────────────────────────────────────────────────────────
    tdoa_enabled: bool = field(
        default_factory=lambda: _env_bool("TDOA_ENABLED", True))

    def parse_fleet_nodes(self):
        """Parse FLEET_NODES CSV into SensorNodeTrust objects."""
        from backend.models.sensor_node import SensorNodeTrust
        nodes = []
        if not self.fleet_nodes_csv:
            return nodes
        for spec in self.fleet_nodes_csv.split(','):
            spec = spec.strip()
            if not spec:
                continue
            try:
                # Format: node_id@host:port:api_key:hardware_code
                node_id, rest = spec.split('@', 1)
                parts = rest.split(':')
                host = parts[0]
                port = int(parts[1]) if len(parts) > 1 else 5259
                api_key = parts[2] if len(parts) > 2 else ""
                hw = parts[3] if len(parts) > 3 else "rtlsdr"
                nodes.append(SensorNodeTrust(
                    node_id=node_id,
                    hardware_code=hw,
                    kujhad_host=host,
                    kujhad_port=port,
                    kujhad_api_key=api_key,
                ))
            except Exception as exc:
                import logging
                logging.getLogger(__name__).warning(
                    "Failed to parse fleet node spec '%s': %s", spec, exc)
        return nodes


# Singleton config loaded at import time
config = BackendConfig()
