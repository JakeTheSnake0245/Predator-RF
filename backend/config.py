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

    # ── Persistence ───────────────────────────────────────────────────────
    # SQLite-backed mission log (events / tracks / assessments). On crash
    # or restart, active tracks are rehydrated from this DB so an operator
    # doesn't lose situational awareness mid-mission.
    persistence_enabled: bool = field(
        default_factory=lambda: _env_bool("PERSISTENCE_ENABLED", True))
    data_dir: str = field(
        default_factory=lambda: _env("DATA_DIR", "./predator_data"))
    mission_db_filename: str = field(
        default_factory=lambda: _env("MISSION_DB", "mission.db"))
    track_replay_window_hours: float = field(
        default_factory=lambda: _env_float("TRACK_REPLAY_WINDOW_H", 24.0))

    @property
    def mission_db_path(self) -> str:
        import os
        return os.path.join(self.data_dir, self.mission_db_filename)

    # ── CoT / TAK output ──────────────────────────────────────────────────
    # OFF by default — RX-only posture. Operator must explicitly opt in to
    # transmit anything. When enabled, only tracks with an assessment that
    # has escalate_to_atak=True will produce CoT beacons.
    cot_enabled: bool = field(
        default_factory=lambda: _env_bool("COT_ENABLED", False))
    cot_dest_host: str = field(
        default_factory=lambda: _env("COT_DEST_HOST", "239.2.3.1"))
    cot_dest_port: int = field(
        default_factory=lambda: _env_int("COT_DEST_PORT", 6969))
    cot_uid_prefix: str = field(
        default_factory=lambda: _env("COT_UID_PREFIX", "PREDATOR"))
    cot_stale_seconds: float = field(
        default_factory=lambda: _env_float("COT_STALE_S", 300.0))
    cot_multicast_ttl: int = field(
        default_factory=lambda: _env_int("COT_MULTICAST_TTL", 1))

    # ── AutoTasker ─────────────────────────────────────────────────────────
    # When the DecisionEngine recommends a closer look (focus_all_nodes /
    # increase_dwell_time), AutoTasker re-tunes the recommended sensor
    # nodes via the Kujhad HTTP API. Critical assessments still require
    # an operator-in-the-loop and are NEVER auto-actioned.
    # Default OFF for the same reason as cot_enabled: a SIGINT operator
    # must explicitly arm any surface that emits RF (here: re-tune
    # commands to the C++ nodes). RX-only is the safe posture.
    auto_tasker_enabled: bool = field(
        default_factory=lambda: _env_bool("AUTO_TASKER_ENABLED", False))
    auto_tasker_min_interval_s: float = field(
        default_factory=lambda: _env_float("AUTO_TASKER_MIN_INTERVAL_S", 30.0))

    # Maximum time stop() will wait for in-flight persistence/CoT/TDOA
    # tasks to drain before forcing a cancel. A hung TAK server must
    # not block shutdown forever.
    shutdown_drain_timeout_s: float = field(
        default_factory=lambda: _env_float("SHUTDOWN_DRAIN_TIMEOUT_S", 5.0))

    # ── CoC (Center of Control) mode ───────────────────────────────────────
    # When enabled, the backend additionally consumes events from one or
    # more upstream Predator-RF backends via their SSE feed. Lets a TOC
    # workstation aggregate SIGINT from several deployed field stations.
    # Off by default — a normal field deployment doesn't need it.
    coc_mode_enabled: bool = field(
        default_factory=lambda: _env_bool("COC_MODE_ENABLED", False))
    # CSV of upstream base URLs (no trailing /api/v1).
    # Example: "http://station-alpha:8000,http://station-bravo:8000"
    coc_upstream_urls: str = field(
        default_factory=lambda: _env("COC_UPSTREAM_URLS", ""))
    coc_reconnect_delay_s: float = field(
        default_factory=lambda: _env_float("COC_RECONNECT_DELAY_S", 5.0))

    def parse_coc_upstream_urls(self):
        return [u.strip() for u in self.coc_upstream_urls.split(",") if u.strip()]

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
