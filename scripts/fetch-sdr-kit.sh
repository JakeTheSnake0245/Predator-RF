#!/usr/bin/env bash
# Rebuild android/sdr-kit/arm64-v8a from upstream sources.
#
# Strategy: download the upstream SDR++ Android nightly APK, extract its
# arm64-v8a dependency .so files, then clone each library at its pinned
# version and copy the public headers. This sidesteps the multi-hour
# Docker cross-compile of the official android-sdr-kit recipe.
#
# Requirements (auto-checked below): bash, curl, git, python3, cmake,
# python3-mako (for volk header generation only).
#
# Usage:
#   bash scripts/fetch-sdr-kit.sh            # standard SDR kit only
#   bash scripts/fetch-sdr-kit.sh --kraken   # also copy krakensdr_doa headers

set -euo pipefail

KRAKEN=0
for arg in "$@"; do
  case "$arg" in --kraken) KRAKEN=1 ;; esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KIT="$REPO_ROOT/android/sdr-kit/arm64-v8a"
WORK="$(mktemp -d -t sdrkit.XXXXXX)"
APK_URL="https://github.com/AlexandreRouma/SDRPlusPlus/releases/download/nightly/sdrpp.apk"

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "ERROR: missing required tool: $1"; exit 1; }
}
need curl; need git; need python3; need cmake

echo "==> Workspace: $WORK"
echo "==> Output:    $KIT"
mkdir -p "$KIT/lib" "$KIT/include"
cd "$WORK"

############################################################
# 1) Extract dependency .so files from the upstream nightly APK
############################################################
echo
echo "==> Downloading upstream sdrpp.apk (~64 MB)"
curl -sL --retry 3 -o sdrpp.apk "$APK_URL"

echo "==> Extracting dependency .so files"
python3 - "$KIT/lib" <<'PY'
import sys, zipfile, os, shutil
dest = sys.argv[1]
wanted = {
    'libusb1.0.so', 'libfftw3f.so', 'libvolk.so', 'libzstd.so',
    'librtlsdr.so', 'libairspy.so', 'libairspyhf.so', 'libhackrf.so',
    'libhydrasdr.so', 'libiio.so', 'libxml2.so', 'libad9361.so',
    'libcodec2.so', 'libcorrect.so', 'libfec.so',
}
with zipfile.ZipFile('sdrpp.apk') as z:
    for n in z.namelist():
        if n.startswith('lib/arm64-v8a/'):
            base = os.path.basename(n)
            if base in wanted:
                with z.open(n) as src, open(os.path.join(dest, base), 'wb') as dst:
                    shutil.copyfileobj(src, dst)
                print(f'  + {base}')
PY

