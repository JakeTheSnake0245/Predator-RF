#!/usr/bin/env bash
# ============================================================
# Predator-RF — one-shot installer for Raspberry Pi (Debian/RPi OS).
# Run as root; idempotent; safe to re-run after upstream changes.
#
#   curl -sSf https://raw.githubusercontent.com/JakeTheSnake0245/Predator-RF/main/deploy/install_rpi.sh | sudo bash
#
# What it does:
#   1. Creates the `predator` system user (no shell, no home).
#   1b. Installs the full SDR userspace (rtl-sdr, hackrf, airspy, soapy,
#       gpsd) + KrakenSDR DoA stack by default — hardware-agnostic baseline
#       sensor package; pass --no-kraken to skip the DoA stack.
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
# Baseline sensor package doctrine: install EVERYTHING (all SDR userspace
# tools, gpsd, KrakenSDR DoA stack) so a node works with whatever hardware
# gets plugged in later. Pass --no-kraken to skip the heavy DoA stack on
# nodes that will definitely never host a Kraken array.
KRAKEN=1

for arg in "$@"; do
  case "$arg" in
    --kraken)    KRAKEN=1 ;;
    --no-kraken) KRAKEN=0 ;;
  esac
done

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

echo "[3/8] SDR userspace (baseline sensor package — hardware-agnostic)"
# Install host tools + udev rules for every SDR family we support, so the
# node accepts whatever gets plugged in. Each package is best-effort: names
# vary slightly across Debian/RPi OS/Ubuntu releases and a miss must not
# abort the install.
for pkg in rtl-sdr librtlsdr0 hackrf libhackrf0 airspy libairspy0 \
           soapysdr-tools usbutils gpsd gpsd-clients; do
  apt-get install -y --no-install-recommends "$pkg" 2>/dev/null || \
    echo "  → $pkg unavailable on this distro, skipping"
done
# DVB kernel drivers grab RTL dongles before SDR tools can; blacklist them.
if [[ ! -f /etc/modprobe.d/blacklist-rtlsdr.conf ]]; then
  cat > /etc/modprobe.d/blacklist-rtlsdr.conf <<'BLK'
blacklist dvb_usb_rtl28xxu
blacklist rtl2832
blacklist rtl2830
BLK
  echo "  → blacklisted DVB kernel drivers"
fi
# Apply the blacklist NOW — no reboot before operators plug in dongles.
for mod in dvb_usb_rtl28xxu rtl2832 rtl2830; do
  if lsmod | grep -q "^${mod}"; then
    modprobe -r "$mod" 2>/dev/null || rmmod "$mod" 2>/dev/null || \
      echo "  → WARNING: $mod loaded and busy — unplug the dongle and re-run, or reboot"
  fi
done
# Bake the blacklist into the initramfs so early boot never loads them.
command -v update-initramfs >/dev/null 2>&1 && update-initramfs -u >/dev/null 2>&1 || true
echo "  → DVB drivers unloaded + blacklisted (effective immediately)"
usermod -aG plugdev "${SVC_USER}" 2>/dev/null || true
usermod -aG dialout "${SVC_USER}" 2>/dev/null || true   # GPS pucks on /dev/ttyUSB*/ttyACM*
# gpsd stays installed-but-disabled until a puck is present; enable with
# `systemctl enable --now gpsd` once hardware is attached.
systemctl disable --now gpsd 2>/dev/null || true

# KrakenSDR optional: numpy/scipy for N-LOB weighted least-squares.
# The Python backend always installs them if requirements.txt lists them;
# this block adds them explicitly when --kraken is passed as a safety net
# for minimal deployments that skip requirements.txt.
if [[ "$KRAKEN" -eq 1 ]]; then
  echo "[3/8+] KrakenSDR opt-in: ensuring numpy + scipy in venv"
  # venv may not exist yet; deferred pip install happens after step 5.
  echo "  → will install numpy scipy after venv creation (step 5)"
fi

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

if [[ "$KRAKEN" -eq 1 ]]; then
  echo "[5+/8] KrakenSDR: installing numpy + scipy"
  sudo -u "${SVC_USER}" "${INSTALL_DIR}/.venv/bin/pip" install -q numpy scipy

  echo "[5+/8] KrakenSDR: installing krakensdr_doa dependencies"
  apt-get install -y --no-install-recommends \
    python3-dev libatlas-base-dev usbutils \
    rtl-sdr 2>/dev/null || true

  echo "[5+/8] KrakenSDR: writing udev rule for KrakenSDR USB interface"
  cat > /etc/udev/rules.d/99-krakensdr.rules <<'UDEV'
