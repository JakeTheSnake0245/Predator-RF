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

        # Wire event callback
        self.fleet_manager.on_event(self._on_rf_event)
        self.track_manager.on_new_track(self._on_new_track)

    def _on_rf_event(self, event):
        """Called for every RFEvent arriving from any C++ node."""
        # Feed to baseline
        self.baseline.observe(event)

        # Fuse into tracks
        track = self.track_manager.ingest(event)

        # Run anomaly detection on updated track
        flags = self.anomaly_detector.analyze(track, event)
        if flags:
            logger.info("Anomaly on track %s: %s",
                        track.emitter_id[:8],
                        ", ".join(f.description for f in flags))

        # Publish to SSE subscribers
        push_event(event.to_dict())

    def _on_new_track(self, track):
        logger.info("New track: %s at %.4f MHz",
                    track.emitter_id[:8], track.primary_frequency / 1e6)

    async def start(self):
        logger.info("Predator-SDR Backend starting...")

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
        await self.fleet_manager.stop_all()

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
