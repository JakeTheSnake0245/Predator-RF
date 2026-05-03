"""CoT approval-queue endpoints. Operator UI lists pending items, then
POSTs to {id}/approve or {id}/reject."""
from __future__ import annotations

from typing import Any, Dict

from fastapi import APIRouter, HTTPException, Body

router = APIRouter()

# Injected by api/server.py
queue = None  # ApprovalQueue


@router.get("")
async def list_pending():
    if queue is None:
        return []
    return await queue.list_pending()


@router.get("/stats")
async def stats():
    if queue is None:
        return {}
    return queue.stats()


@router.post("/{approval_id}/approve")
async def approve(approval_id: str,
                  body: Dict[str, Any] = Body(default={})):
    if queue is None:
        raise HTTPException(503, "approval queue not configured")
    operator = body.get("operator") or "operator"
    item = await queue.approve(approval_id, operator=operator)
    if item is None:
        raise HTTPException(404, "no such pending approval")
    return item.to_dict()


@router.post("/{approval_id}/reject")
async def reject(approval_id: str,
                 body: Dict[str, Any] = Body(default={})):
    if queue is None:
        raise HTTPException(503, "approval queue not configured")
    operator = body.get("operator") or "operator"
    reason = body.get("reason") or ""
    ok = await queue.reject(approval_id, reason=reason, operator=operator)
    if not ok:
        raise HTTPException(404, "no such pending approval")
    return {"approval_id": approval_id, "state": "rejected"}
