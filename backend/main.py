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
from backend.coordination.kujhad_client import KujhadFleetManager
from backend.intelligence.anomaly_detector import AnomalyDetector
from backend.intelligence.decision_engine import DecisionEngine
from backend.intelligence.rf_baseline import RFBaseline
from backend.api.routes.events import push_event
from backend.persistence import MissionStore
from backend.fusion.tdoa_coordinator import TDOACoordinator
from backend.output import CoTEmitter
from backend.coordination.auto_tasker import AutoTasker

logging.basicConfig(
    level=getattr(logging, config.log_level, logging.INFO),
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("predator.backend")


class PredatorBackend:
    """Top-level service orchestrator."""

    def __init__(self):
        self.baseline = RFBaseline(
            learning_window_hours=config.baseline_learning_window_hours)
        self.anomaly_detector = AnomalyDetector(self.baseline)
        self.decision_engine = DecisionEngine(self.anomaly_detector)
        self.track_manager = TrackManager()
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

        # Background task accounting — every fire-and-forget task spawned
        # from `_on_rf_event` (persistence writes, TDOA solves, CoT emits,
        # AutoTasker tunes) is registered here so `stop()` can drain them
        # before closing the DB / UDP socket. Without this, a SIGTERM
        # mid-mission would race with in-flight writes and lose the last
        # few seconds of data — exactly when an operator most wants it.
        self._pending_tasks: set[asyncio.Task] = set()

        # Wire event callback
        self.fleet_manager.on_event(self._on_rf_event)
        self.track_manager.on_new_track(self._on_new_track)

    def _spawn(self, coro) -> asyncio.Task:
        """Schedule a coroutine and track it for shutdown drain."""
        task = asyncio.create_task(coro)
        self._pending_tasks.add(task)
        task.add_done_callback(self._pending_tasks.discard)
        return task

    def _on_rf_event(self, event):
        """Called for every RFEvent arriving from any C++ node."""
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
        # report.escalate_to_atak. Use the detecting node's GPS as a
        # fallback location so high-threat tracks without a TDOA fix
        # still produce a "near node X" marker on the TAK map.
        if self.cot.enabled and report.escalate_to_atak:
            fallback = None
            node = self.track_manager.sensor_nodes.get(event.node_id)
            if node and node.location_gps:
                fallback = (node.location_gps[0], node.location_gps[1])
            self._spawn(self.cot.emit_track(
                track.to_dict(), report.to_dict(),
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
        logger.info("Track %s located: (%.5f, %.5f) conf=%.2f via %d nodes",
                    emitter_id[:8], result.estimated_lat, result.estimated_lon,
                    result.location_confidence,
                    len(result.participating_nodes))
        if self.store is not None:
            await self.store.record_track(track.to_dict())

    def _on_new_track(self, track):
        logger.info("New track: %s at %.4f MHz",
                    track.emitter_id[:8], track.primary_frequency / 1e6)

    async def start(self):
        logger.info("Predator-SDR Backend starting...")
        # Operator-visible gate banner — at-a-glance check of which
        # outbound surfaces are armed. Critical for field deployments
        # where the operator must KNOW whether AutoTasker / CoT will
        # take any active action. Both default OFF (RX-only posture).
        logger.info(
            "GATES — persistence=%s tdoa=%s cot=%s auto_tasker=%s",
            "on" if self.store is not None else "off",
            "on" if self.tdoa is not None else "off",
            "ARMED" if self.cot.enabled else "off",
            "ARMED" if self.auto_tasker.enabled else "off")

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

        if self.fleet_manager.node_count() == 0:
            logger.warning("No fleet nodes configured. "
                           "Set FLEET_NODES env var or register via API.")

        # Background maintenance tasks
        asyncio.create_task(
            self.track_manager.maintenance_loop(config.track_maintenance_interval_s))
        asyncio.create_task(self._merge_loop())
        asyncio.create_task(self._baseline_prune_loop())

        logger.info("Backend started. %d node(s) in fleet.",
                    self.fleet_manager.node_count())

    async def stop(self):
        logger.info("Backend stopping...")
        # Stop accepting new RF events first so no fresh tasks spawn
        # while we drain.
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


async def main():
    backend = PredatorBackend()
    await backend.start()

    # Build FastAPI app with injected dependencies
    from backend.api.server import create_app
    app = create_app(
        track_manager=backend.track_manager,
        fleet_manager=backend.fleet_manager,
        decision_engine=backend.decision_engine,
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
