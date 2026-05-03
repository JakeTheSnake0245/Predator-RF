"""
Predator-SDR Python Backend — main entry point.

Starts:
  1. KujhadFleetManager — connects to C++ sensor nodes via HTTP API
  2. TrackManager — fusion engine (associate events → tracks)
  3. Intelligence pipeline (anomaly detection + decision engine)
  4. FastAPI REST server

Usage (from project root):
    python -m backend.main
    LOG_LEVEL=DEBUG FLEET_NODES="node1@192.168.1.10:5259:mykey:hackrf" python -m backend.main
"""

import asyncio
import logging
import os
import signal
import sys
from typing import Optional

# Ensure project root is on sys.path so 'backend.*' imports resolve regardless
# of the working directory the script is launched from.
_project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _project_root not in sys.path:
    sys.path.insert(0, _project_root)

from backend.config import config
from backend.fusion.track_manager import TrackManager
from backend.fusion.proximity_estimator import ProximityEstimator
from backend.fusion.cross_station_dedup import CrossStationDedup
from backend.coordination.kujhad_client import KujhadFleetManager
from backend.intelligence.anomaly_detector import AnomalyDetector
from backend.intelligence.decision_engine import DecisionEngine
from backend.intelligence.rf_baseline import RFBaseline
from backend.api.routes.events import push_event
from backend.persistence import MissionStore
from backend.fusion.tdoa_coordinator import TDOACoordinator
from backend.output import CoTEmitter
from backend.rns.bridge import RNSCotBridge
from backend.rns.daemon import RNSDaemon
from backend.coordination.auto_tasker import AutoTasker
from backend.coc import CoCAggregator
from backend.operator.approvals import ApprovalQueue
from backend.operator.missions import MissionRegistry
from backend.operator.overrides import OverrideRegistry
from backend.observability.logging import configure_logging
from backend.observability.metrics import metrics

# Structured logging — JSON when LOG_FORMAT=json, plain text otherwise.
configure_logging(level=config.log_level, fmt=config.log_format)
logger = logging.getLogger("predator.backend")


