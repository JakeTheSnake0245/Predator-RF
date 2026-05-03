"""GET /api/v1/preflight — run the same readiness checks the CLI
does and return JSON. Used by the operator UI's status banner so
a glance at the dashboard tells you whether the rig is GO or NO-GO."""
from __future__ import annotations

import logging
from typing import Any, Dict

logger = logging.getLogger(__name__)

try:
    from fastapi import APIRouter
    router = APIRouter(prefix="/api/v1/preflight", tags=["preflight"])

    @router.get("")
    async def get_preflight() -> Dict[str, Any]:
        """Live preflight. Cheap to call (a few socket probes + disk
        stat); intended for a once-per-30-s pull from the UI."""
        try:
            from deploy.preflight import run_all
        except ImportError as exc:
            return {"go": False, "summary": {"PASS": 0, "WARN": 0, "FAIL": 1},
                    "results": [{"check": "preflight_module",
                                  "severity": "FAIL",
                                  "message": f"can't import preflight: {exc}"}]}
        return await run_all(allow_lab=True)

except ImportError:
    # FastAPI not available in the test/lab env. The orchestrator
    # checks for `router` before mounting, so a missing one is fine.
    router = None  # type: ignore
