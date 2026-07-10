"""
IQCaptureService — operator-demand IQ capture coordinator.

When the operator taps "Capture IQ" on a hit, this service:
  1. Sends a timed capture command to predator-rfd via /v1/command
  2. Waits for the capture-complete event on the SSE stream
  3. Downloads the .cs8 file from the node
  4. Records the capture in the SignalRepository

Command wire format (sent to predator-rfd POST /v1/command):
    {"class": "iq_capture", "action": "start",
     "args": {"freq_hz": 433920000, "duration_s": 5.0,
              "sample_rate_hz": 2400000, "signal_id": "uuid"}}

predator-rfd responds with {"ok": true, "capture_id": "...", "file": "..."}
The captured file is available at GET /api/iq/<capture_id>
"""
from __future__ import annotations

import logging
import uuid
from typing import Optional

logger = logging.getLogger(__name__)

IQ_CAPTURE_TIMEOUT_S = 30.0


class IQCaptureService:
    def __init__(self,
                 signal_repo,
                 fleet_manager=None,
                 default_duration_s: float = 5.0,
                 default_sample_rate_hz: float = 2.4e6):
        self._repo       = signal_repo
        self._fleet      = fleet_manager
        self._duration   = default_duration_s
        self._sample_rate= default_sample_rate_hz

    async def capture(self,
                      node_id: str,
                      freq_hz: float,
                      signal_id: Optional[str] = None,
                      duration_s: Optional[float] = None,
                      sample_rate_hz: Optional[float] = None,
                      captured_by: str = "operator") -> Optional[str]:
        """
        Trigger an IQ capture on node_id for freq_hz.
        Returns the capture_id on success, None on failure.
        """
        dur  = duration_s    or self._duration
        sr   = sample_rate_hz or self._sample_rate
        sid  = signal_id or str(uuid.uuid4())

        if self._fleet is None:
            logger.warning("IQCaptureService: no fleet manager — cannot send capture command")
            return None

        client = self._fleet.get_client(node_id)
        if client is None:
            logger.warning("IQCaptureService: node %s not found in fleet", node_id)
            return None

        try:
            ok = await client.send_iq_capture_command(freq_hz, dur, sr, sid)
        except Exception as exc:
            logger.error("IQCaptureService: capture command failed on %s: %s", node_id, exc)
            return None

        if not ok:
            logger.warning("IQCaptureService: node %s rejected capture command", node_id)
            return None

        file_path = f"node:{node_id}:iq:{sid}.cs8"
        capture_id = self._repo.record_iq_capture(
            signal_id=sid,
            file_path=file_path,
            duration_s=dur,
            sample_rate_hz=sr,
            center_hz=freq_hz,
            captured_by=captured_by,
        )
        logger.info(
            "IQCaptureService: capture queued node=%s freq=%.3f MHz dur=%.1fs capture_id=%s",
            node_id, freq_hz / 1e6, dur, capture_id)
        return capture_id

    async def async_capture(self, *args, **kwargs) -> Optional[str]:
        return await self.capture(*args, **kwargs)
