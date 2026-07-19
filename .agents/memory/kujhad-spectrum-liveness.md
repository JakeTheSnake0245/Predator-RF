---
name: Kujhad spectrum local-row snapshot must be unconditional
description: Why the FFT-row snapshot/serial in releaseFFTBuffer must not be gated on the device-server toggle.
---
The local FFT raw snapshot + kujhadSpectrumLocalSerial in releaseFFTBuffer are consumed by TWO clients: the device-server spectrum provider AND the overlay-mode UI fallback (liveness signal + base row).
**Why:** gating the snapshot on kujhadDeviceServerEnabled made overlay mode appear completely broken on controller-only phones (serial never advanced -> fallback misfired, base row was floor-only).
**How to apply:** any future consumer of the local spectrum cache must assume it is always fresh while the SDR runs; gate only network export on the server toggle, never the bookkeeping.

Related: never re-apply peer view geometry (setCenterFrequency) every UI frame while the operator can drag — change-detect it, or drags are snapped back and drive-by-slide is impossible.
