# Android SDR Kit (`android/sdr-kit/`)

This directory contains the prebuilt native SDR libraries that the Android build
of Predator RF links against. It is committed to the repo so a fresh
`git clone` can build the APK without any extra setup.

## What's in here

```
android/sdr-kit/
└── arm64-v8a/
    ├── lib/        # 15 stripped .so files (~6.8 MB)  — link targets
    └── include/    # 94 public headers (~1 MB)        — compile-time headers
```

The `arm64-v8a` ABI is the only one shipped (matches the Samsung S22 target).
Other ABIs (x86, x86_64, armeabi-v7a) are not included to keep the repo small.

### Libraries provided

| `.so` | Source | Purpose |
| --- | --- | --- |
| `libusb1.0.so` | libusb 1.0.25 | USB transport for all USB SDRs |
| `libfftw3f.so` | FFTW 3.3.10 | Single-precision FFTs for the waterfall |
| `libvolk.so` | gnuradio/volk (master) | SIMD vector kernels |
| `libzstd.so` | facebook/zstd 1.5.2 | Compression (PlutoSDR, recordings) |
| `librtlsdr.so` | AlexandreRouma/rtl-sdr | RTL2832U dongles |
| `libairspy.so` | airspy/airspyone_host | Airspy R0/R2/Mini |
| `libairspyhf.so` | airspy/airspyhf | Airspy HF+ Discovery |
| `libhackrf.so` | AlexandreRouma/hackrf | HackRF One |
| `libhydrasdr.so` | hydrasdr/rfone_host | HydraSDR RFOne |
| `libiio.so` | analogdevicesinc/libiio v0.24 | PlutoSDR / IIO transport |
| `libxml2.so` | libxml2 2.9.14 | XML parsing for libiio |
| `libad9361.so` | analogdevicesinc/libad9361-iio v0.2 | PlutoSDR RF frontend |
| `libcodec2.so` | drowe67/codec2-dev v1.0.5 | Voice codec for M17 / FreeDV |
| `libcorrect.so`, `libfec.so` | upstream sdr-kit | FEC for digital decoders |

## Where these came from

The `.so` binaries were extracted from the upstream SDR++ Android nightly APK
(`AlexandreRouma/SDRPlusPlus`, asset `sdrpp.apk`), which was built with the
official upstream Docker recipe (`AlexandreRouma/android-sdr-kit`). The headers
were assembled from each library's public source at the version pin recorded
in the upstream `build.sh`.

This packaging approach is documented and reproducible — see
`scripts/fetch-sdr-kit.sh` for the exact commands.

## How to refresh / rebuild

If you need to regenerate the kit from scratch (e.g. to pick up a newer
upstream nightly), run from the repo root:

```sh
bash scripts/fetch-sdr-kit.sh
```

That script will:

1. Download the latest `sdrpp.apk` from upstream releases
2. Extract the dependency `.so` files into `arm64-v8a/lib/`
3. Clone each library at the pinned version, build only what's needed to
   generate the auto-generated headers (volk), and copy the public headers into
   `arm64-v8a/include/`

## How the build picks up the kit

Resolution order, defined in `android/app/build.gradle` and `CMakeLists.txt`:

1. `sdr.kit.dir=...` in `android/local.properties`
2. `SDR_KIT_ROOT` environment variable
3. **This repo's `android/sdr-kit/` directory (default)**
4. Legacy `/sdr-kit` (only matters on hand-built Linux setups)

So if you don't override anything, the build just works.

## ABI / NDK notes

- These `.so` files were compiled by upstream against **NDK 25.1.8937393**.
- Predator RF's `android/app/build.gradle` currently pins `ndkVersion "23.2.8568313"`.
- The libc++ ABI is generally compatible across NDK 23–25, but if you hit link
  errors at APK time, switching `ndkVersion` to `25.1.8937393` is the first
  thing to try.
