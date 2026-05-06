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

    # ── RNS transport bridge ───────────────────────────────────────────────
    # Off by default (RX-only posture is preserved). When enabled, the
    # backend spins up an in-process RNS daemon that publishes the same
    # CoT XML the TAK UDP path emits over the `predatorrf/cot.v1` RNS
    # Destination, and forwards inbound RNS-CoT to the local pipeline
    # tagged source_transport="rns". State (identity + interface config)
    # lives under `rns_state_dir`.
    rns_enabled: bool = field(
        default_factory=lambda: _env_bool("RNS_ENABLED", True))
    rns_state_dir: str = field(
        default_factory=lambda: _env("RNS_STATE_DIR", ""))
    # Per spec section C: inbound CoT received over RNS is NOT echoed
    # back over the TAK UDP/TCP feed unless the operator explicitly
    # opts in (default off). This breaks the IP↔RNS bridge loop.
    rns_to_ip_relay: bool = field(
        default_factory=lambda: _env_bool("RNS_TO_IP_RELAY", False))
    # Reliable mode default for the CoT bridge. Spec: false on LoRa
    # (we leave it unset per-interface in schema, default false), true
    # on TCP/UDP/I2P/Auto/Pipe — operators flip per-interface in the
    # Kujhad UI; this is just the bridge-wide fallback when no per-
    # interface flag is set.
    rns_reliable_default: bool = field(
        default_factory=lambda: _env_bool("RNS_RELIABLE_DEFAULT", True))
    # Forward inbound RNS CoT XML to a local ATAK app over UDP. On
    # Android the ATAK Civ build listens on 127.0.0.1:4242 by default;
    # operators set RNS_ATAK_LOCAL_PORT=4242 (or any other port) to
    # have peer-relayed CoT shown on the device's local TAK map.
    # Disabled (0) by default so headless / desktop deployments don't
    # spew UDP at a port nothing is listening on.
    rns_atak_local_port: int = field(
        # On Android (detected via the standard ANDROID_ROOT env var
        # exported by the Bionic init) the spec requires inbound RNS
        # CoT to reach the local ATAK app by default, so we ship 4242
        # (ATAK Civ's standard local UDP CoT input) as the platform
        # default. Desktop/headless deployments stay opt-in (0=off).
        default_factory=lambda: _env_int(
            "RNS_ATAK_LOCAL_PORT",
            4242 if os.environ.get("ANDROID_ROOT") else 0))
    rns_atak_local_host: str = field(
        default_factory=lambda: _env("RNS_ATAK_LOCAL_HOST", "127.0.0.1"))

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
    # Cross-station emitter dedup interval (seconds). Coalesces tracks
    # from local fleet + upstream CoC peers when freq+location agree.
    coc_dedup_interval_s: float = field(
        default_factory=lambda: _env_float("COC_DEDUP_INTERVAL_S", 15.0))
    coc_dedup_freq_tolerance_hz: float = field(
        default_factory=lambda: _env_float("COC_DEDUP_FREQ_TOL_HZ", 5_000.0))
    coc_dedup_location_tolerance_m: float = field(
        default_factory=lambda: _env_float("COC_DEDUP_LOC_TOL_M", 500.0))

    # ── Auth ──────────────────────────────────────────────────────────────
    # Bearer token for all /api/v1/* routes. Empty = open (lab posture).
    # Set to a long random string for any LAN-exposed deployment.
    api_bearer_token: str = field(
        default_factory=lambda: _env("API_BEARER_TOKEN", ""))

    # ── Manual approval gate for CoT pushes ───────────────────────────────
    # When true, CoT escalations enqueue into ApprovalQueue and the
    # operator must POST /api/v1/approvals/{id}/approve before they go out.
    # The cot_enabled flag is still required — both gates must agree.
    cot_require_manual_approval: bool = field(
        default_factory=lambda: _env_bool(
            "COT_REQUIRE_MANUAL_APPROVAL", False))
    cot_approval_expiry_s: float = field(
        default_factory=lambda: _env_float("COT_APPROVAL_EXPIRY_S", 7200.0))
    cot_approval_max_pending: int = field(
        default_factory=lambda: _env_int("COT_APPROVAL_MAX_PENDING", 200))

    # ── AutoTasker fleet budget ───────────────────────────────────────────
    # Global brake — at most N tunes per minute across the whole fleet.
    # Prevents an assessment-loop bug from thrashing every node at once.
    auto_tasker_global_max_per_minute: int = field(
        default_factory=lambda: _env_int(
            "AUTO_TASKER_GLOBAL_MAX_PER_MIN", 30))

    # ── GPS lock freshness ────────────────────────────────────────────────
    # Drop a node from TDOA participation if its last GPS fix is older
    # than this. Default 60 s = "still moving with GPS"; tune lower for
    # vehicle deployments, higher for static workstations.
    gps_max_age_s: float = field(
        default_factory=lambda: _env_float("GPS_MAX_AGE_S", 60.0))

    # ── Single-node RSSI proximity (no-fleet fallback geolocation) ────────
    # When TDOA can't run (single phone / single sensor, or only one
    # GPS-synced node hearing the emitter), this fallback uses free-space
    # path-loss + an assumed transmitter EIRP to convert the received
    # power into a coarse range estimate, centred on the detecting node's
    # GPS. The map renders this as a wide circle, NOT a tight dot — the
    # uncertainty is inherent (TX power is unknown, no bearing info, no
    # multipath model). Tag on the track is `location_method =
    # "rssi_proximity"` so the operator can tell it from a TDOA fix.
    # Disabled by default — opt-in because the result is easy to over-
    # trust if it's not labelled clearly in the UI.
    rssi_proximity_enabled: bool = field(
        default_factory=lambda: _env_bool("RSSI_PROXIMITY_ENABLED", False))
    # Assumed transmitter EIRP in dBm. 30 dBm = 1 W, typical of a
    # handheld VHF/UHF radio. Bump to 40 (10 W) for vehicle mobile;
    # drop to 20 (100 mW) for IoT / short-range telemetry. The
    # estimator has no way to know the truth — this is the operator's
    # best a-priori guess given the band they're sweeping.
    rssi_assumed_eirp_dbm: float = field(
        default_factory=lambda: _env_float("RSSI_ASSUMED_EIRP_DBM", 30.0))
    # Conversion from the SDR's reported dBFS to absolute dBm. Without
    # an absolute power calibration this is approximate; -30 means a
    # 0 dBFS sample ≈ -30 dBm at the antenna port (typical mid-gain
    # RTL-SDR). The Calibrator module can tighten this per-node.
    rssi_dbfs_to_dbm_offset: float = field(
        default_factory=lambda: _env_float("RSSI_DBFS_TO_DBM_OFFSET", -30.0))
    # Multiply the estimated range by this factor to get the rendered
    # circle radius — accounts for path-loss model error, EIRP guess
    # error, multipath. 2.0 = "the emitter is somewhere within 2× the
    # nominal free-space range." Bump higher in cluttered environments.
    rssi_radius_uncertainty_factor: float = field(
        default_factory=lambda: _env_float("RSSI_RADIUS_UNCERTAINTY_FACTOR", 2.0))
    rssi_min_radius_m: float = field(
        default_factory=lambda: _env_float("RSSI_MIN_RADIUS_M", 50.0))
    rssi_max_radius_m: float = field(
        default_factory=lambda: _env_float("RSSI_MAX_RADIUS_M", 5000.0))

    # ── /v1/timing poll ───────────────────────────────────────────────────
    # How often KujhadClient asks the C++ node for its timing telemetry
    # (NTP offset, GPSDO lock state, last-PPS age). Cheap call; 30 s is
    # plenty.
    timing_poll_interval_s: float = field(
        default_factory=lambda: _env_float("TIMING_POLL_INTERVAL_S", 30.0))

    # ── Observability ─────────────────────────────────────────────────────
    # text | json. JSON for ingest into Loki/Splunk/journald.
    log_format: str = field(
        default_factory=lambda: _env("LOG_FORMAT", "text"))
    metrics_enabled: bool = field(
        default_factory=lambda: _env_bool("METRICS_ENABLED", True))

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