# KrakenSDR — 5-element coherent SDR (RTL2832U-based)
# Grant the 'predator' service user RW access to the USB device.
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2832", \
    MODE="0664", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2838", \
    MODE="0664", GROUP="plugdev", TAG+="uaccess"
UDEV
  udevadm control --reload-rules 2>/dev/null || true
  usermod -aG plugdev "${SVC_USER}" 2>/dev/null || true

  # ── krakensdr_doa: clone + pip-install + systemd unit ─────────────────
  KRAKEN_DOA_DIR="/opt/krakensdr_doa"
  KRAKEN_DOA_REPO="https://github.com/krakenrf/krakensdr_doa.git"

  if [[ -d "${KRAKEN_DOA_DIR}/.git" ]]; then
    echo "  → krakensdr_doa already cloned — pulling latest"
    git -C "${KRAKEN_DOA_DIR}" pull --ff-only 2>/dev/null || true
  else
    echo "  → cloning krakensdr_doa into ${KRAKEN_DOA_DIR}"
    apt-get install -y --no-install-recommends git 2>/dev/null || true
    git clone --depth 1 "${KRAKEN_DOA_REPO}" "${KRAKEN_DOA_DIR}"
  fi

  if [[ -f "${KRAKEN_DOA_DIR}/requirements.txt" ]]; then
    echo "  → installing krakensdr_doa Python requirements"
    pip3 install -q --break-system-packages \
      -r "${KRAKEN_DOA_DIR}/requirements.txt" 2>/dev/null || \
    pip3 install -q -r "${KRAKEN_DOA_DIR}/requirements.txt"
  fi

  # ── /etc/krakensdr/predator.env ───────────────────────────────────────
  mkdir -p /etc/krakensdr
  if [[ ! -f /etc/krakensdr/predator.env ]]; then
    cat > /etc/krakensdr/predator.env <<'KENV'
# KrakenSDR → Predator RF integration environment.
# Adjust KRAKEN_DOA_HOST/PORT to match your krakensdr_doa instance.
PREDATOR_LOB_WS_PORT=8082
PREDATOR_LOB_WS_PATH=/ws
KRAKEN_DOA_HOST=127.0.0.1
KRAKEN_DOA_DOA_PORT=8081
KENV
    echo "  → wrote /etc/krakensdr/predator.env (review and adjust)"
  fi

  # ── krakensdr-doa.service systemd unit ────────────────────────────────
  # Runs krakensdr_doa's web server so Predator RF can connect to it via
  # WebSocket.  The unit uses the environment file above so operators only
  # need to edit one file.  Enabled but not started — the operator must
  # confirm hardware is connected before first start.
  if [[ ! -f /etc/systemd/system/krakensdr-doa.service ]]; then
    cat > /etc/systemd/system/krakensdr-doa.service <<'SVCEOF'
[Unit]
Description=KrakenSDR DOA engine
Documentation=https://github.com/krakenrf/krakensdr_doa
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/krakensdr_doa
EnvironmentFile=-/etc/krakensdr/predator.env
ExecStart=/usr/bin/python3 /opt/krakensdr_doa/_UI/kraken_web_interface.py
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
SVCEOF
    echo "  → wrote krakensdr-doa.service (start with: systemctl start krakensdr-doa)"
  fi
  systemctl daemon-reload 2>/dev/null || true
  systemctl enable krakensdr-doa.service 2>/dev/null || true

  echo "[5+/8] KrakenSDR setup complete"
  echo "  → krakensdr_doa at ${KRAKEN_DOA_DIR}"
  echo "  → edit /etc/krakensdr/predator.env then: systemctl start krakensdr-doa"
fi

echo "[6/8] env file"
if [[ ! -f "${ETC_DIR}/predator-rf.env" ]]; then
  install -o root -g "${SVC_USER}" -m 0640 \
    "${INSTALL_DIR}/deploy/predator-rf.env.example" \
    "${ETC_DIR}/predator-rf.env"
  echo "  → wrote ${ETC_DIR}/predator-rf.env"
