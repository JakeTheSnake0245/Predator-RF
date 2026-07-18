#!/usr/bin/env bash
# ============================================================
# Standby-kit snapshot puller — coordinator failure recovery.
#
# Run this on the BACKUP coordinator kit, on a cron, while the
# primary coordinator is healthy. It pulls a consistent SQLite
# snapshot of the primary's mission DB over the existing HTTP API
# so a promotion starts from recent mission state, not empty.
#
#   COORDINATOR_URL=http://192.168.10.2:8000 \
#   API_BEARER_TOKEN=... \
#   ./fetch_snapshot.sh [dest-dir]
#
# Defaults: dest-dir = ${DATA_DIR:-/var/lib/predator-rf}/standby
# Keeps the last ${SNAPSHOT_KEEP:-8} snapshots plus a stable
# `latest.db` symlink the promotion procedure copies from.
#
# Cron example (every 15 min):
#   */15 * * * * predator COORDINATOR_URL=http://192.168.10.2:8000 \
#     /opt/predator-rf/deploy/fetch_snapshot.sh >/dev/null 2>&1
#
# Cron-friendly: silent on success, single-line ERROR on failure.
# ============================================================
set -euo pipefail

COORDINATOR_URL="${COORDINATOR_URL:?ERROR: set COORDINATOR_URL, e.g. http://192.168.10.2:8000}"
DATA_DIR="${DATA_DIR:-/var/lib/predator-rf}"
DEST="${1:-${DATA_DIR}/standby}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
KEEP="${SNAPSHOT_KEEP:-8}"

AUTH=()
if [[ -n "${API_BEARER_TOKEN:-}" ]]; then
  AUTH=(-H "Authorization: Bearer ${API_BEARER_TOKEN}")
fi

mkdir -p "${DEST}"
TMP="$(mktemp "${DEST}/.snap.XXXXXX")"
trap 'rm -f "${TMP}"' EXIT

if ! curl -fsS --max-time "${SNAPSHOT_TIMEOUT_S:-120}" "${AUTH[@]}" \
     -o "${TMP}" "${COORDINATOR_URL%/}/api/v1/snapshot"; then
  echo "ERROR: snapshot fetch from ${COORDINATOR_URL} failed" >&2
  exit 1
fi

# Sanity check: must be a SQLite database, not an HTML error page.
if ! head -c 16 "${TMP}" | grep -q "SQLite format 3"; then
  echo "ERROR: fetched file is not a SQLite DB (auth failure or wrong URL?)" >&2
  exit 1
fi

OUT="${DEST}/mission-snapshot-${STAMP}.db"
mv "${TMP}" "${OUT}"
trap - EXIT
ln -sfn "$(basename "${OUT}")" "${DEST}/latest.db"

# Rotate: keep last ${KEEP} snapshots.
ls -1t "${DEST}"/mission-snapshot-*.db 2>/dev/null \
  | tail -n +"$((KEEP + 1))" | xargs -r rm -f

echo "${OUT}"
