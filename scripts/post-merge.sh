#!/bin/bash
# Post-merge setup for Predator SDR.
#
# This Replit environment runs only the Python landing page (`server.py`
# on port 5000). The actual SDR app is C++/Kotlin and builds on the user's
# machine via Gradle, not here. So all this script needs to do is:
#   1. Confirm the landing page still parses (catches any merge that
#      broke server.py syntactically).
#   2. Make sure the script itself is harmless and idempotent.
#
# Anything heavier (CMake configure, Gradle assemble, NDK toolchain) would
# be wasted cycles in Replit and is intentionally NOT run here.

set -e

echo "[post-merge] verifying landing page (server.py)…"
python -m py_compile server.py
echo "[post-merge] OK"
