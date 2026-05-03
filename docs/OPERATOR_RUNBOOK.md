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
curl $H -X POST localhost:8000/api/v1/missions/start \
  -d '{"name":"OVERWATCH-20260315","operator":"K9-Actual"}'

# … operate …

# Active mission
curl $H localhost:8000/api/v1/missions/active

# End
curl $H -X POST localhost:8000/api/v1/missions/end

# After-action ledger (events, tracks, assessments, approvals,
#  overrides — everything stamped to that mission_id)
curl $H -OJ localhost:8000/api/v1/missions/<id>/aar.tar.gz
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

## 9. When to hard-stop the mission

* Time sync silently lost (`/api/v1/preflight` shows `time` FAIL) → TDOA garbage
* >50% of nodes unreachable in `/api/v1/health` → no fusion
* Approval queue at `max_pending` for > 60 s → operator overload
* Disk free < 200 MB on `DATA_DIR` → mission ledger at risk
