#!/usr/bin/env bash
# Predator-SDR Linux native build script
# Supports: Ubuntu 20.04/22.04/24.04, Debian 11/12
# Usage:
#   ./build_linux.sh              # full build with all hardware
#   ./build_linux.sh --minimal    # RTL-SDR + HackRF only (fewer deps)
#   ./build_linux.sh --deps-only  # install deps, skip build
#   ./build_linux.sh --clean      # wipe build dir and rebuild

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_linux"
INSTALL_DIR="$SCRIPT_DIR/install_linux"
MINIMAL=0
DEPS_ONLY=0
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --minimal)   MINIMAL=1 ;;
        --deps-only) DEPS_ONLY=1 ;;
        --clean)     CLEAN=1 ;;
        -h|--help)
            echo "Usage: $0 [--minimal] [--deps-only] [--clean]"
            exit 0 ;;
    esac
done

# ── Detect distro ────────────────────────────────────────────────────────────
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO_ID="${ID:-unknown}"
    DISTRO_VERSION="${VERSION_ID:-0}"
else
    DISTRO_ID="unknown"
    DISTRO_VERSION="0"
fi

echo "==> Predator-SDR Linux Build"
echo "    Distro  : $DISTRO_ID $DISTRO_VERSION"
echo "    Build   : $BUILD_DIR"
echo "    Minimal : $MINIMAL"
echo ""

# ── Install dependencies ─────────────────────────────────────────────────────
install_deps() {
    echo "==> Installing build dependencies..."

    if command -v apt-get &>/dev/null; then
        sudo apt-get update -qq

        PKGS=(
            build-essential cmake git pkg-config
            libfftw3-dev libglfw3-dev libzstd-dev
            librtlsdr-dev libhackrf-dev libairspy-dev libairspyhf-dev
            librtaudio-dev libsoapysdr-dev
            python3 python3-pip python3-venv
        )

        if [ "$MINIMAL" -eq 0 ]; then
            PKGS+=(
                libiio-dev libad9361-dev
                liblimesuite-dev libbladerf-dev
                portaudio19-dev libcodec2-dev
                libvolk2-dev libzstd-dev
                autoconf libtool xxd p7zip-full
            )
        fi

        sudo apt-get install -y "${PKGS[@]}"

        # libvolk fallback (some Ubuntu versions use different package name)
        if ! dpkg -l | grep -q libvolk; then
            sudo apt-get install -y libvolk-dev 2>/dev/null || \
            sudo apt-get install -y libvolk2-dev 2>/dev/null || \
            echo "    WARN: libvolk not found; spectral processing may be slower"
        fi

    elif command -v dnf &>/dev/null; then
        sudo dnf install -y \
            gcc-c++ cmake git pkgconfig \
            fftw-devel glfw-devel libzstd-devel \
            rtl-sdr-devel hackrf-devel airspy-devel \
            rtaudio-devel SoapySDR-devel \
            python3 python3-pip

    elif command -v pacman &>/dev/null; then
        sudo pacman -Sy --noconfirm \
            base-devel cmake git \
            fftw glfw zstd \
            rtl-sdr hackrf libairspy \
            rtaudio soapysdr \
            python python-pip

    else
        echo "ERROR: Unsupported package manager. Install dependencies manually."
        exit 1
    fi

    echo "==> Dependencies installed."
}

install_deps

[ "$DEPS_ONLY" -eq 1 ] && { echo "==> Deps-only mode; done."; exit 0; }

# ── Clean ────────────────────────────────────────────────────────────────────
if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "==> Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

# ── CMake configure ──────────────────────────────────────────────────────────
echo "==> Configuring CMake..."

CMAKE_OPTS=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"
    -DOPT_BACKEND_GLFW=ON
    -DOPT_BACKEND_ANDROID=OFF
    # Core SDR sources (always on)
    -DOPT_BUILD_RTL_SDR_SOURCE=ON
    -DOPT_BUILD_RTL_TCP_SOURCE=ON
    -DOPT_BUILD_HACKRF_SOURCE=ON
    -DOPT_BUILD_AIRSPY_SOURCE=ON
    -DOPT_BUILD_AIRSPYHF_SOURCE=ON
    -DOPT_BUILD_FILE_SOURCE=ON
    -DOPT_BUILD_HERMES_SOURCE=ON
    -DOPT_BUILD_RFSPACE_SOURCE=ON
    -DOPT_BUILD_SDRPP_SERVER_SOURCE=ON
    -DOPT_BUILD_SPECTRAN_HTTP_SOURCE=ON
    -DOPT_BUILD_SPYSERVER_SOURCE=ON
    -DOPT_BUILD_RTL_TCP_SOURCE=ON
    -DOPT_BUILD_SOAPY_SOURCE=ON
    # Sinks
    -DOPT_BUILD_AUDIO_SINK=ON
    -DOPT_BUILD_NETWORK_SINK=ON
    # Decoders
    -DOPT_BUILD_RADIO=ON
    -DOPT_BUILD_METEOR_DEMODULATOR=ON
    -DOPT_BUILD_DSDFME_DECODER=ON
    -DOPT_BUILD_RTL433_DECODER=ON
    # Misc modules
    -DOPT_BUILD_FREQUENCY_MANAGER=ON
    -DOPT_BUILD_RECORDER=ON
    -DOPT_BUILD_RIGCTL_CLIENT=ON
    -DOPT_BUILD_RIGCTL_SERVER=ON
    -DOPT_BUILD_SCANNER=ON
    -DOPT_BUILD_DISCORD_PRESENCE=OFF
)

if [ "$MINIMAL" -eq 0 ]; then
    CMAKE_OPTS+=(
        -DOPT_BUILD_PLUTOSDR_SOURCE=ON
        -DOPT_BUILD_LIMESDR_SOURCE=ON
        -DOPT_BUILD_BLADERF_SOURCE=ON
        -DOPT_BUILD_M17_DECODER=ON
        -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON
    )
else
    CMAKE_OPTS+=(
        -DOPT_BUILD_PLUTOSDR_SOURCE=OFF
        -DOPT_BUILD_LIMESDR_SOURCE=OFF
        -DOPT_BUILD_BLADERF_SOURCE=OFF
    )
fi

cd "$BUILD_DIR"
cmake "$SCRIPT_DIR" "${CMAKE_OPTS[@]}"

# ── Build ────────────────────────────────────────────────────────────────────
CORES=$(nproc 2>/dev/null || echo 4)
echo "==> Building with $CORES cores..."
cmake --build . --parallel "$CORES"

# ── Install locally ──────────────────────────────────────────────────────────
echo "==> Installing to $INSTALL_DIR..."
cmake --install .

# ── Python backend venv ──────────────────────────────────────────────────────
VENV_DIR="$SCRIPT_DIR/backend/.venv"
if [ ! -d "$VENV_DIR" ]; then
    echo "==> Creating Python venv for backend..."
    python3 -m venv "$VENV_DIR"
fi
echo "==> Installing Python backend dependencies..."
"$VENV_DIR/bin/pip" install -q --upgrade pip
"$VENV_DIR/bin/pip" install -q -r "$SCRIPT_DIR/backend/requirements.txt"

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║  Predator-SDR Linux build complete                   ║"
echo "╠══════════════════════════════════════════════════════╣"
echo "║  App binary : $INSTALL_DIR/bin/sdrpp"
echo "║  Run app    : $INSTALL_DIR/bin/sdrpp"
echo "║  Backend    : source backend/.venv/bin/activate"
echo "║             : python -m backend.main"
echo "╚══════════════════════════════════════════════════════╝"
