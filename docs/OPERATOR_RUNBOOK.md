# Predator-RF Operator Runbook (Path 1 — Python backend)

Intended audience: the SIGINT operator running this backend on a
Raspberry Pi or laptop, with one or more Kujhad-equipped sensor
nodes on the LAN. Path 2 (C++ Android client) is built separately
on Windows and consumes this backend's REST/SSE feed.

## 0. Where things live

| Path | What |
|---|---|
| `/opt/predator-rf` | Source checkout + Python venv |
| `/etc/predator-rf/predator-rf.env` | All env-var config (see `deploy/predator-rf.env.example`) |
| `/var/lib/predator-rf/mission.db` | SQLite mission ledger |
| `/var/lib/predator-rf/backups/` | Snapshots from `deploy/backup_mission.sh` |
| systemd unit | `/etc/systemd/system/predator-rf.service` |
| Logs | `journalctl -u predator-rf` |

## 1. Start / stop / status

```bash
sudo systemctl start   predator-rf     # bring up
sudo systemctl stop    predator-rf     # SIGTERM; drains in <5s
sudo systemctl restart predator-rf     # apply env changes
sudo systemctl status  predator-rf
journalctl -u predator-rf -f           # tail
```

Backend listens on `:8000` by default. Bind via reverse proxy if
you need TLS (the backend itself is plain HTTP — TLS termination is
intentionally out of scope so the same binary works behind nginx,
Caddy, Traefik, or nothing).

### CoC Web Dashboard

Navigate to `http://<pi-ip>:8000/dashboard` in any browser on the
LAN to open the Chain-of-Command operator dashboard. No login
required on a trusted LAN (add `API_BEARER_TOKEN` in the env file
to gate it behind a bearer token).

The dashboard is served directly from the Python backend — no
separate Node.js or web server process is needed. It auto-opens a
live SSE connection to `/api/v1/events/stream` for real-time track
updates and polls the fleet, approvals, and RNS status endpoints.

| Panel | Data source | Refresh |
|---|---|---|
| Fleet Nodes | `GET /api/v1/nodes/` | Every 10 s |
| Emitter Tracks | `GET /api/v1/events/stream` (SSE) + `/api/v1/tracks/` | Real-time |
| Approvals | `GET /api/v1/approvals` | Every 5 s |
| Map | `root/res/maps/index.html` iframe + postMessage | Real-time |
| RNS Status | `GET /api/v1/rns/status` | Every 8 s |

**Quick one-command Pi startup (development / ad-hoc):**

```bash
cd /opt/predator-rf
source .venv/bin/activate
FLEET_NODES="..." python -m backend.main
# Dashboard: http://<pi-ip>:8000/dashboard
```

## 2. The two-key TX gate (CoT and AutoTasker)

Predator-RF starts in **RX-only** posture. Two flags arm the only
TX surfaces:

* `COT_ENABLED=true` lets the backend send CoT beacons to TAK.
* `AUTO_TASKER_ENABLED=true` lets the backend re-tune SDRs over Kujhad.

When `COT_ENABLED=true`, **always** set `COT_REQUIRE_MANUAL_APPROVAL=true`
in the field. That makes every escalation queue at
`GET /api/v1/approvals` and wait for the operator to POST
`/api/v1/approvals/{id}/approve` (or `/reject`). This is the
two-person rule equivalent — operator IS the second person.

When `AUTO_TASKER_ENABLED=true`, the global brake
(`AUTO_TASKER_GLOBAL_MAX_PER_MIN`, default 30) caps total fleet
re-tunes per minute so a runaway assessment loop can't thrash every
node.

## 3. Mission lifecycle (operator side)

```bash
TOKEN=$(grep API_BEARER_TOKEN /etc/predator-rf/predator-rf.env | cut -d= -f2)
H="-H Authorization:Bearer\ $TOKEN"

# Start
curl $H -X POST localhost:8000/api/v1/missions \
  -d '{"name":"OVERWATCH-20260315","operator":"K9-Actual"}'

# … operate …

# Active mission
curl $H localhost:8000/api/v1/missions/active

# End
curl $H -X POST localhost:8000/api/v1/missions/end

# After-action ledger (events, tracks, assessments, approvals,
#  overrides — everything stamped to that mission_id)
curl $H -OJ localhost:8000/api/v1/missions/<id>/export
```

