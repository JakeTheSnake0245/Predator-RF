#!/usr/bin/env bash
#
# DF-Kracked LOB sensor — idempotent installer for a KrakenSDR Raspberry Pi.
#
# One command: sudo ./install.sh
#   * checks python3
#   * installs aiohttp + websockets (pip, --break-system-packages if needed)
#   * copies sensor.py + config to /opt/df_kracked_sensor
#   * installs, enables and (re)starts the systemd unit
#   * prints the peer code so the operator can pair the phone app
#
# Re-running is safe: it re-syncs files and restarts the service.

set -euo pipefail

INSTALL_DIR="/opt/df_kracked_sensor"
UNIT_NAME="df-kracked-sensor.service"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Service account: explicit $SERVICE_USER wins; otherwise the invoking user
# (sudo preserves it in $SUDO_USER); 'pi' only as a last resort. A hardcoded
# 'pi' default bit a KrakenSDR image whose user is 'krakenrf' — the unit
# shipped User=pi and systemd failed with status=217/USER.
if [ -z "${SERVICE_USER:-}" ]; then
    SERVICE_USER="${SUDO_USER:-$(id -un)}"
    if [ "${SERVICE_USER}" = "root" ]; then SERVICE_USER="pi"; fi
fi
if ! id "${SERVICE_USER}" >/dev/null 2>&1; then
    echo "ERROR: service user '${SERVICE_USER}' does not exist on this system." >&2
    echo "       Re-run as:  SERVICE_USER=<your-user> sudo -E ./install.sh" >&2
    exit 1
fi

echo "== DF-Kracked LOB sensor installer =="

# 1. python3 ---------------------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found. Install it first (apt install python3 python3-pip)." >&2
    exit 1
fi
echo "python3: $(python3 --version)"

# 2. pip deps --------------------------------------------------------------
PIP="python3 -m pip"
if ! $PIP --version >/dev/null 2>&1; then
    echo "pip not found; installing python3-pip via apt…"
    sudo apt-get update -qq && sudo apt-get install -y python3-pip
fi

echo "Installing Python deps (aiohttp, websockets)…"
# PEP 668 externally-managed environments (recent Raspberry Pi OS) need the
# override flag; try the clean install first, then fall back.
if ! $PIP install --upgrade aiohttp websockets >/dev/null 2>&1; then
    echo "Retrying with --break-system-packages (PEP 668 managed env)…"
    $PIP install --upgrade --break-system-packages aiohttp websockets
fi
echo "deps OK"

# 3. copy files ------------------------------------------------------------
echo "Installing to ${INSTALL_DIR}…"
sudo mkdir -p "${INSTALL_DIR}"
sudo cp "${SRC_DIR}/sensor.py" "${INSTALL_DIR}/sensor.py"
# Preserve an existing generated key/config; only seed if missing.
if [ -f "${SRC_DIR}/df_kracked_sensor.json" ] && \
   [ ! -f "${INSTALL_DIR}/df_kracked_sensor.json" ]; then
    sudo cp "${SRC_DIR}/df_kracked_sensor.json" "${INSTALL_DIR}/df_kracked_sensor.json"
fi
# Make sure the service user owns the dir so it can persist the API key.
if id "${SERVICE_USER}" >/dev/null 2>&1; then
    sudo chown -R "${SERVICE_USER}:${SERVICE_USER}" "${INSTALL_DIR}"
else
    echo "WARN: user '${SERVICE_USER}' not found; leaving ${INSTALL_DIR} owned by root." >&2
    echo "      Set SERVICE_USER=<youruser> and edit ${UNIT_NAME} User=." >&2
fi

# Lock down permissions: the config file holds the shared API key.
# 750 on the dir keeps it off other users; 600 on the key file keeps the
# secret readable only by the service user.
sudo chmod 750 "${INSTALL_DIR}"
if [ -f "${INSTALL_DIR}/df_kracked_sensor.json" ]; then
    sudo chmod 600 "${INSTALL_DIR}/df_kracked_sensor.json"
fi

# 4. systemd unit ----------------------------------------------------------
echo "Installing systemd unit…"
# Rewrite User= to the chosen service user for convenience.
sudo sed "s/^User=.*/User=${SERVICE_USER}/; s/^Group=.*/Group=${SERVICE_USER}/" \
    "${SRC_DIR}/${UNIT_NAME}" | sudo tee "/etc/systemd/system/${UNIT_NAME}" >/dev/null

sudo systemctl daemon-reload
sudo systemctl enable "${UNIT_NAME}"
sudo systemctl restart "${UNIT_NAME}"

echo "Service started. Recent log:"
sleep 1
sudo systemctl --no-pager --lines=20 status "${UNIT_NAME}" || true

# 5. peer code -------------------------------------------------------------
echo ""
echo "Fetching peer code from the service log…"
sleep 1
# The pairing block is printed to stdout by sensor.py on startup.
sudo journalctl -u "${UNIT_NAME}" --no-pager --lines=40 | grep -A6 "PAIRING" || {
    echo "(Peer code not in journal yet — run: journalctl -u ${UNIT_NAME} | grep 'PEER CODE')"
}

echo ""
echo "Done. To pair: open Predator RF → Kujhad → Add Peer → enter the"
echo "overlay IP:port and API key shown above (header X-Kujhad-Key)."