else
  echo "  → ${ETC_DIR}/predator-rf.env already exists, leaving alone"
fi
# Auto-generate the bearer token if unset — nodes must never go live
# with an empty token.
if grep -q '^API_BEARER_TOKEN=$' "${ETC_DIR}/predator-rf.env"; then
  TOKEN="$( (openssl rand -hex 32 2>/dev/null) || head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n')"
  sed -i "s|^API_BEARER_TOKEN=$|API_BEARER_TOKEN=${TOKEN}|" "${ETC_DIR}/predator-rf.env"
  echo "  → auto-generated API_BEARER_TOKEN (view: sudo grep TOKEN ${ETC_DIR}/predator-rf.env)"
fi

echo "[6a/8] tailscale client"
if ! command -v tailscale >/dev/null 2>&1; then
  echo "  → installing tailscale (official installer)"
  curl -fsSL https://tailscale.com/install.sh | sh || \
    echo "  → WARNING: tailscale install failed — install manually before going live"
fi
if command -v tailscale >/dev/null 2>&1; then
  systemctl enable --now tailscaled 2>/dev/null || true
  if tailscale status >/dev/null 2>&1; then
    echo "  → tailscale already up ($(tailscale ip -4 2>/dev/null | head -1))"
  elif [[ -n "${TS_AUTHKEY:-}" ]]; then
    # Zero-touch join: TS_AUTHKEY=tskey-auth-... sudo -E bash install_rpi.sh
    tailscale up --authkey "${TS_AUTHKEY}" --ssh 2>/dev/null || \
      tailscale up --authkey "${TS_AUTHKEY}" || \
      echo "  → WARNING: tailscale up failed with provided TS_AUTHKEY"
    echo "  → joined tailnet ($(tailscale ip -4 2>/dev/null | head -1))"
  else
    echo "  → NOT on the tailnet yet. Join with:  sudo tailscale up"
    echo "    (or re-run installer with TS_AUTHKEY=tskey-auth-… for zero-touch join)"
  fi
fi

echo "[6b/8] firewall — API reachable from tailnet + loopback ONLY"
API_PORT="$(grep -E '^API_PORT=' "${ETC_DIR}/predator-rf.env" | cut -d= -f2)"
API_PORT="${API_PORT:-8000}"
if ! command -v ufw >/dev/null 2>&1; then
  apt-get install -y --no-install-recommends ufw 2>/dev/null || true
fi
if command -v ufw >/dev/null 2>&1; then
  ufw allow OpenSSH >/dev/null 2>&1 || ufw allow 22/tcp >/dev/null 2>&1 || true
  # Tailscale CGNAT range; loopback is implicitly allowed by ufw.
  ufw allow from 100.64.0.0/10 to any port "${API_PORT}" proto tcp >/dev/null 2>&1 || true
  ufw deny "${API_PORT}/tcp" >/dev/null 2>&1 || true
  ufw --force enable >/dev/null 2>&1 || true
  echo "  → ufw: SSH open, port ${API_PORT} restricted to 100.64.0.0/10 (tailnet)"
else
  echo "  → WARNING: ufw unavailable — port ${API_PORT} is NOT firewalled to the tailnet"
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
echo "preflight: GO — starting predator-rf now"
systemctl restart predator-rf
sleep 2
if systemctl is-active --quiet predator-rf; then
  echo "  → predator-rf ACTIVE and enabled at boot"
else
  echo "  → predator-rf failed to start — check: journalctl -u predator-rf -n 50"
fi
echo "Tail logs:"
echo "    journalctl -u predator-rf -f"

# ── Hardware detection report (informational only) ─────────────────────
echo
echo "── attached SDR / GPS hardware ──"
if command -v lsusb >/dev/null 2>&1; then
  FOUND=0
  while read -r vid name; do
    if lsusb | grep -qi "$vid"; then echo "  ✔ $name"; FOUND=1; fi
  done <<'HW'
0bda:2838 RTL-SDR (RTL2832U)
0bda:2832 RTL-SDR (RTL2832U raw)
1d50:6089 HackRF One
1d50:60a1 Airspy
2cf0:5250 LimeSDR
HW
  [[ $FOUND -eq 0 ]] && echo "  (none detected — plug in an SDR any time; drivers are ready)"
else
  echo "  lsusb unavailable — skipping detection"
fi
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | sed 's/^/  serial (possible GPS): /' || true
