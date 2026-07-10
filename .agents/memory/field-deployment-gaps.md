---
name: Field deployment gap list (6-node cellular scenario)
description: Prioritized gaps found by end-to-end code trace of a multi-node field deployment; consult before hardening work or re-running deployment scenario audits.
---

# Confirmed gaps (traced against real code, mid-2026)

Prioritized fix list from the 6-operator rural emitter-hunt trace:

1. **No bearing source except KrakenSDR.** HackRF/RTL-SDR nodes produce zero LOB data; without Kraken arrays the system silently falls back to RSSI proximity (conf cap 0.20) or GPS-timing-dependent TDOA. Fix: add Kraken to kit, or harden TDOA (gate on GPS/PPS lock, require ≥3 hearers, surface "no DF hardware" to operator).
2. **No identity gate before fusion.** Track association is frequency/time only (assoc tol ~5 kHz, LOB 25 kHz, MATCH_THRESHOLD 0.4); modulation checked only in cross-station dedup's location-missing path. Two different nearby-frequency signals can fuse bearings into a fake fix. Fix: wire the signal-repository fingerprinter (cosine sim) into associator scoring / `ingest_lob` as a pre-fusion match requirement.
3. **No field-node disk persistence.** Device event ring is memory-only; `since=<serial>` catch-up recovers outages but node reboot/battery death loses everything. Fix: persist the ring on the C++ device.
4. **No coordinator failover.** SQLite WAL + `load_active_tracks()` survive a restart, but if the coordinator stays down no node is promoted; custody election is sensor-tasking only, not backend failover.
5. **Battery telemetry stubbed.** `power_pct` hardcoded 100.0 in the sense loop; AdaptiveModeSelector never sees real battery; no low-battery warning/flush. Android has thermal throttling only.
6. **No "nominate target" primitive.** Candidacy is automatic (AnomalyDetector/ConfidenceEngine); only manual gate is the CoT approval queue. No operator target-nomination action exists.
7. **No network-layer health surfacing.** A dead VPN/overlay, an expired Tailscale/Headscale node key, and a dead node are indistinguishable in the dashboard — the poll loop just logs warnings and retries every 5 s forever.

# Transport/reachability facts worth keeping

- **Kujhad is designed for a private overlay** (ZeroTier/Tailscale): plain HTTP, overlay = trust boundary; `kujhadEnumerateInterfaces()` scores ZeroTier 100 > Tailscale 90 > RFC1918 10; TLS pinning optional with OpenSSL. On a tailnet, coordinator→node IP polling works fine over cellular CGNAT (DERP fallback latency ≪ the 5–10 s poll timeouts).
- **Without an overlay, cellular-only deployments fail**: the default model is coordinator-polls-node (`FLEET_NODES` CSV or `POST /nodes/register`), which cannot reach CGNAT nodes; the LAN auto-probe only tries hotspot/AP IPs; RNS works but needs a public relay or RNode hardware.
- **Nothing waits for the overlay**: systemd units order only on `network-online.target` (not tailscaled); Python binds 0.0.0.0 by default so no bind race, but binding to a specific tailnet IP before the interface is up crash-loops until StartLimitBurst (10×5 s) is exhausted. Nothing auto-calls `/nodes/register` — registration is manual or pre-configured.