## 4. Operator overrides

These are the runtime knobs you'll actually touch in the field.

| Endpoint | When to use |
|---|---|
| `POST /api/v1/overrides/blacklist` | Drop events on a freq you know is your own gear / a known nuisance |
| `POST /api/v1/overrides/friendly` | Mark an emitter ID as friendly — never escalates to TAK |
| `POST /api/v1/overrides/manual_location` | You have better DF than TDOA — replace estimate |
| `DELETE /api/v1/overrides/...` | Each is reversible |

All overrides survive a restart and are stamped to the active mission.

## 5. Health & observability

| Endpoint | Purpose |
|---|---|
| `GET /healthz` | Liveness — always 200 if the process is up |
| `GET /readyz` | Readiness — 200 only when fleet poll has succeeded once |
| `GET /metrics` | Prometheus text format (events, tracks, approvals, AutoTasker) |
| `GET /api/v1/preflight` | Same checks as `deploy/preflight.py`, JSON |
| `GET /api/v1/health` | Per-node trust, GPS lock, GPS age, timing source |
| `GET /api/v1/events/stream` | SSE — all RF events as they happen |

The SSE stream accepts `?token=` so a browser EventSource can use
the bearer token. Every other route requires the `Authorization`
header — the query fallback is intentionally narrowed to SSE.

## 6. Scheduled maintenance

Add to `/etc/cron.d/predator-rf`:

```
# Hourly DB snapshot to USB (path optional)
17 * * * *  predator  /opt/predator-rf/deploy/backup_mission.sh /media/usb >/dev/null 2>&1

# Daily preflight - GO/NO-GO logged to journald
03 04 * * * predator  /opt/predator-rf/.venv/bin/python /opt/predator-rf/deploy/preflight.py | systemd-cat -t predator-preflight
```

## 7. Incident response — quick reference

| Symptom | Diagnosis | Action |
|---|---|---|
| Backend won't start | Check journald: `journalctl -u predator-rf -n 50` | Most common: bad `FLEET_NODES` parse, missing `DATA_DIR` perms |
| All nodes show low confidence | Time sync drift | `chronyc tracking`; restart chrony; confirm GPS-disciplined source if no WAN |
| One node drops out of TDOA | `gps_age_s` exceeded `GPS_MAX_AGE_S` | Reposition antenna; node still feeds events, just no location contribution |
| Stuck approvals piling up | Operator was busy | Bulk reject false positives via `POST /api/v1/approvals/{id}/reject` |
| Suspect false TDOA fixes | Node geometry collapsed (≤ 2 nodes hearing) | Check `error_ellipse` on the track; widen baselines |
| Kujhad node hangs | Node-side issue | Backend keeps polling — inspect via `ssh` and `journalctl -u kujhad` on the node |

## 8. CoC / TOC aggregation (multi-station)

Only enable on the TOC workstation. Set
`COC_MODE_ENABLED=true` and `COC_UPSTREAM_URLS` to the field
stations' base URLs. The TOC backend then treats each upstream's
SSE feed exactly like a local node — same fusion, same baselines,
same TDOA. Cross-station dedup coalesces tracks for the same
physical emitter heard by multiple stations.

## 9. Coordinator down — failure recovery

