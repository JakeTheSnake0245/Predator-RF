---
name: AOU grid localization
description: Design rules for the grid-based transmitter AOU (probability grid) feature fusing Kraken bearings + RSSI hits
---

# AOU grid localization

- Estimator is header-only shared core (`predator::aou`), pure std — no JSON/GUI deps, so both Android and Linux builds compute it; only Android has a display bridge today (glfw/web `setAouGrid` stub false).
- **Math contract:** bearings paint Gaussian angular likelihood (azimuth = `atan2(dx,dy)`, 0°=N CW, matches the app's TRUE bearings). Bearingless SDRs (RTL/HackRF) contribute ONLY via pairwise RSSI differences between hits >150 m apart (PDOA: Δrssi ≈ −10·n·log10(d_i/d_j), n=3, σ=6 dB). A lone RSSI hit (or co-located stack) is rejected — no fake AOU.
- Flat-grid gate: peak cell must beat 8× uniform prior or the result is invalid; prevents rendering noise as a blob.
- **Why:** unknown TX power makes absolute RSSI→range meaningless; only power ratios between separated positions carry geometry.
- **How to apply:** any new observation type must be a proper likelihood over the grid; keep per-cluster cost bounded (obs caps, top-N clusters, ~0.2 Hz) since this runs on the render thread in backgroundMapTick.
- Map JS: only the operator-selected signal renders (per-marker popup dropdown → 5 kHz freq key). Marker features MUST carry numeric `freqHz` in properties — dropdown silently vanishes otherwise (review-caught bug).
- Guidance layer: estimator also emits AOU shape (mean/cov → sigma major/minor + axis bearing) and two walker waypoints — DF vantage ACROSS the long axis, RSSI walk ALONG it. Emit only when sigmaMajor ≥ 1.3× sigmaMinor (arbitrary axis on circular AOUs makes waypoints jump) and standoffs must clear the sigma of the axis they travel on. Convergence notification: one-shot below 500 m sigmaMajor, re-arm above 1 km hysteresis.
