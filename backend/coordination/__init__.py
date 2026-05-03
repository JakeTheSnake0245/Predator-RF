"""Coordination layer — fleet management, mode selection, auto-tasking.

Lazy re-exports: importing `AdaptiveModeSelector` pulls in `backend.sensor`
which depends on numpy. Modules in this package that don't need numpy
(e.g. `auto_tasker`, `kujhad_client`) must be importable on a stripped-down
host (a bare RPi without numpy). __getattr__ defers the heavy import until
the symbol is actually requested, while keeping the public API intact for
existing call sites.
"""
from .kujhad_client import KujhadClient, KujhadFleetManager
from .auto_tasker import AutoTasker

__all__ = ["KujhadClient", "KujhadFleetManager", "AutoTasker",
           "AdaptiveModeSelector"]


def __getattr__(name):
    if name == "AdaptiveModeSelector":
        from .mode_selector import AdaptiveModeSelector
        return AdaptiveModeSelector
    raise AttributeError(f"module 'backend.coordination' has no attribute {name!r}")
