"""GET /api/v1/snapshot — pull a consistent single-file SQLite
snapshot of the mission DB.

Coordinator failure recovery: a pre-designated standby kit fetches
this periodically (see deploy/fetch_snapshot.sh) so that if the
coordinator RPi dies, the backup coordinator starts from recent
mission state instead of empty. The snapshot is produced with
`VACUUM INTO`, so it is transactionally consistent even while the
backend keeps writing, and it is a plain mission.db file — drop it
at DATA_DIR/mission.db on the backup kit and start the backend.

Also exposes GET /api/v1/snapshot/info — cheap metadata probe
(row counts, DB size) the standby kit can use to decide whether a
fresh pull is worth the bandwidth on a slow link.
"""
from __future__ import annotations

import logging
import time

logger = logging.getLogger(__name__)

try:
    from fastapi import APIRouter, HTTPException
    from fastapi.responses import Response

    router = APIRouter()

    # Injected by api/server.py
    store = None  # MissionStore

    @router.get("")
    async def get_snapshot():
        """Stream a consistent SQLite snapshot of the mission DB."""
        if store is None:
            raise HTTPException(503, "mission store not configured")
        try:
            blob = await store.snapshot_to_bytes()
        except Exception as exc:
            logger.warning("snapshot export failed: %s", exc)
            raise HTTPException(500, f"snapshot failed: {exc}")
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        fname = f"mission-snapshot-{stamp}.db"
        return Response(
            content=blob,
            media_type="application/vnd.sqlite3",
            headers={
                "Content-Disposition": f"attachment; filename={fname}",
                "X-Snapshot-Ts": str(int(time.time())),
            })

    @router.get("/info")
    async def get_snapshot_info():
        """Metadata probe — lets the standby kit skip a pull when
        nothing changed since the last one."""
        if store is None:
            raise HTTPException(503, "mission store not configured")
        return {
            "ts": int(time.time()),
            "db_path": store.db_path,
            "events": store.event_count(),
            "tracks": store.track_count(),
            "assessments": store.assessment_count(),
        }

except ImportError:
    # FastAPI not available in the test/lab env.
    router = None  # type: ignore
    store = None  # type: ignore
