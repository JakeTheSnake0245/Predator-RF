---
name: RPi sensor install doctrine
description: User-mandated zero-touch, hardware-agnostic rules for the Pi sensor installer; consult before touching deploy/install_rpi.sh.
---

# Baseline sensor package doctrine (user-mandated, July 2026)

The Pi installer must produce a node that "rocks and rolls with whatever is available" — no tailoring per hardware.

Rules:
- **Everything installed up front**: all SDR userspace tools (rtl-sdr, hackrf, airspy, soapy), gpsd (installed, disabled until a puck exists), Kraken DoA stack default-ON (`--no-kraken` is the opt-out, not the reverse).
- **DVB blacklist must be effective at install time** — modprobe -r now + modprobe.d + initramfs bake. Operators plug dongles in later without a reboot.
- **Zero-touch security**: API bearer token auto-generated; ufw restricts API port to Tailscale CGNAT 100.64.0.0/10 only, SSH kept open.
- **Fail closed, exit non-zero**: if the firewall can't be verified active with the tailnet rule, or the service isn't active after preflight GO, the installer must fail loudly — never report hardened/running when it isn't. Kraken stack failures are the exception: best-effort warnings, never abort baseline.
- **Tailscale client installed + brought up by the installer**; zero-touch join via `TS_AUTHKEY` (must be passed through sudo: `curl … | sudo TS_AUTHKEY=… bash`).

**Why:** field operators deploy bare Pis unattended; any manual step or silent misconfiguration becomes a dead or exposed node.
**How to apply:** any future change to deploy/install_rpi.sh keeps: idempotent re-runs, no interactive prompts, truthful exit codes.

Roles: installer default role is **sensor** (df_kracked_sensor Kujhad peer, port 9151); `--coc` installs the Python fusion backend + dashboard (port 8000). The Python backend is the COC, NOT the sensor — confusing them infuriated the user once already.
The sensor is SDR-agnostic via the sweep ingester in df_kracked_sensor/sensor.py: lsusb auto-detect → rtl_power/hackrf_sweep subprocess, median-floor + threshold hits (type "hit", detector "sweep") into the same event ring; `--sweep auto` yields to the Kraken WS when connected (a Kraken enumerates as RTL dongles and would fight rtl_power). C++ predator-rfd remains not compile-validated on real Linux.
