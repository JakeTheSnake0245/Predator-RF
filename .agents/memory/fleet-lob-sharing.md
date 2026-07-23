---
name: Fleet LOB sharing contract
description: How Kraken bearings travel the fleet — event wire shape, clock-skew rule, headless Python sensor peer.
---
Kraken DoA bearings ride the ordinary Kujhad `/v1/events` sync as `decoder="KRAKEN_LOB"` event rows; the controller aggregates them (local + peer) and pushes wedges to the map via `backend::setFleetLobs`. No Python fusion backend needed for wedge display.

Rules learned:
- **Freshness must use local receive time**, never the sensor's `raw.timestamp_unix` — field Pis on disconnected overlays have skewed clocks and skew-based age checks silently drop every valid bearing. Rows get a lazily-stamped `rxClock` (controller epoch) on first sight.
- **Event wire shape is load-bearing**: peer rows must match main_window's native-decoder rows exactly — `time` is a strftime `"%Y-%m-%d %H:%M:%S"` STRING (an epoch int renders as "?"), and `raw.{bearing_deg,gps_lat,gps_lon,bearing_std_deg,confidence,heading_deg,timestamp_unix}` keys are read verbatim.
- `bearing_deg` is already TRUE bearing (heading applied upstream) — never re-add `heading_deg` when rendering.
- A sensor-only Kujhad peer just needs `/v1/identify|state|gps|events` + `X-Kujhad-Key`; `/v1/command` may 501 without tripping controller liveness. Minimal `/v1/state` needs scanStatus + empty searchBands/targets/hits arrays.
- Headless Linux sensor lives in `df_kracked_sensor/` (standalone aiohttp, no backend/ imports); key file must be 0600 (O_CREAT mode + chmod beats umask).
