from typing import List, Optional
from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

router = APIRouter()
fleet_manager = None   # Injected by server.py


class NodeRegistration(BaseModel):
    node_id: str
    hardware_code: str = "rtlsdr"
    hardware_serial: str = ""
    kujhad_host: str
    kujhad_port: int = 5259
    kujhad_api_key: str = ""
    kujhad_tls: bool = False
    location_lat: Optional[float] = None
    location_lon: Optional[float] = None


@router.get("/")
async def list_nodes():
    if not fleet_manager:
        return []
    return [
        client.node.to_dict()
        for client in fleet_manager._clients.values()
    ]


@router.post("/register")
async def register_node(reg: NodeRegistration):
    if not fleet_manager:
        raise HTTPException(status_code=503, detail="Fleet manager not available")

    from backend.models.sensor_node import SensorNodeTrust
    node = SensorNodeTrust(
        node_id=reg.node_id,
        hardware_code=reg.hardware_code,
        hardware_serial=reg.hardware_serial,
        kujhad_host=reg.kujhad_host,
        kujhad_port=reg.kujhad_port,
        kujhad_api_key=reg.kujhad_api_key,
        kujhad_tls=reg.kujhad_tls,
        location_gps=(reg.location_lat, reg.location_lon)
            if reg.location_lat and reg.location_lon else None,
    )
    await fleet_manager.add_node(node)
    return {"registered": reg.node_id, "url": node.kujhad_base_url()}


@router.delete("/{node_id}")
async def remove_node(node_id: str):
    if not fleet_manager:
        raise HTTPException(status_code=503, detail="Fleet manager not available")
    await fleet_manager.remove_node(node_id)
    return {"removed": node_id}


@router.post("/{node_id}/tune")
async def tune_node(node_id: str, frequency_hz: float):
    if not fleet_manager or node_id not in fleet_manager._clients:
        raise HTTPException(status_code=404, detail="Node not found")
    ok = await fleet_manager._clients[node_id].send_tune_command(frequency_hz)
    return {"ok": ok}
