"""`python -m backend.rns` — start the RNS daemon.

Honors:
  PREDATOR_RNS_STATE_DIR  — config + identity directory
  PREDATOR_RNS_SOCKET     — override default control socket path
"""
from __future__ import annotations

import argparse
import os
import sys

from .daemon import run_daemon


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="predator-rns")
    parser.add_argument("--state-dir",
                        default=os.environ.get("PREDATOR_RNS_STATE_DIR"))
    parser.add_argument("--socket",
                        default=os.environ.get("PREDATOR_RNS_SOCKET"))
    args = parser.parse_args(argv)
    run_daemon(state_dir=args.state_dir, sock_path=args.socket)
    return 0


if __name__ == "__main__":
    sys.exit(main())