############################################################
# 2) Clone each library at its pinned version (parallel)
############################################################
echo
echo "==> Cloning library sources in parallel"
( git clone -q --depth 1 --branch v1.0.25 https://github.com/libusb/libusb.git libusb ) &
( git clone -q --depth 1 https://github.com/AlexandreRouma/rtl-sdr.git rtl-sdr ) &
( git clone -q --depth 1 https://github.com/AlexandreRouma/hackrf.git hackrf ) &
( git clone -q --depth 1 https://github.com/airspy/airspyone_host.git airspy ) &
( git clone -q --depth 1 https://github.com/airspy/airspyhf.git airspyhf ) &
( git clone -q --depth 1 https://github.com/hydrasdr/rfone_host.git hydrasdr ) &
( git clone -q --depth 1 --branch v1.5.2 https://github.com/facebook/zstd.git zstd ) &
( git clone -q --depth 1 --branch v0.24 https://github.com/analogdevicesinc/libiio.git libiio ) &
( git clone -q --depth 1 --branch v0.2 https://github.com/analogdevicesinc/libad9361-iio.git libad9361 ) &
( git clone -q --depth 1 --recurse-submodules https://github.com/gnuradio/volk.git volk ) &
( curl -sL https://www.fftw.org/fftw-3.3.10.tar.gz | tar -xz && mv fftw-3.3.10 fftw ) &
( curl -sL https://github.com/drowe67/codec2-dev/archive/refs/tags/v1.0.5.tar.gz | tar -xz && mv codec2-dev-1.0.5 codec2 ) &
( curl -sL https://gitlab.gnome.org/GNOME/libxml2/-/archive/v2.9.14/libxml2-v2.9.14.tar.gz | tar -xz && mv libxml2-v2.9.14 libxml2 ) &
wait

############################################################
# 3) Generate volk's auto-generated headers (configure + build natively)
############################################################
echo
echo "==> Generating volk headers (native build, ~1 min)"
python3 -m pip install --quiet --user mako 2>/dev/null || true
( cd volk && mkdir -p build && cd build && \
    cmake .. -DENABLE_TESTING=OFF -DENABLE_MODTOOL=OFF >/dev/null 2>&1 && \
    cmake --build . -j"$(nproc 2>/dev/null || echo 2)" >/dev/null 2>&1 ) || {
        echo "ERROR: volk header generation failed. Install python3-mako and retry."; exit 1;
    }

############################################################
# 4) Copy headers into the kit
############################################################
echo
echo "==> Installing headers into $KIT/include"
mkdir -p "$KIT/include"/{libairspy,libairspyhf,libhackrf,libhydrasdr,libxml2/libxml,volk,codec2}

cp libusb/libusb/libusb.h                                            "$KIT/include/"
cp fftw/api/fftw3.h                                                  "$KIT/include/"
cp rtl-sdr/include/rtl-sdr.h rtl-sdr/include/rtl-sdr_export.h        "$KIT/include/"
cp hackrf/host/libhackrf/src/hackrf.h                                "$KIT/include/libhackrf/"
cp airspy/libairspy/src/airspy.h airspy/libairspy/src/airspy_commands.h  "$KIT/include/libairspy/"
cp airspyhf/libairspyhf/src/airspyhf.h                               "$KIT/include/libairspyhf/"
cp hydrasdr/libhydrasdr/src/hydrasdr.h hydrasdr/libhydrasdr/src/hydrasdr_commands.h  "$KIT/include/libhydrasdr/"
cp zstd/lib/zstd.h zstd/lib/zstd_errors.h zstd/lib/zdict.h           "$KIT/include/"
cp libiio/iio.h                                                      "$KIT/include/"
cp libad9361/ad9361.h                                                "$KIT/include/"
cp codec2/src/codec2.h codec2/src/codec2_fdmdv.h codec2/src/codec2_cohpsk.h \
   codec2/src/codec2_ofdm.h codec2/src/codec2_fft.h codec2/src/codec2_fifo.h \
   codec2/src/codec2_fm.h codec2/src/comp.h codec2/src/comp_prim.h \
   codec2/src/freedv_api.h codec2/src/modem_stats.h                  "$KIT/include/codec2/"
# codec2/version.h is generated by codec2's CMake from cmake/version.h.in.
# Render it manually with the v1.0.5 version constants since codec2.h #includes it.
cat > "$KIT/include/codec2/version.h" <<'CODEC2VER'
#ifndef CODEC2_HAVE_VERSION
#define CODEC2_HAVE_VERSION
#define CODEC2_VERSION_MAJOR 1
#define CODEC2_VERSION_MINOR 0
#define CODEC2_VERSION_PATCH 5
#define CODEC2_VERSION "1.0.5"
#endif
CODEC2VER
cp libxml2/include/libxml/*.h                                        "$KIT/include/libxml2/libxml/"
cp volk/include/volk/*.h volk/include/volk/*.hh                      "$KIT/include/volk/" 2>/dev/null || true
cp volk/build/include/volk/*.h                                       "$KIT/include/volk/"
cp volk/build/lib/volk_machines.h                                    "$KIT/include/volk/" 2>/dev/null || true

############################################################
# 5) Optional: KrakenSDR companion assets (--kraken flag)
############################################################
if [[ "$KRAKEN" -eq 1 ]]; then
echo
echo "==> KrakenSDR opt-in: headers, udev rules, EEPROM firmware"

# ── Protocol reference header ─────────────────────────────────────────────
mkdir -p "$KIT/include/krakensdr"

cat > "$KIT/include/krakensdr/krakensdr_doa_protocol.h" <<'KRAKENHDR'
/*
 * krakensdr_doa wire protocol — Predator RF reference header.
 *
 * krakensdr_doa streams JSON text frames over WebSocket.
 * Default port: 8081 (native krakensdr_doa); 8082 is a legacy alias.
 *
 * Native field names (krakensdr_doa ≥ v1.0 on port 8081):
 *  "frequency_hz"     double  Centre frequency (Hz)        ← preferred
 *  "doa_max_deg"      double  True bearing, [0, 360)       ← preferred
 *  "doa_std_deg"      double  1-sigma bearing uncertainty (degrees)
 *  "confidence"       double  DOA confidence, [0, 1]
 *  "power_dbfs"       double  Received power (dBFS)
 *  "snr_db"           double  Signal-to-noise ratio (dB)
 *  "gps_lat"          double  Node WGS-84 latitude
 *  "gps_lon"          double  Node WGS-84 longitude
 *  "heading_deg"      double  Platform heading (0 = north / stationary)
 *  "timestamp_unix"   double  UNIX timestamp (seconds, fractional)
 *  "node_id"          string  Device identifier (e.g. "kraken-0")
 *
 * Legacy alias names (port 8082, accepted for backward-compat):
 *  "freq_hz"          → frequency_hz
 *  "bearing_deg"      → doa_max_deg
 *  "bearing_std_deg"  → doa_std_deg
 *
 * Other message types ("status", "config", etc.) are silently ignored.
 */
#pragma once

#define KRAKEN_DOA_DEFAULT_PORT   8081
#define KRAKEN_DOA_LEGACY_PORT    8082
#define KRAKEN_DOA_DEFAULT_PATH   "/ws"
#define KRAKEN_DAQ_DEFAULT_PORT   5000
#define KRAKEN_MIN_CROSSING_DEG   15.0   /* degenerate geometry veto */
#define KRAKEN_MAX_LOB_RANGE_M    50000  /* bearing wedge drawn to this range */
KRAKENHDR

echo "  → wrote $KIT/include/krakensdr/krakensdr_doa_protocol.h"

# ── Linux udev rules (development host) ──────────────────────────────────
# Write udev rules to the local sdr-kit tree for reference and to the
# system udev directory if running as root on a Linux host.
mkdir -p "$KIT/udev"
cat > "$KIT/udev/99-krakensdr.rules" <<'UDEV'
# KrakenSDR — 5-element coherent SDR array (RTL2832U-based)
# Grant the sdr / plugdev group RW access to each of the 5 USB interfaces.
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2832", \
    MODE="0664", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2838", \
    MODE="0664", GROUP="plugdev", TAG+="uaccess"
# RTL-SDR Blog v4 (KrakenSDR coherent variant)
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="8832", \
    MODE="0664", GROUP="plugdev", TAG+="uaccess"
UDEV

if [[ "$(uname -s)" == "Linux" ]] && [[ -d /etc/udev/rules.d ]]; then
    if [[ "$EUID" -eq 0 ]]; then
        cp "$KIT/udev/99-krakensdr.rules" /etc/udev/rules.d/
        udevadm control --reload-rules 2>/dev/null || true
        echo "  → installed udev rules to /etc/udev/rules.d/"
    else
        echo "  → udev rules written to $KIT/udev/99-krakensdr.rules"
        echo "     (run as root to install system-wide)"
    fi
fi

# ── KrakenSDR EEPROM firmware ─────────────────────────────────────────────
# The KrakenSDR array requires each RTL2832U stick to have its EEPROM
# programmed with a unique serial (kraken0..kraken4) so the coherent
# driver can claim all 5 interfaces in order.  The programming utility
# is rtl_eeprom (part of rtl-sdr).  Reference EEPROM images are hosted
# in the krakenrf/krakensdr_doa repository; we download them here so
# the operator can flash offline without needing git.
EEPROM_DIR="$KIT/eeprom/krakensdr"
mkdir -p "$EEPROM_DIR"
EEPROM_BASE="https://raw.githubusercontent.com/krakenrf/krakensdr_doa/main/krakensdr_doa/eeprom"
echo "  → downloading KrakenSDR EEPROM images (0..4)"
for i in 0 1 2 3 4; do
    DST="$EEPROM_DIR/kraken${i}.eeprom"
    if [[ ! -f "$DST" ]]; then
        curl -sSfL "${EEPROM_BASE}/kraken${i}.eeprom" -o "$DST" 2>/dev/null || \
            echo "     WARNING: could not download kraken${i}.eeprom (offline?)"
    else
        echo "     kraken${i}.eeprom already present"
    fi
done
echo "  → EEPROM images at $EEPROM_DIR"
echo "     Flash with: rtl_eeprom -d <N> -f $EEPROM_DIR/kraken<N>.eeprom"
echo "     (run once per device, requires root + rtl-sdr installed)"

# ── krakensdr_doa: managed clone + optional service install ──────────────
# On a Linux host running as root, provision /opt/krakensdr_doa with the
# same steps used by deploy/install_rpi.sh --kraken (clone, pip-install,
# systemd service, env file).  On a non-root / non-Linux host, mirror the
# repo locally for offline C++ development only.
KRAKEN_OPT="/opt/krakensdr_doa"
KRAKEN_REPO="https://github.com/krakenrf/krakensdr_doa.git"

if [[ "$(uname -s)" == "Linux" ]] && [[ "$EUID" -eq 0 ]]; then
    # ── Root on Linux: full managed install ──────────────────────────────
    if [[ -d "${KRAKEN_OPT}/.git" ]]; then
        echo "  → krakensdr_doa at ${KRAKEN_OPT} — pulling latest"
        git -C "${KRAKEN_OPT}" pull --ff-only 2>/dev/null || true
    else
        echo "  → cloning krakensdr_doa into ${KRAKEN_OPT}"
        apt-get install -y --no-install-recommends git 2>/dev/null || true
        git clone --depth 1 "${KRAKEN_REPO}" "${KRAKEN_OPT}"
    fi

    if [[ -f "${KRAKEN_OPT}/requirements.txt" ]]; then
        echo "  → pip-installing krakensdr_doa requirements"
        pip3 install -q --break-system-packages \
            -r "${KRAKEN_OPT}/requirements.txt" 2>/dev/null || \
        pip3 install -q -r "${KRAKEN_OPT}/requirements.txt"
    fi

    mkdir -p /etc/krakensdr
    if [[ ! -f /etc/krakensdr/predator.env ]]; then
        cat > /etc/krakensdr/predator.env <<'KENV'
PREDATOR_LOB_WS_PORT=8082
PREDATOR_LOB_WS_PATH=/ws
KRAKEN_DOA_HOST=127.0.0.1
KRAKEN_DOA_DOA_PORT=8081
KENV
        echo "  → wrote /etc/krakensdr/predator.env"
    fi

    if [[ ! -f /etc/systemd/system/krakensdr-doa.service ]]; then
        cat > /etc/systemd/system/krakensdr-doa.service <<'SVCEOF'
[Unit]
Description=KrakenSDR DOA engine
After=network.target
[Service]
Type=simple
User=root
WorkingDirectory=/opt/krakensdr_doa
EnvironmentFile=-/etc/krakensdr/predator.env
ExecStart=/usr/bin/python3 /opt/krakensdr_doa/_UI/kraken_web_interface.py
Restart=on-failure
RestartSec=10
[Install]
WantedBy=multi-user.target
SVCEOF
        echo "  → wrote krakensdr-doa.service"
    fi
    systemctl daemon-reload 2>/dev/null || true
    systemctl enable krakensdr-doa.service 2>/dev/null || true
    echo "  → krakensdr-doa.service enabled (start: systemctl start krakensdr-doa)"
else
    # ── Non-root / macOS / CI: local mirror for offline C++ development ──
    KRAKEN_LOCAL="$(dirname "$KIT")/krakensdr_doa"
    if [[ ! -d "${KRAKEN_LOCAL}/.git" ]]; then
        echo "  → cloning krakensdr_doa into ${KRAKEN_LOCAL} (for offline dev)"
        git clone --depth 1 "${KRAKEN_REPO}" "${KRAKEN_LOCAL}" 2>/dev/null || \
            echo "     WARNING: clone failed (offline?). skipping."
    else
        echo "  → krakensdr_doa already at ${KRAKEN_LOCAL} — skipping clone"
    fi
fi

fi

echo
echo "==> Done."
echo "    Libs:    $(ls "$KIT/lib"  | wc -l) files, $(du -sh "$KIT/lib"     | cut -f1)"
echo "    Headers: $(find "$KIT/include" -type f | wc -l) files, $(du -sh "$KIT/include" | cut -f1)"
