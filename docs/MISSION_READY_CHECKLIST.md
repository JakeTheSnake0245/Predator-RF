# Predator-RF — Mission-Ready Checklist

Print this. Walk it before you go live. Anything not green is a no-go.

## Pre-departure (in shop / vehicle, with WAN)

- [ ] `sudo apt update && sudo apt upgrade -y` on operator workstation + each RPi
- [ ] Mission DB backed up — `deploy/backup_mission.sh` to USB
- [ ] System time disciplined (`chronyc tracking` shows `Leap status : Normal`)
- [ ] `/etc/predator-rf/predator-rf.env` reviewed; `FLEET_NODES` matches today's node serials
- [ ] `API_BEARER_TOKEN` rotated for this mission (`openssl rand -hex 32`)
- [ ] CoT/TAK destination + UID prefix set ONLY if you intend to push to TOC
- [ ] `COT_REQUIRE_MANUAL_APPROVAL=true` if `COT_ENABLED=true` (two-key gate)
- [ ] `AUTO_TASKER_ENABLED` matches your ROE — leave OFF unless you're authorized to re-tune nodes
- [ ] `AUTO_TASKER_GLOBAL_MAX_PER_MIN` sized for your fleet (default 30 = ~6 nodes worth of churn)

## On-site, before fleet power-on

- [ ] Each sensor node placed; antennas oriented; GPS sky-view confirmed
- [ ] Power budget sanity-checked (battery / vehicle alternator vs. node draw)
- [ ] Network reachable: `ping <node-ip>` succeeds from the operator workstation

## Bring-up sequence

1. Power on all sensor nodes; wait 60 s for GPS lock + Kujhad ready
2. `sudo systemctl start predator-rf`
3. `python deploy/preflight.py` → must report **GO**
4. `journalctl -u predator-rf -f` → no `ERROR` lines in the first 30 s
5. `curl -H "Authorization: Bearer $TOKEN" http://localhost:8000/api/v1/health` → `"status":"ok"`
6. `curl -H "Authorization: Bearer $TOKEN" http://localhost:8000/api/v1/missions/start -d '{"name":"<callsign-YYYYMMDD>"}'`

## In-mission checks (every 30 minutes)

- [ ] `curl /metrics` → `predator_events_total` is climbing
- [ ] `curl /api/v1/health` → all listed nodes show `gps_lock=true` AND `gps_age_s < 60`
- [ ] No CoT approvals stuck > 5 min in `/api/v1/approvals` (operator backlog)

## End-of-mission

1. `POST /api/v1/missions/end`
2. `GET /api/v1/missions/<id>/aar.tar.gz` → save to USB
3. `deploy/backup_mission.sh /media/usb-stick`
4. `sudo systemctl stop predator-rf`

## Failure modes — what they mean

| Symptom | Likely cause | First fix |
|---|---|---|
| Preflight `time` FAIL | NTP not syncing | `sudo systemctl restart chrony`; if no WAN, point to a local NTP server |
| Preflight `fleet` FAIL on one node | Cable / power / Kujhad crash | Walk to the node; check power LED; `ssh` and `systemctl status kujhad` |
| `/api/v1/health` shows `gps_age_s` climbing | Node lost GPS lock | Reposition antenna; node will be auto-dropped from TDOA |
| `auto_tasker_global_budget` counter rising | Assessment loop thrashing | Check `/api/v1/tracks` for duplicate tracks; raise `MIN_CONFIDENCE` |
| Approval queue full | Operator missed escalations | Drain via `/api/v1/approvals`, reject the false positives |
| SSE stream silent | Token wrong, or backend crashed | `journalctl -u predator-rf -n 50`; verify `API_BEARER_TOKEN` |
