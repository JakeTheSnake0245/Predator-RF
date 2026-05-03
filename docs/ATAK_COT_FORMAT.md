# Predator-RF — CoT (Cursor-on-Target) XML Format

This is the EXACT XML the backend produces, both:

* over UDP via `backend/output/cot_emitter.py` (multicast or unicast
  to TAK's SA feed), and
* over HTTP via `GET /api/v1/cot/export` (pull-style for the Android
  client when multicast isn't reachable).

The Android client should treat **either source** as identical bytes.

## Per-track event

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<event version="2.0"
       uid="PREDATOR.<emitter_id>"
       type="a-u-G"
       time="2026-03-15T18:42:01.123Z"
       start="2026-03-15T18:42:01.123Z"
       stale="2026-03-15T18:47:01.123Z"
       how="m-g">
  <point lat="38.8895100" lon="-77.0353000"
         hae="9999999.0" ce="125.4" le="9999999.0"/>
  <detail>
    <contact callsign="PREDATOR-1a2b3c4d"/>
    <remarks>PREDATOR-RF HIGH | 433.9200 MHz | obs=42 | conf=0.87 | unknown 433MHz emitter near grid</remarks>
    <__group name="Cyan" role="Team Member"/>
    <precisionlocation altsrc="???" geopointsrc="GPS"/>
  </detail>
</event>
```

## Field reference

| Field | Source | Notes |
|---|---|---|
| `uid` | `f"{COT_UID_PREFIX}.{emitter_id}"` | Stable across re-emits — ATAK uses it to UPDATE markers |
| `type` | `a-u-G` (geolocated) or `b-m-p-s-p-loc` (no fix; node-position fallback) | Operator can re-style in ATAK by type |
| `time` / `start` | UTC now, ISO-8601 + `.SSS` + `Z` | Always identical for a synthetic emit |
| `stale` | `time + COT_STALE_S` (default 300 s) | ATAK auto-removes after this |
| `how` | `m-g` | Machine-derived, GPS-anchored |
| `point.lat / lon` | `EmitterTrack.estimated_lat / lon` | TDOA fix |
| `point.ce` | `50 + (1 - location_confidence) * 4950` metres | Linearly maps confidence → circular error |
| `point.hae / le` | `9999999.0` | "Unknown" sentinel — we do 2D only |
| `contact.callsign` | `f"{COT_UID_PREFIX}-{emitter_id[:8]}"` | Visible label in ATAK |
| `remarks` | freeform | `PREDATOR-RF <THREAT> \| <freq MHz> \| obs=N \| conf=X \| <summary>` |

## Bulk export envelope (HTTP only)

`GET /api/v1/cot/export` returns N events wrapped in an `<events>`
root so ATAK's "file import" reads them in one shot:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<events>
  <event version="2.0" ...>...</event>
  <event version="2.0" ...>...</event>
</events>
```

When pulling a single track (`?emitter_id=...`), the response is a
single `<event>` document — no envelope.

## Stability contract

* **The XML schema above is frozen for v2.0.0.** A future schema
  bump goes in the `version` attribute on `<event>`.
* New optional `<detail>` children may be added; the Android parser
  MUST ignore unknown children rather than fail.
* The `uid` format `PREDATOR.<emitter_id>` is stable — Android can
  parse it to recover the backend emitter ID from a TAK marker.

## Gotchas

* `lat` / `lon` are formatted with 7 decimal digits (~ 1 cm); don't
  reject on more or fewer digits.
* `ce` can be quite large (up to 5000 m) when TDOA confidence is
  low — render the error ring rather than dropping the marker.
* When multicast and HTTP-pull are BOTH active (operator using ATAK
  AND the Android Predator-RF app), the same `uid` will arrive twice
  per cycle. ATAK dedups by uid, so this is harmless — just don't
  double-count in your own tally.
* The `b-m-p-s-p-loc` type only ever appears in UDP emissions when a
  fallback (most-trustworthy node position) is supplied. The HTTP
  pull endpoint NEVER falls back — if no TDOA fix, no event.
