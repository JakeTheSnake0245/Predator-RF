"""
Predator-RF Android on-device backend entry point.

Called by PredatorBackendService.kt via Chaquopy:
    Python.getInstance()
        .getModule("backend.android_service")
        .callAttr("main", filesDir)

Runs the full Predator-RF Python backend on the Android device:
  - FastAPI HTTP server at 127.0.0.1:5259  (MapActivity + C++ side)
  - RNS daemon + Unix control socket       (kujhad_rns.h + RnsBridge.kt)
  - TDOA coordinator                       (multi-sensor geolocation)
  - TrackManager                           (local emitter tracks)

The Linux entry point (backend/main.py via uvicorn CLI) is NOT modified.
This file is Android-only; it imports the same PredatorBackend class and
wires it up programmatically instead of via the CLI.
"""
from __future__ import annotations

import asyncio
import os
import sys


def main(files_dir: str) -> None:
    """
    Entry point called from PredatorBackendService with the app's filesDir
    (e.g. /data/user/0/org.sdrpp.sdrpp/files).

    Environment variables MUST be set before any backend.* import so the
    global PredatorConfig singleton is constructed with correct values.
    """
    # ── 1. Environment ────────────────────────────────────────────────
    # HOME drives all ~/.local and ~/.config paths in the daemon and RNS.
    os.environ.setdefault("HOME", files_dir)

    # Pre-set the control socket path so kujhad_rns.h and RnsBridge.kt
    # can connect even before the daemon has finished exporting it.
    rns_sock = os.path.join(
        files_dir, ".local", "state", "predator-rns", "control.sock"
    )
    os.environ["PREDATOR_RNS_SOCK"] = rns_sock

    # Enable RNS daemon (default is now True, but be explicit for clarity).
    os.environ.setdefault("RNS_ENABLED", "1")

    # ── 1b. RNS config ────────────────────────────────────────────────────
    # AutoInterface uses multicast (socket.if_nametoindex + multicast socket
    # binding), neither of which work in Chaquopy's Android Python.  Write a
    # TCPServerInterface config on loopback so RNS doesn't synthesize
    # AutoInterface at all.
    #
    # Two paths are written because we can't be certain which configdir the
    # backend passes to RNS.Reticulum() — the standard path and the XDG path
    # used by the predator-rns daemon subprocess.  Both are overwritten every
    # launch; RNS auto-creates default configs (panic_on_interface_error=Yes)
    # on first boot and the "if not exists" guard would preserve those bad ones.
    for _dir, _port in [
        (os.path.join(files_dir, ".reticulum"), 4242),
        (os.path.join(files_dir, ".config", "predator-rns", "reticulum"), 4243),
    ]:
        os.makedirs(_dir, exist_ok=True)
        with open(os.path.join(_dir, "config"), "w") as _f:
            _f.write(
                "[reticulum]\n"
                "panic_on_interface_error = No\n"
                "\n"
                "[logging]\n"
                "loglevel = 4\n"
                "\n"
                "[[LocalTCP]]\n"
                "  type = TCPServerInterface\n"
                "  interface_enabled = Yes\n"
                "  listen_ip = 127.0.0.1\n"
                f"  listen_port = {_port}\n"
            )

    # ── 1c. Writable data directory ───────────────────────────────────────
    # backend/config.py defaults DATA_DIR to "./predator_data", which
    # resolves to /predator_data on Android (cwd is /) — a read-only path.
    # Set DATA_DIR before any backend import so MissionStore uses a writable
    # location inside the app's sandboxed files directory.
    _data_dir = os.path.join(files_dir, "predator_data")
    os.makedirs(_data_dir, exist_ok=True)
    os.environ.setdefault("DATA_DIR", _data_dir)

    # Ensure the Predator-RF project root is on sys.path so that
    # `import backend.*` resolves correctly when Chaquopy's srcDirs
    # points at the repo root.
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if project_root not in sys.path:
        sys.path.insert(0, project_root)

    # ── 2. Import backend (after env is set so config reads correct values) ──
    from backend.main import PredatorBackend          # noqa: E402
    from backend.api.server import create_app         # noqa: E402

    # ── 3. Run ────────────────────────────────────────────────────────
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    async def _run() -> None:
        backend = PredatorBackend()
        await backend.start()

        app = create_app(
            track_manager=getattr(backend, "track_manager", None),
            fleet_manager=getattr(backend, "fleet_manager", None),
            decision_engine=getattr(backend, "decision_engine", None),
            backend=backend,
            rns_daemon=getattr(backend, "rns_daemon", None),
        )

        import uvicorn
        cfg = uvicorn.Config(
            app,
            host="127.0.0.1",
            port=5259,
            loop="none",      # use the already-running asyncio loop
            log_level="info",
        )
        server = uvicorn.Server(cfg)
        await server.serve()

    try:
        loop.run_until_complete(_run())
    except Exception:
        import logging
        logging.getLogger("predator.android").exception(
            "Android backend fatal error"
        )
    finally:
        loop.close()
