"""Outbound emitters — CoT/TAK, syslog, etc."""
from .cot_emitter import CoTEmitter, build_cot_xml

__all__ = ["CoTEmitter", "build_cot_xml"]
