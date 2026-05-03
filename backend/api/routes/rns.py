"""UNUSED scaffolding — DO NOT MOUNT (task #27).

Per spec section F and the threat model, the RNS daemon control
plane is local-only on every platform:

  * Linux GUI (Kujhad panel) → Unix socket via
    `backend/rns/daemon.py::ControlServer` (uid-checked, 0600).
  * Android UI (`RnsBridge.kt`) → same Unix socket via
    `android.net.LocalSocket`.

This module remains as importable scaffolding only so that existing
unit tests (`backend/tests/test_rns_daemon_api.py`) that exercise
the handler logic in isolation continue to pass, and so a future
opt-in mode could re-enable an HTTP control plane behind explicit
configuration. **`backend/api/server.py` does NOT mount this
router** — the daemon must never be reachable over the network.
"""
from __future__ import annotations

from typing import Any, Dict, Optional

daemon = None  # Injected by server.py from PredatorBackend.rns_daemon.
router = None

try:
    from fastapi import APIRouter, HTTPException
    from pydantic import BaseModel
except Exception:  # pragma: no cover
    pass
else:
    router = APIRouter()

    def _need():
        if daemon is None:
            raise HTTPException(status_code=503,
                                detail="RNS daemon not configured "
                                       "(set RNS_ENABLED=1 and restart)")
        return daemon

    class InterfaceCfg(BaseModel):
        cfg: Dict[str, Any]

    class UpdateInterfaceCfg(BaseModel):
        cfg: Dict[str, Any]

    class EnabledFlag(BaseModel):
        enabled: bool

    class ExportReq(BaseModel):
        passphrase: str
        include_identity: bool = True

    class ImportReq(BaseModel):
        token: str
        passphrase: str
        placeholders: Optional[Dict[str, Any]] = None

    class MintReq(BaseModel):
        new_passphrase: str
        include_identity: bool = False

    @router.get("/status")
    async def status():
        return _need().status()

    @router.get("/interfaces")
    async def list_interfaces():
        return _need().list_interfaces()

    @router.get("/interfaces/{iid}")
    async def get_interface(iid: str):
        rv = _need().get_interface(iid)
        if rv is None:
            raise HTTPException(status_code=404, detail="unknown interface")
        return rv

    @router.post("/interfaces")
    async def add_interface(body: InterfaceCfg):
        try:
            return _need().add_interface(body.cfg)
        except Exception as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.put("/interfaces/{iid}")
    async def update_interface(iid: str, body: UpdateInterfaceCfg):
        try:
            return _need().update_interface(iid, body.cfg)
        except Exception as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.delete("/interfaces/{iid}")
    async def remove_interface(iid: str):
        return {"removed": _need().remove_interface(iid)}

    @router.post("/interfaces/{iid}/enabled")
    async def set_enabled(iid: str, body: EnabledFlag):
        try:
            return _need().set_enabled(iid, body.enabled)
        except Exception as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.post("/interfaces/{iid}/restart")
    async def restart_interface(iid: str):
        try:
            return _need().restart_interface(iid)
        except Exception as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.post("/restart_all")
    async def restart_all():
        return _need().restart_all()

    @router.post("/validate")
    async def validate(body: InterfaceCfg):
        try:
            return _need().validate_interface(body.cfg)
        except Exception as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.post("/export")
    async def export_config(body: ExportReq):
        try:
            return _need().export_config(body.passphrase,
                                         body.include_identity)
        except Exception as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.post("/import")
    async def import_config(body: ImportReq):
        try:
            return _need().import_config(body.token, body.passphrase,
                                         body.placeholders or {})
        except Exception as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.post("/mint")
    async def mint(body: MintReq):
        try:
            return _need().mint_replication_token(body.new_passphrase,
                                                  body.include_identity)
        except Exception as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.get("/logs")
    async def logs(level: str = "INFO", since_ms: int = 0,
                   limit: int = 200):
        return _need().get_logs(level, since_ms, limit)

    @router.get("/schema")
    async def schema():
        """Returns the locked field schema (section B of task-27.md) so
        the Android UI can render forms without hard-coding the field
        list."""
        from backend.rns.schema import (
            COMMON_FIELDS, PER_TYPE_FIELDS, DEVICE_LOCAL_FIELDS,
        )

        def serialize_fields(fields):
            out = []
            for name, typ, required, _validator in fields:
                t = (typ.__name__ if isinstance(typ, type) else "any")
                out.append({"name": name, "type": t, "required": required})
            return out

        return {
            "common": serialize_fields(COMMON_FIELDS),
            "per_type": {k: serialize_fields(v)
                         for k, v in PER_TYPE_FIELDS.items()},
            "device_local": {k: list(v)
                             for k, v in DEVICE_LOCAL_FIELDS.items()},
        }
