#!/usr/bin/env bash
# ============================================================
# Snapshot the mission DB to a tar.gz on the target dir (USB stick,
# operator laptop, whatever). Uses sqlite3's `VACUUM INTO` so the
# snapshot is consistent even while the backend keeps writing.
#
#   ./backup_mission.sh                   # → /var/lib/predator-rf/backups/
#   ./backup_mission.sh /media/usb-stick  # → that dir
#
# Cron-friendly: silent on success, single-line ERROR on failure.
# ============================================================
set -euo pipefail

DATA_DIR="${DATA_DIR:-/var/lib/predator-rf}"
DB="${DATA_DIR}/${MISSION_DB:-mission.db}"
DEST="${1:-${DATA_DIR}/backups}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"

if [[ ! -f "${DB}" ]]; then
  echo "ERROR: mission DB not found at ${DB}" >&2
  exit 1
fi

mkdir -p "${DEST}"
SNAP="$(mktemp -d)/mission_${STAMP}.db"

# VACUUM INTO is online-safe; the backend's connection isn't blocked.
sqlite3 "${DB}" "VACUUM INTO '${SNAP}'"

OUT="${DEST}/predator-rf-mission-${STAMP}.tar.gz"
tar -C "$(dirname "${SNAP}")" -czf "${OUT}" "$(basename "${SNAP}")"
rm -f "${SNAP}"

# Keep last 14 archives unless the operator wants more.
KEEP="${BACKUP_KEEP:-14}"
ls -1t "${DEST}"/predator-rf-mission-*.tar.gz 2>/dev/null \
  | tail -n +"$((KEEP + 1))" | xargs -r rm -f

echo "${OUT}"
