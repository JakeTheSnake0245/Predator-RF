# Predator-RF — Android Client Integration Guide

The Android client (Predator-RF.git, your separate Windows-built APK)
talks to this Python backend over HTTP only. There is no native socket
or shared memory — pull or subscribe via the documented endpoints
below and you have everything the operator UI shows.

## 1. Auth

Every `/api/v1/*` request:

```
Authorization: Bearer <API_BEARER_TOKEN>
```

Exception: SSE endpoints (`*/stream`) accept `?token=<token>` because
Android's `EventSource`-equivalents can't always set custom headers.
This fallback is intentionally NARROWED to SSE paths — every other
route is header-only.

If `API_BEARER_TOKEN` is unset on the backend, auth is fully open
(lab posture); the backend logs a `WARN` at startup.

## 2. Endpoints the phone needs

### 2.1 Polling snapshot (RECOMMENDED for Android)

```
GET /api/v1/android-pull?since_ns=<cursor>&max_events=200
```

* Cheap, single round-trip, gzippable.
* Send `since_ns=0` on first poll to get a full snapshot.
* Echo back the response's `cursor` field as `since_ns` next poll.
* Returns ONLY tracks/events updated since the cursor (delta sync).
* Approvals + nodes are always full (small, operator-critical).

Suggested cadence: **5 s on Wi-Fi, 15 s on cellular**. Backend
caches the preflight result for 30 s so polling more often won't
overload it.

Schema (versioned — clients MUST ignore unknown fields):

```json
{
  "schema": 2,
  "server_ts_ns": 1762000000000000000,
  "cursor": 1762000000000000000,
  "mission": {"mission_id": "...", "name": "...", "operator": "...", "started_ts_ns": ...} | null,
  "nodes": [{"node_id": "...", "trust_score": 0.95, "gps_lock": true, "gps_age_s": 1.2, "lat": ..., "lon": ..., ...}],
  "tracks": [{"emitter_id": "...", "estimated_lat": ..., "estimated_lon": ..., "confidence": 0.8, ...}],
  "events": [{"node_id": "...", "frequency": 433000000, "power_dbfs": -45.2, "timestamp_ns": ...}],
  "approvals_pending": [{"approval_id": "...", "emitter_id": "...", "expires_ns": ...}],
  "preflight_go": true
}
```

### 2.2 SSE streams (when you want push instead of pull)

```
GET /api/v1/events/stream?token=<token>     → RFEvent feed
GET /api/v1/tracks/stream?token=<token>     → track updates
```

Reconnect on close with exponential backoff. The backend re-streams
fresh state on every new connection.

### 2.3 CoT XML pull (for ATAK plugin)

```
GET /api/v1/cot/export                      → bulk, all escalated tracks
GET /api/v1/cot/export?emitter_id=<id>      → single track (operator-pulled)
```

Returns `application/xml` matching `docs/ATAK_COT_FORMAT.md`. Feed
straight into ATAK's local CoT pipeline.

### 2.4 Approvals (operator-in-the-loop from the phone)

```
GET    /api/v1/approvals                    → list pending
POST   /api/v1/approvals/{id}/approve       → release the CoT push
POST   /api/v1/approvals/{id}/reject        → suppress
```

### 2.5 Overrides

```
POST   /api/v1/overrides/blacklist          {"frequency_hz": 433920000, "tolerance_hz": 5000}
POST   /api/v1/overrides/friendly           {"emitter_id": "..."}
POST   /api/v1/overrides/manual_location    {"emitter_id": "...", "lat": ..., "lon": ...}
DELETE /api/v1/overrides/blacklist/<freq>
```

### 2.6 Mission control

```
POST   /api/v1/missions/start               {"name": "...", "operator": "..."}
POST   /api/v1/missions/end
GET    /api/v1/missions/active
GET    /api/v1/missions/<id>/aar.tar.gz     → download after-action ledger
```

### 2.7 Health / preflight

```
GET    /api/v1/health                       → per-node trust + GPS state
GET    /api/v1/preflight                    → JSON preflight (same as CLI)
GET    /healthz                             → liveness
GET    /readyz                              → readiness
```

## 3. Connecting the APK

Backend URL config in the APK (set at build OR via in-app settings):

```kotlin
const val PREDATOR_BACKEND_URL = "http://192.168.10.5:8000"  // RPi LAN IP
const val PREDATOR_BEARER_TOKEN = BuildConfig.PREDATOR_TOKEN  // from local.properties
```

## 4. Offline behavior

The Android client should:

* Cache the last successful `android-pull` snapshot in
  `SharedPreferences` so the UI shows stale-but-labelled data after
  a network drop.
* Queue operator actions (approve/reject, overrides) and replay them
  when the link comes back. **Idempotency keys aren't yet enforced**
  on the backend — see "Known issues" below.
* Display the `preflight_go` boolean as a banner. NO-GO + > 60 s of
  network silence = mission-abort prompt.

## 5. Schema versioning

* `schema=2` is the current `android-pull` payload version.
* Older clients (`schema=1`) are NOT supported by this backend
  (it would silently drop the `mission` and `approvals_pending`
  fields on a v1 client). Bump your client to ≥ schema 2.
* Future schema bumps will add fields, not remove them. Clients MUST
  ignore unknown fields.

## 6. Known issues / things to be aware of

These are real things the backend doesn't (yet) protect against. The
APK should defend itself accordingly:

1. **No idempotency on operator actions.** Approving the same
   approval twice is harmless (second call is a no-op), but rejecting
   then approving in rapid succession will race. Always wait for the
   200 before sending the next.
2. **Cursor is a server timestamp, not a sequence number.** If the
   backend's clock jumps backward (rare but possible on a fresh GPS
   discipline event), you may briefly see duplicate events. Dedup on
   `(node_id, timestamp_ns, frequency)` if it matters.
3. **`android-pull` returns max 2000 events per poll** even at
   `max_events=2000`. A backlog from a long offline period must be
   drained over several polls.
4. **CoT pull does NOT respect per-emitter rate limits** — UDP
   multicast does (5 s per emitter), HTTP pull does not. Don't poll
   `/cot/export` faster than every 30 s or you'll re-import the same
   markers in ATAK.
5. **No WebSocket support.** SSE only. Carrier proxies sometimes
   buffer SSE silently — fall back to `android-pull` if you don't see
   the SSE keepalive comment (`:keepalive\n\n`) every 15 s.
6. **TLS is out of scope of the backend.** If you expose the backend
   beyond a trusted LAN, terminate TLS at nginx / Caddy and pin the
   cert in the APK. Don't ship the bearer token over plaintext HTTP
   on a hostile network.
7. **No multi-operator coordination.** Two operators on the same
   backend can both approve/reject the same approval; last write
   wins. Coordinate out-of-band.
