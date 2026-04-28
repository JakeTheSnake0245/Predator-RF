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
# Usage:  bash scripts/fetch-sdr-kit.sh

set -euo pipefail

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

echo
echo "==> Done."
echo "    Libs:    $(ls "$KIT/lib"  | wc -l) files, $(du -sh "$KIT/lib"     | cut -f1)"
echo "    Headers: $(find "$KIT/include" -type f | wc -l) files, $(du -sh "$KIT/include" | cut -f1)"