class PredatorBackend:
    """Top-level service orchestrator."""

    def __init__(self):
        self.baseline = RFBaseline(
            learning_window_hours=config.baseline_learning_window_hours)
        self.anomaly_detector = AnomalyDetector(self.baseline)
        self.decision_engine = DecisionEngine(self.anomaly_detector)
        _proximity = (ProximityEstimator() if config.rssi_proximity_enabled else None)
        self.track_manager = TrackManager(proximity_estimator=_proximity)
        self.fleet_manager = KujhadFleetManager()

        # TDOA geolocation — needs ≥2 GPS-synced nodes hearing the same
        # emitter inside a 5s window. Was previously instantiated nowhere;
        # this is the joint-sensing headline feature.
        self.tdoa: Optional[TDOACoordinator] = (
            TDOACoordinator() if config.tdoa_enabled else None)

        # CoT/TAK emitter — RX-by-default, operator must set COT_ENABLED=1
        # to opt in. Even then, per-track gating still requires the
        # AssessmentReport to set escalate_to_atak=True.
        self.cot: CoTEmitter = CoTEmitter(
            dest_host=config.cot_dest_host,
            dest_port=config.cot_dest_port,
            enabled=config.cot_enabled,
            uid_prefix=config.cot_uid_prefix,
            stale_seconds=config.cot_stale_seconds,
            multicast_ttl=config.cot_multicast_ttl,
        )

        # RNS transport bridge — parallel to the TAK UDP/TCP CoT path.
        # The bridge is created unconditionally and runs as a soft no-op
        # until `rns_enabled` is set; that lets every existing test keep
        # constructing PredatorBackend without RNS side effects.
        self.rns_bridge: RNSCotBridge = RNSCotBridge(
            own_hash16=("0" * 16),
            reliable_default=getattr(config, "rns_reliable_default", True))
        self.rns_daemon: Optional[RNSDaemon] = None
        if getattr(config, "rns_enabled", False):
            try:
                self.rns_daemon = RNSDaemon(
                    state_dir=getattr(config, "rns_state_dir", None) or None,
                    cot_bridge=self.rns_bridge)
                # Sync the bridge's hash with the daemon's identity so
                # loop-suppression and outbound `src` tags match.
                self.rns_bridge.own_hash16 = self.rns_daemon.identity_hash16()
                # Apply the configured peer allowlist (empty list = open
                # mode per spec section D, with a startup warning logged
                # by the daemon).
                self.rns_bridge.set_allowlist(
                    self.rns_daemon.config.get("peer_allowlist", []))
                # Inbound RNS-CoT is fed to the CoC aggregator path with
                # source_transport="rns" so it reuses the existing
                # remote-event handler (track fusion, persistence, dedupe).
                self.rns_bridge.set_inbound_fn(self._on_rns_inbound_cot)
            except Exception as exc:
                logger.error("RNS daemon init failed: %s — RNS disabled", exc)
                self.rns_daemon = None
        # Hook the CoT emitter so every successful TAK send also goes
        # over RNS. When the daemon isn't running, publish() is a no-op.
        # `reliable=None` lets the bridge pick its configured default;
        # the daemon promotes to Link automatically when the envelope
        # exceeds the path MTU (spec section C).
        self.cot.attach_fanout(
            lambda xml, uid: self.rns_bridge.publish(xml, uid))
        # IP↔RNS loop break: by default RNS-sourced CoT is not echoed
        # back over the TAK UDP feed (spec section C).
        self.cot.set_rns_to_ip_relay(
            getattr(config, "rns_to_ip_relay", False))

        # AutoTasker — closes the intel→action loop. On
        # `increase_dwell_time` / `focus_all_nodes` assessments, tunes the
        # recommended_nodes to the track's primary frequency. Critical
        # assessments still require human approval. We pass `spawn=self._spawn`
        # below in __init__ so its tune tasks join the shutdown-drain set;
        # a SIGTERM mid-tune will be bounded/cancelled by
        # SHUTDOWN_DRAIN_TIMEOUT_S instead of leaking past stop().
        self.auto_tasker: AutoTasker = AutoTasker(
            self.fleet_manager,
            min_interval_s=config.auto_tasker_min_interval_s,
            enabled=config.auto_tasker_enabled,
            global_max_per_minute=config.auto_tasker_global_max_per_minute,
            spawn=lambda coro: self._spawn(coro),
        )

        # Mission persistence — SQLite-backed event/track/assessment log.
        # Disabled cleanly when PERSISTENCE_ENABLED=false (e.g., for unit
        # tests that don't want DB side effects).
        self.store: Optional[MissionStore] = None
        if config.persistence_enabled:
            try:
                self.store = MissionStore(config.mission_db_path)
                logger.info("MissionStore opened at %s "
                            "(events=%d, tracks=%d, assessments=%d)",
                            config.mission_db_path,
                            self.store.event_count(),
                            self.store.track_count(),
                            self.store.assessment_count())
            except Exception as exc:
                logger.error("MissionStore init failed at %s: %s — "
                             "running without persistence",
                             config.mission_db_path, exc)
                self.store = None

        # CoC aggregator — optional, consumes events from peer backends
        # over SSE and feeds them through our local pipeline as if they
        # were native fleet events. Off by default (field-station mode).
        upstreams = config.parse_coc_upstream_urls()
        self.coc: Optional[CoCAggregator] = None
        if config.coc_mode_enabled and upstreams:
            self.coc = CoCAggregator(
                upstream_urls=upstreams,
                reconnect_delay_s=config.coc_reconnect_delay_s,
                spawn=lambda coro: self._spawn(coro),
            )

        # Mission lifecycle — operator marks the start/end of a SIGINT
        # mission so events/tracks/assessments get tagged with mission_id
        # and can be exported as a single AAR bundle.
        self.missions: MissionRegistry = MissionRegistry(store=self.store)
        if self.store is not None:
            self.store.set_mission_provider(lambda: self.missions.active_id)

        # Operator override registry — friendly list, frequency
        # blacklist, manual location overrides. Persists into the
        # mission DB so a restart preserves operator intent.
        self.overrides: OverrideRegistry = OverrideRegistry(store=self.store)

        # CoT manual-approval queue — when COT_REQUIRE_MANUAL_APPROVAL
        # is set, escalations enqueue here instead of going straight to
        # the CoT emitter. The operator UI POSTs approve/reject and the
        # approved items drain to cot.emit_track via on_approved.
        self.approvals: ApprovalQueue = ApprovalQueue(
            max_pending=config.cot_approval_max_pending,
            expiry_s=config.cot_approval_expiry_s)
        self.approvals.on_approved(self._on_approval_decision)
        # Snapshot mission_id at enqueue time so audit attribution is
        # correct even if the operator rolls the mission before deciding.
        self.approvals.set_mission_provider(lambda: self.missions.active_id)
        # Persist EVERY terminal state (approved/rejected/expired/dropped)
        # to op_approvals_log so the AAR has the full operator decision
        # ledger — not just the approved subset.
        self.approvals.on_terminal(self._on_approval_terminal)

        # Cross-station dedup — coalesces tracks for the same physical
        # emitter heard by both local fleet and CoC peers.
        self.dedup: CrossStationDedup = CrossStationDedup(
            freq_tolerance_hz=config.coc_dedup_freq_tolerance_hz,
            location_tolerance_m=config.coc_dedup_location_tolerance_m)

        # Background task accounting — every fire-and-forget task spawned
        # from `_on_rf_event` (persistence writes, TDOA solves, CoT emits,
        # AutoTasker tunes) is registered here so `stop()` can drain them
        # before closing the DB / UDP socket. Without this, a SIGTERM
        # mid-mission would race with in-flight writes and lose the last
        # few seconds of data — exactly when an operator most wants it.
        self._pending_tasks: set[asyncio.Task] = set()

        # Wire event callbacks
        self.fleet_manager.on_event(self._on_rf_event)
        self.track_manager.on_new_track(self._on_new_track)
        self.track_manager.on_update(self._on_track_update)
        # CoC events feed the SAME callback so they go through baseline,
        # tracking, anomaly detection, decisioning, persistence, CoT and
        # AutoTasker — exactly like a local-fleet event would. They keep
        # their `_upstream` tag so the operator can tell origin.
        if self.coc is not None:
            self.coc.on_event(self._on_remote_event)

    def _spawn(self, coro) -> asyncio.Task:
        """Schedule a coroutine and track it for shutdown drain."""
        task = asyncio.create_task(coro)
        self._pending_tasks.add(task)
        task.add_done_callback(self._pending_tasks.discard)
        return task

    def _on_rf_event(self, event):
        """Called for every RFEvent arriving from any C++ node."""
        # Frequency blacklist gate — operator-marked freqs (regulatory
        # off-limits, known interferer, our own beacons) are dropped at
        # ingest. Counted so the operator can see the muting working.
        if self.overrides.is_blacklisted(event.frequency):
            metrics.counter("predator_events_blacklisted_total",
                             help_text="Events dropped by operator blacklist")
            return

        metrics.counter("predator_events_ingested_total",
                         labels={"node": event.node_id},
                         help_text="RF events ingested into pipeline")

        # Feed to baseline
        self.baseline.observe(event)

        # Persist the raw event (fire-and-forget; failure logged by store)
        if self.store is not None:
            self._spawn(self.store.record_event(event.to_dict()))

        # Fuse into tracks
        track = self.track_manager.ingest(event)

        # Run anomaly detection on updated track
        flags = self.anomaly_detector.analyze(track, event)
        if flags:
            track.anomaly_flags = [f.description for f in flags]
            logger.info("Anomaly on track %s: %s",
                        track.emitter_id[:8],
                        ", ".join(f.description for f in flags))

        # Produce + persist an assessment so the tasking loop (T002) and
        # CoT exporter (T004) have something to consume. Was previously
        # dead code — DecisionEngine was instantiated but never invoked.
        report = self.decision_engine.assess(
            track, anomaly_flags=flags or [],
            available_nodes=list(self.track_manager.sensor_nodes.values()))

        # TDOA: record this node's hearing of the emitter, then attempt a
        # solve if we now have ≥2 distinct nodes within the time window.
        if self.tdoa is not None:
            node = self.track_manager.sensor_nodes.get(event.node_id)
            if node is not None:
                self.tdoa.record_measurement(
                    track.emitter_id, node, event.timestamp_ns)
                self._spawn(self._try_tdoa_solve(track.emitter_id))

        if self.store is not None:
            self._spawn(self.store.record_track(track.to_dict()))
            self._spawn(self.store.record_assessment(report.to_dict()))

        # AutoTasker — react to the assessment by re-tuning recommended
        # nodes to this emitter's frequency for closer inspection.
        self.auto_tasker.handle_assessment(track.to_dict(), report.to_dict())

        # CoT/TAK escalation — gated by config.cot_enabled AND
        # report.escalate_to_atak AND (when COT_REQUIRE_MANUAL_APPROVAL)
        # operator approval via /api/v1/approvals/{id}/approve. Friendly-
        # listed emitters never escalate. Use the detecting node's GPS
        # as a fallback location so high-threat tracks without a TDOA
        # fix still produce a "near node X" marker on the TAK map.
        if (self.cot.enabled and report.escalate_to_atak
                and not self.overrides.is_friendly(track.emitter_id)):
            fallback = None
            node = self.track_manager.sensor_nodes.get(event.node_id)
            if node and node.location_gps:
                fallback = (node.location_gps[0], node.location_gps[1])
            track_d = self.overrides.apply_to_track(track.to_dict())
            if config.cot_require_manual_approval:
                # Two-key gate: enqueue for operator review. The actual
                # cot.emit_track() call happens in _on_approval_decision
                # when the operator clicks Approve in the UI.
                self._spawn(self.approvals.enqueue(
                    track_d, report.to_dict(), fallback))
            else:
                self._spawn(self.cot.emit_track(
                    track_d, report.to_dict(),
                    fallback_location=fallback))

        # Publish to SSE subscribers
        push_event(event.to_dict())

    async def _try_tdoa_solve(self, emitter_id: str,
                               max_age_s: float = 5.0):
        """Prune stale measurements, then run TDOA if ≥2 distinct nodes
        remain. On success, write the location estimate back to the
        in-memory track and re-persist."""
        if self.tdoa is None:
            return
        self.tdoa.prune_old(emitter_id, max_age_s=max_age_s)
        if self.tdoa.distinct_nodes(emitter_id) < 2:
            return
        try:
            result = await self.tdoa.solve(emitter_id)
        except Exception as exc:
            logger.debug("TDOA solve failed for %s: %s", emitter_id, exc)
            return
        if result is None:
            return
        track = self.track_manager.tracks.get(emitter_id)
        if track is None:
            return
        track.estimated_lat = result.estimated_lat
        track.estimated_lon = result.estimated_lon
        track.location_confidence = result.location_confidence
        # TDOA wins over any prior RSSI-proximity fallback — tag the
        # method so the UI can switch the marker style. Mirror the
        # ellipse-radius scaling used by the renderer (50 m at conf=1
        # → 5 km at conf=0).
        track.location_method = "tdoa"
        track.location_error_radius_m = 50.0 + (
            1.0 - max(0.0, min(1.0, result.location_confidence))) * 4950.0
        # Store the actual 1-sigma error ellipse so the Android map can
        # render it accurately instead of approximating with a circle.
        track.tdoa_ellipse_a_m = result.ellipse_a_m
        track.tdoa_ellipse_b_m = result.ellipse_b_m
        track.tdoa_ellipse_theta_deg = result.ellipse_theta_deg
        logger.info("Track %s located: (%.5f, %.5f) conf=%.2f via %d nodes",
                    emitter_id[:8], result.estimated_lat, result.estimated_lon,
                    result.location_confidence,
                    len(result.participating_nodes))
        if self.store is not None:
            await self.store.record_track(track.to_dict())

    async def _on_approval_decision(self, approval) -> None:
        """Drain hook from ApprovalQueue.on_approved: an operator-
        approved item becomes an actual CoT push. The audit row is
        written separately by `_on_approval_terminal` (which fires for
        every terminal state, not just approved)."""
        try:
            await self.cot.emit_track(approval.track, approval.report,
                fallback_location=approval.fallback_location)
        except Exception as exc:
            logger.warning("Approved CoT push failed for %s: %s",
                           approval.approval_id[:8], exc)

    async def _on_approval_terminal(self, approval) -> None:
        """Persist EVERY terminal approval transition. The mission_id
        was snapshotted at enqueue time and lives on the approval; we
        prefer that over `self.missions.active_id` so a mission roll
        during the approval window doesn't mis-attribute the audit
        row."""
        if self.store is None:
            return
        payload = approval.to_dict()
        if not payload.get("mission_id"):
            payload["mission_id"] = self.missions.active_id
        try:
            await self.store.record_approval(payload)
        except Exception as exc:
            logger.warning("record_approval failed for %s (%s): %s",
                           approval.approval_id[:8], approval.state, exc)

    def _on_rns_inbound_cot(self, xml: bytes, src_hash16: str) -> None:
        """Hand inbound CoT XML received over RNS to the local pipeline
        AND, when configured, forward the raw XML to a local ATAK app
        over UDP (e.g. Android ATAK on 127.0.0.1:4242). The ATAK
        forward is fire-and-forget — it must never break the inbound
        path. Tagged `source_transport="rns"` and `_upstream="rns:<hash>"`
        so downstream dedupe + persistence can attribute it correctly.
        """
        # Local ATAK forward (UDP). Spec: peer-relayed CoT must reach
        # the device's local TAK app so operators see what the mesh
        # delivered, not just what their own sensor produced.
        port = int(getattr(self.config, "rns_atak_local_port", 0) or 0)
        if port > 0:
            try:
                if not hasattr(self, "_atak_local_sock"):
                    import socket as _s
                    self._atak_local_sock = _s.socket(
                        _s.AF_INET, _s.SOCK_DGRAM)
                    self._atak_local_sock.setblocking(False)
                host = getattr(self.config, "rns_atak_local_host",
                               "127.0.0.1") or "127.0.0.1"
                self._atak_local_sock.sendto(xml, (host, port))
            except Exception as exc:
                logger.debug("RNS→ATAK local UDP forward failed: %s", exc)
        try:
            ev = {
                "source_transport": "rns",
                "_upstream": f"rns:{src_hash16}",
                "raw": xml.decode("utf-8", errors="replace"),
                "frequency": 0,
            }
            push_event(ev)
            if self.coc is not None:
                self.coc.feed_event(ev, source=f"rns:{src_hash16}")
        except Exception as exc:
            logger.debug("RNS inbound CoT handling failed: %s", exc)

    def _on_remote_event(self, ev_dict: dict) -> None:
        """Bridge from CoCAggregator (which delivers dicts) into the
        same per-event pipeline used by the local fleet (which delivers
        RFEvent objects). We rehydrate the dict back into an RFEvent
        and route through `_on_rf_event`. CoC provenance — `_upstream`
        from the aggregator, or `upstream_source` already on the event
        if it was relayed through multiple CoC layers — is preserved
        end-to-end via the dedicated RFEvent.upstream_source field."""
        try:
            from backend.models.rf_event import RFEvent
            # Pull provenance out before constructing — `_upstream` is
            # the aggregator's tag, `upstream_source` may already be set
            # if a peer CoC station relayed this event from yet another
            # field station. Prefer the deepest origin.
            upstream = ev_dict.get("upstream_source") or ev_dict.get("_upstream")
            clean = {k: v for k, v in ev_dict.items()
                     if not k.startswith("_") and k != "upstream_source"}
            if hasattr(RFEvent, "from_dict"):
                ev = RFEvent.from_dict(clean)
            else:
                ev = RFEvent(**clean)
            if upstream:
                ev.upstream_source = upstream
        except Exception as exc:
            logger.debug("CoC: unable to rehydrate upstream event: %s", exc)
            return
        self._on_rf_event(ev)

    def _on_new_track(self, track):
        logger.info("New track: %s at %.4f MHz",
                    track.emitter_id[:8], track.primary_frequency / 1e6)

    def _on_track_update(self, track):
        metrics.gauge("predator_track_confidence",
                      track.confidence,
                      labels={"emitter": track.emitter_id[:8]},
                      help_text="Current confidence score for an emitter track")

    async def start(self):
        logger.info("Predator-SDR Backend starting...")
        # Operator-visible gate banner — at-a-glance check of which
        # outbound surfaces are armed. Critical for field deployments
        # where the operator must KNOW whether AutoTasker / CoT will
        # take any active action. Both default OFF (RX-only posture).
        logger.info(
            "GATES — persistence=%s tdoa=%s cot=%s auto_tasker=%s coc=%s",
            "on" if self.store is not None else "off",
            "on" if self.tdoa is not None else "off",
            "ARMED" if self.cot.enabled else "off",
            "ARMED" if self.auto_tasker.enabled else "off",
            f"on({len(config.parse_coc_upstream_urls())} upstream)"
                if self.coc is not None else "off")

        # Rehydrate active tracks from the previous mission (if any) so a
        # mid-mission restart doesn't lose context.
        if self.store is not None:
            self._rehydrate_tracks()

        # Register fleet nodes from config
        for node in config.parse_fleet_nodes():
            await self.fleet_manager.add_node(node)
            self.track_manager.register_node(node)
            logger.info("Fleet node registered: %s (%s)",
                        node.node_id, node.hardware_code)

        if self.fleet_manager.node_count() == 0 and self.coc is None:
            logger.warning("No fleet nodes configured AND CoC mode is off. "
                           "Set FLEET_NODES env var or register via API, "
                           "or enable COC_MODE_ENABLED + COC_UPSTREAM_URLS.")

        # Start the CoC aggregator (if configured). Each upstream gets
        # its own consumer task registered in _pending_tasks via _spawn.
        if self.coc is not None:
            await self.coc.start()

        # Start the RNS daemon (if configured). Soft no-op when the
        # `rns` Python package isn't importable — daemon reports
        # `daemon=stub` in that case.
        if self.rns_daemon is not None:
            try:
                self.rns_daemon.start()
                logger.info("RNS daemon started: id=%s",
                            self.rns_daemon.identity_hash16())
            except Exception as exc:
                logger.error("RNS daemon start failed: %s", exc)
            # Local-only Unix-socket control plane — used by both
            # the Linux GUI Kujhad sub-panel (kujhad_rns.h) and the
            # Android RnsBridge.kt (via android.net.LocalSocket).
            # This is the ONLY control surface; no HTTP route is
            # mounted on the FastAPI server (see backend/api/server.py).
            # Best-effort start; failure here is logged but the daemon
            # itself keeps running.
            try:
                from backend.rns.daemon import ControlServer
                self._rns_control = ControlServer(self.rns_daemon)
                self._rns_control.start()
                logger.info("RNS control socket: %s",
                            self._rns_control.sock_path)
            except Exception as exc:
                logger.warning("RNS control socket start failed: %s", exc)
                self._rns_control = None

        # Background maintenance tasks
        asyncio.create_task(
            self.track_manager.maintenance_loop(config.track_maintenance_interval_s))
        asyncio.create_task(self._merge_loop())
        asyncio.create_task(self._baseline_prune_loop())
        # Cross-station dedup runs only when CoC is on (otherwise every
        # track is local and the dedup pass is a no-op).
        if self.coc is not None:
            asyncio.create_task(self._dedup_loop())
        # Approval-queue housekeeping: expire stale items so the UI
        # doesn't show a 6-hour-old escalation as still actionable.
        asyncio.create_task(self._approval_expiry_loop())

        logger.info("Backend started. %d node(s) in fleet.",
                    self.fleet_manager.node_count())

    async def stop(self):
        logger.info("Backend stopping...")
        # Stop accepting new RF events first so no fresh tasks spawn
        # while we drain. Includes the CoC upstream consumers.
        if self.coc is not None:
            await self.coc.stop()
        await self.fleet_manager.stop_all()

        # Drain in-flight persistence/CoT/TDOA/AutoTasker tasks. We give
        # them a bounded window — a hung remote (TAK server, Kujhad node)
        # must not block shutdown forever.
        if self._pending_tasks:
            pending = list(self._pending_tasks)
            logger.info("Draining %d in-flight task(s) before close...",
                        len(pending))
            try:
                done, still_pending = await asyncio.wait(
                    pending, timeout=config.shutdown_drain_timeout_s)
                if still_pending:
                    logger.warning(
                        "%d task(s) did not finish in %.1fs — cancelling",
                        len(still_pending), config.shutdown_drain_timeout_s)
                    for t in still_pending:
                        t.cancel()
                    await asyncio.gather(*still_pending, return_exceptions=True)
            except Exception as exc:
                logger.error("Error draining tasks: %s", exc)

        if self.rns_daemon is not None:
            try:
                self.rns_daemon.stop()
            except Exception as exc:
                logger.warning("RNS daemon stop failed: %s", exc)
            ctrl = getattr(self, "_rns_control", None)
            if ctrl is not None:
                try:
                    ctrl.stop()
                except Exception as exc:
                    logger.warning("RNS control socket stop failed: %s", exc)

        # Now safe to close the persistent backends.
        if self.store is not None:
            self.store.close()
        self.cot.close()
        logger.info("Backend stopped cleanly.")

    def _rehydrate_tracks(self):
        """Replay open tracks from the mission DB into the in-memory
        TrackManager. Tracks load with their persisted state (confidence,
        observation_count, location, etc.) so the very next event for a
        known emitter associates with the prior track instead of spawning
        a fresh one."""
        from backend.models.emitter_track import EmitterTrack, TrackState
        rows = self.store.load_active_tracks(
            window_s=config.track_replay_window_hours * 3600.0)
        for r in rows:
            try:
                state = TrackState(r.get("state", "new"))
            except ValueError:
                state = TrackState.NEW
            tr = EmitterTrack(
                emitter_id=r["emitter_id"],
                state=state,
                primary_frequency=float(r.get("primary_frequency", 0.0)),
                last_power_dbfs=r.get("last_power_dbfs"),
                first_seen_ns=int(r.get("first_seen_ns", 0)),
                last_seen_ns=int(r.get("last_seen_ns", 0)),
                observation_count=int(r.get("observation_count", 0)),
                confidence=float(r.get("confidence") or 0.0),
                threat_level=r.get("threat_level") or "unknown",
                modulation=r.get("modulation"),
                protocol=r.get("protocol"),
                estimated_lat=r.get("estimated_lat"),
                estimated_lon=r.get("estimated_lon"),
                location_confidence=float(r.get("location_confidence") or 0.0),
                detecting_nodes=r.get("detecting_nodes") or [],
                anomaly_flags=r.get("anomaly_flags") or [],
            )
            self.track_manager.tracks[tr.emitter_id] = tr
            self.track_manager._associator.index_track(tr)
        if rows:
            logger.info("Rehydrated %d active track(s) from mission DB", len(rows))

    async def _merge_loop(self):
        while True:
            await asyncio.sleep(config.track_merge_interval_s)
            self.track_manager.merge_duplicates()

    async def _baseline_prune_loop(self):
        prune_interval_s = config.baseline_prune_interval_hours * 3600
        while True:
            await asyncio.sleep(prune_interval_s)
            self.baseline.prune_stale()

    async def _dedup_loop(self):
        """Periodic cross-station emitter dedup. Coalesces tracks for
        the same physical emitter heard by both the local fleet and
        upstream CoC peers."""
        while True:
            await asyncio.sleep(config.coc_dedup_interval_s)
            try:
                self.dedup.run(self.track_manager)
            except Exception as exc:
                logger.warning("Cross-station dedup pass failed: %s", exc)

    async def _approval_expiry_loop(self):
        """Mark stale CoT-approval items as expired every minute. Stops
        the UI from listing 2-hour-old escalations as actionable."""
        while True:
            await asyncio.sleep(60.0)
            try:
                await self.approvals.expire_stale()
            except Exception as exc:
                logger.debug("Approval expiry pass failed: %s", exc)


async def main():
    backend = PredatorBackend()
    await backend.start()

    # Build FastAPI app with injected dependencies. We pass the full
    # backend so health/missions/approvals/overrides routes can reach
    # the subsystems they need without a service-locator pattern.
    from backend.api.server import create_app
    app = create_app(
        track_manager=backend.track_manager,
        fleet_manager=backend.fleet_manager,
        decision_engine=backend.decision_engine,
        backend=backend,
        rns_daemon=backend.rns_daemon,
    )

    import uvicorn
    server_config = uvicorn.Config(
        app=app,
        host=config.api_host,
        port=config.api_port,
        log_level=config.log_level.lower(),
        workers=config.api_workers,
    )
    server = uvicorn.Server(server_config)

    # Graceful shutdown on SIGINT/SIGTERM
    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, lambda: asyncio.create_task(_shutdown(backend, server)))

    logger.info("API server starting on http://%s:%d", config.api_host, config.api_port)
    await server.serve()


async def _shutdown(backend, server):
    await backend.stop()
    server.should_exit = True


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