The coordinator (this backend's RPi/laptop) is a single point of
failure for **fusion**: while it is down, no new TDOA fixes, track
updates, assessments, or CoT escalations happen. The sensor nodes
themselves keep running — this section is about making coordinator
loss a bounded, rehearsed event.

### What survives, what is lost

| Survives coordinator loss | Lost |
|---|---|
| Node-side Kujhad event rings (each node keeps buffering hits) | Fusion output during the outage (no fixes/assessments produced) |
| SQLite mission DB on the coordinator disk (WAL — survives SIGKILL) | In-flight approval queue entries (queue is ephemeral by design) |
| Operator overrides, missions, approvals *ledger* (all in the DB) | Events older than the node rings' retention if outage is long |
| Standby snapshots pulled by the backup kit (see below) | Anything written after the last standby snapshot, if the disk itself dies |

**Data-loss bound:** same-kit restart with an intact disk loses
nothing durable. Backup promotion loses at most one snapshot
interval of coordinator-side state (default cron: 15 min) — but
events still buffered in node rings are re-fetched via `since=`
catch-up, so the practical loss is usually just assessments/fix
history from the gap, not the raw events.

### 9a. Same-kit restart (RPi rebooted / process died, disk OK)

1. `sudo systemctl start predator-rf` (or reboot the Pi; the unit
   is `WantedBy=multi-user.target`).
2. Watch `journalctl -u predator-rf -f` — you should see
   `MissionStore schema now at v2` and track rehydration
   (`load_active_tracks`) log lines.
3. Verify recovery:
   ```bash
   curl -H "Authorization: Bearer $TOKEN" localhost:8000/readyz    # 200 after first fleet poll
   curl -H "Authorization: Bearer $TOKEN" localhost:8000/api/v1/tracks/   # pre-outage tracks present
   ```
4. The backend re-polls every `FLEET_NODES` entry and catches up
   each node's event ring from its last-seen cursor — no operator
   action needed. Tracks are keyed by `emitter_id`, so replayed
   events update existing tracks instead of duplicating them.

### 9b. Promoting the pre-designated backup kit

Prerequisites (verify BEFORE the mission — see §9c):
the backup kit has the same source checkout + venv, a copy of
`/etc/predator-rf/predator-rf.env` with current `FLEET_NODES` and
`API_BEARER_TOKEN`, and a snapshot cron pulling
`GET /api/v1/snapshot` via `deploy/fetch_snapshot.sh`.

1. **Confirm the primary is really dead** (not just a network
   blip) — two coordinators polling the same fleet is wasteful but
   safe (RX-only); two coordinators with `AUTO_TASKER_ENABLED`
   is NOT. If in doubt, physically power off the primary.
2. On the backup kit, place the newest snapshot as the live DB:
   ```bash
   sudo systemctl stop predator-rf   # if it was idling
   cp /var/lib/predator-rf/standby/latest.db /var/lib/predator-rf/mission.db
   rm -f /var/lib/predator-rf/mission.db-wal /var/lib/predator-rf/mission.db-shm
   ```
3. Preflight: `PREFLIGHT_STANDBY=1 python deploy/preflight.py`
   → must report **GO** (the `backup` check verifies FLEET_NODES
   and snapshot freshness).
4. `sudo systemctl start predator-rf`.
5. Verify: same checks as §9a step 3. Rehydrated tracks come from
   the snapshot; node re-polling catches up event rings via the
   `since=` cursors. Because tracks upsert on `emitter_id` and
   events are `INSERT OR IGNORE` on `event_id`, replaying events
   the snapshot already contained does **not** duplicate anything.
6. If the fleet nodes pin the coordinator's address (static IP /
   Kujhad registration), either move the primary's IP to the backup
   kit or update the nodes — nodes configured by hostname need no
   change if DNS/mDNS points at the backup.
7. Start a new mission (or continue the active one — the active
   mission row travels with the snapshot).

**Do not** re-arm `COT_ENABLED` / `AUTO_TASKER_ENABLED` on the
backup until you have confirmed the primary is off — the two-key
posture starts safe because the env file ships with both off
unless you copied armed values.

### 9c. Keeping the backup ready (during the mission)

On the **backup kit**, cron the snapshot pull while the primary is
healthy:

```
*/15 * * * * predator COORDINATOR_URL=http://<primary-ip>:8000 \
  API_BEARER_TOKEN=<token> /opt/predator-rf/deploy/fetch_snapshot.sh >/dev/null 2>&1
```

Snapshots land in `/var/lib/predator-rf/standby/` with a
`latest.db` symlink; the last 8 are kept. Tune with
`SNAPSHOT_KEEP` and check freshness anytime with
`PREFLIGHT_STANDBY=1 python deploy/preflight.py` (FAILs if the
newest snapshot is older than `STANDBY_SNAPSHOT_MAX_AGE_S`,
default 3600 s, or if `FLEET_NODES` is unset).

## 10. When to hard-stop the mission

* Time sync silently lost (`/api/v1/preflight` shows `time` FAIL) → TDOA garbage
* >50% of nodes unreachable in `/api/v1/health` → no fusion
* Approval queue at `max_pending` for > 60 s → operator overload
* Disk free < 200 MB on `DATA_DIR` → mission ledger at risk
