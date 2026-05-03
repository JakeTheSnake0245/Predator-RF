#!/usr/bin/env bash
# ============================================================
# Predator-RF — one-shot installer for Raspberry Pi (Debian/RPi OS).
# Run as root; idempotent; safe to re-run after upstream changes.
#
#   curl -sSf https://raw.githubusercontent.com/JakeTheSnake0245/Predator-RF/main/deploy/install_rpi.sh | sudo bash
#
# What it does:
#   1. Creates the `predator` system user (no shell, no home).
#   2. Lays out /opt/predator-rf, /etc/predator-rf, /var/lib/predator-rf,
#      /var/log/predator-rf with the right ownership.
#   3. Installs python3-venv + chrony + sqlite3 (apt; no extras).
#   4. Clones / pulls the repo into /opt/predator-rf.
#   5. Creates a Python venv and installs the backend in editable mode.
#   6. Drops predator-rf.env into /etc/predator-rf if missing.
#   7. Installs + enables the systemd unit.
#   8. Runs preflight; reports GO / NO-GO.
#
# What it does NOT do:
#   * It will not start the service if preflight reports NO-GO.
#   * It will not overwrite an existing /etc/predator-rf/predator-rf.env.
# ============================================================
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/JakeTheSnake0245/Predator-RF.git}"
INSTALL_DIR="/opt/predator-rf"
ETC_DIR="/etc/predator-rf"
DATA_DIR="/var/lib/predator-rf"
LOG_DIR="/var/log/predator-rf"
SVC_USER="predator"

if [[ $EUID -ne 0 ]]; then
  echo "must run as root (sudo)" >&2
  exit 1
fi

echo "[1/8] creating ${SVC_USER} system user"
id -u "${SVC_USER}" &>/dev/null || \
  useradd --system --no-create-home --shell /usr/sbin/nologin "${SVC_USER}"

echo "[2/8] creating directories"
install -d -o "${SVC_USER}" -g "${SVC_USER}" -m 0755 \
  "${INSTALL_DIR}" "${DATA_DIR}" "${LOG_DIR}"
install -d -o root -g "${SVC_USER}" -m 0750 "${ETC_DIR}"

echo "[3/8] apt deps"
apt-get update -qq
apt-get install -y --no-install-recommends \
  python3 python3-venv python3-pip git chrony sqlite3 ca-certificates

echo "[4/8] fetching source"
if [[ -d "${INSTALL_DIR}/.git" ]]; then
  sudo -u "${SVC_USER}" git -C "${INSTALL_DIR}" pull --ff-only
else
  sudo -u "${SVC_USER}" git clone --depth=20 "${REPO_URL}" "${INSTALL_DIR}"
fi

echo "[5/8] python venv + deps"
sudo -u "${SVC_USER}" python3 -m venv "${INSTALL_DIR}/.venv"
# Backend is intentionally pure-stdlib at the core; only install
# extras if requirements.txt exists. A minimal field deploy can skip
# aiohttp + numpy entirely.
if [[ -f "${INSTALL_DIR}/requirements.txt" ]]; then
  sudo -u "${SVC_USER}" "${INSTALL_DIR}/.venv/bin/pip" install -q -r \
    "${INSTALL_DIR}/requirements.txt"
fi

echo "[6/8] env file"
if [[ ! -f "${ETC_DIR}/predator-rf.env" ]]; then
  install -o root -g "${SVC_USER}" -m 0640 \
    "${INSTALL_DIR}/deploy/predator-rf.env.example" \
    "${ETC_DIR}/predator-rf.env"
  echo "  → wrote ${ETC_DIR}/predator-rf.env (EDIT BEFORE STARTING)"
else
  echo "  → ${ETC_DIR}/predator-rf.env already exists, leaving alone"
fi

echo "[7/8] systemd unit"
install -m 0644 "${INSTALL_DIR}/deploy/predator-rf.service" \
  /etc/systemd/system/predator-rf.service
systemctl daemon-reload
systemctl enable predator-rf.service

echo "[8/8] preflight"
set +e
sudo -u "${SVC_USER}" \
  env $(grep -v '^#' "${ETC_DIR}/predator-rf.env" | xargs) \
  "${INSTALL_DIR}/.venv/bin/python" "${INSTALL_DIR}/deploy/preflight.py"
PF=$?
set -e

if [[ $PF -ne 0 ]]; then
  echo
  echo "preflight reported NO-GO. Fix the failures above, edit"
  echo "${ETC_DIR}/predator-rf.env, then:"
  echo
  echo "    sudo -u ${SVC_USER} ${INSTALL_DIR}/.venv/bin/python \\"
  echo "        ${INSTALL_DIR}/deploy/preflight.py"
  echo "    sudo systemctl start predator-rf"
  exit $PF
fi

echo
echo "preflight: GO. Start with:"
echo "    sudo systemctl start predator-rf"
echo "Tail logs:"
echo "    journalctl -u predator-rf -f"
