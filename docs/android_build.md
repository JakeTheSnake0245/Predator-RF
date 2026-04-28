# Predator RF — Android APK Build Guide

This guide walks you through producing a sideloadable APK of Predator RF
(this fork of SDR++) targeted at the Samsung S22 and similar arm64 Android
devices.

The Android build path is inherited intact from upstream SDR++. The Predator
additions (decoder bridges, RTL433 ingester, Diablo-tactical overlay) all
compile under the same Android NDK/CMake configuration with no extra setup.

---

## What you'll end up with

- `android/app/build/outputs/apk/debug/app-debug.apk`
  — a debug-signed APK ready to `adb install` on an arm64 device
- Runs full-screen, single-activity, no notification shade
- DPI-scaled 3× and touch-tweaked (larger scrollbars, slider grabs, borders)
  for finger input — see `core/src/gui/style.cpp::applyTouchFriendlyTweaks`

---

## Requirements

| Tool | Version | Notes |
|---|---|---|
| Android Studio | 2023.x or newer | Installs Gradle automatically |
| Android SDK Platform | API 33 | Matches `compileSdkVersion` in `android/app/build.gradle` |
| Android NDK | **23.2.8568313** exactly | Pinned via `ndkVersion` in `android/app/build.gradle` |
| CMake | 3.21+ (bundled with NDK) | |
| Java | JDK 17 | Bundled with Android Studio |
| `sdr-kit` (precompiled SDR libs for Android) | latest | See section below |
| USB-C cable + Android device with **USB Debugging enabled** | | For sideloading |

The Samsung S22 is an arm64-v8a device — the only ABI this build targets.

---

## Step 1 — Install Android Studio + NDK 23.2

1. Install Android Studio.
2. Open **SDK Manager → SDK Platforms** and install **Android API 33**.
3. Open **SDK Manager → SDK Tools**, tick "Show Package Details", expand
   **NDK (Side by side)**, and install **23.2.8568313** specifically. Other
   NDK versions will fail with a fatal error at the CMake step.
4. Note the SDK path (typically `~/Android/Sdk` on Linux/macOS,
   `C:\Users\YOU\AppData\Local\Android\Sdk` on Windows).

---

## Step 2 — Get the `sdr-kit`

The Android build needs precompiled native libraries for SDR hardware
support: RTL-SDR, HackRF, Airspy, AirspyHF, PlutoSDR, etc. These live
outside the repo in a separate `sdr-kit` directory keyed by ABI.

The expected layout is:

```
sdr-kit/
└── arm64-v8a/
    ├── include/
    │   ├── rtl-sdr.h
    │   ├── libhackrf/
    │   ├── libairspy/
    │   └── ...
    └── lib/
        ├── librtlsdr.so
        ├── libhackrf.so
        ├── libairspy.so
        └── ...
```

You have two options:

### Option A — Download a prebuilt sdr-kit (fastest)

Upstream SDR++ publishes prebuilt arm64 SDR kits in its releases. Look for
an archive named like `android-sdr-kit-arm64-v8a-*.zip` on the
[SDR++ releases page](https://github.com/AlexandreRouma/SDRPlusPlus/releases)
and extract it to a known path.

### Option B — Build sdr-kit yourself

Each driver library has its own NDK cross-compile recipe in upstream SDR++
under `android/sdr-kit/`. Follow each library's individual build steps
against `arm64-v8a` and the same NDK 23.2.

For most users, Option A is fine.

---

## Step 3 — Point the build at your SDK + sdr-kit

Create `android/local.properties`:

```properties
sdk.dir=/absolute/path/to/Android/Sdk
sdr.kit.dir=/absolute/path/to/sdr-kit
```

On Windows use forward slashes or escape backslashes:

```properties
sdk.dir=C:/Users/YOU/AppData/Local/Android/Sdk
sdr.kit.dir=C:/dev/sdr-kit
```

Alternatively, set the environment variable `SDR_KIT_ROOT` and `local.properties`
will pick it up automatically.

If you do not configure `sdr.kit.dir` (or `SDR_KIT_ROOT`) the CMake step
will fail with:

```
SDR_KIT_ABI_ROOT does not exist: /sdr-kit/arm64-v8a
```

---

## Step 4 — Build the APK

From the project root:

```bash
cd android
./gradlew assembleDebug
```

On Windows:

```cmd
cd android
gradlew.bat assembleDebug
```

First build downloads Gradle (~150 MB), the Android Gradle Plugin, and
compiles the entire SDR++ core + every enabled source/sink/decoder module
for arm64. **Expect 8–15 minutes on a modern laptop, longer on first run.**

Output:

```
android/app/build/outputs/apk/debug/app-debug.apk
```

Typical size: 25–40 MB depending on which SDR drivers are bundled.

---

## Step 5 — Install on your S22

1. Enable USB Debugging on the S22:
   **Settings → About phone → Software information → tap "Build number" 7×**
   (this unlocks Developer Options), then
   **Settings → Developer options → USB debugging → ON**.

2. Connect the S22 via USB-C and accept the "Allow USB debugging?" prompt
   on the phone.

3. From the project root:

   ```bash
   adb install -r android/app/build/outputs/apk/debug/app-debug.apk
   ```

   `-r` reinstalls over an existing copy if you've installed before.

4. Launch **SDR++** from your app drawer. (App label still shows "SDR++"
   from the upstream `applicationId org.sdrpp.sdrpp` — see the
   "Rebranding to Predator RF" section below if you want the launcher icon
   and label changed.)

---

## Step 6 — Connect an SDR to the S22

Predator RF on Android supports any SDR with a USB OTG driver (most
RTL-SDRs, HackRF One, Airspy R2/Mini/HF+, PlutoSDR over USB).

1. Plug the SDR into the S22 via a USB-C OTG adapter.
2. Android will prompt "Use SDR++ when SDR is connected?" — accept.
3. In the app, open the SDR settings panel and pick your radio from the
   source dropdown. Predator features (Hits, Network, Map, Mission, Bridges)
   become live as soon as the SDR is streaming.

---

## What you'll see on the S22

With `style::uiScale = 3.0f` and `applyTouchFriendlyTweaks()` applied,
the layout on a 1080×2340 portrait S22 screen renders as:

| Region | Approx. logical size | Purpose |
|---|---|---|
| Top status bar | full-width, 126 px | Mission status, lockout tally, GPS state |
| Control bar | full-width, 138 px | Tuning, Listen, Scan, QuickScan, Log Event |
| Right-side icon rail | 192 px wide | 6 Predator tabs (SPEC / HITS / NET / MAP / MISN / SYS) |
| Spectrum / waterfall | ~816 px wide × 1800 px tall | Always visible, behind any open tab |
| Tab overlay (when open) | ~636 px wide × 1800 px tall, slides in over right side of waterfall | Active tab content |
| Bottom safety footer | full-width text | "RX · ANALYZE · LOG · MAP" |

All paddings, font sizes, scrollbar widths, slider grabs, and borders
scale uniformly with `uiScale`, so the same layout adapts to a tablet at
`uiScale = 3.5f` or a smaller phone at `uiScale = 2.5f` without code
changes — just override `uiScale` in `core/src/gui/style.cpp`.

---

## Troubleshooting

**"SDR_KIT_ABI_ROOT does not exist: /sdr-kit/arm64-v8a"**
You skipped Step 3. Set `sdr.kit.dir` in `android/local.properties` or
the `SDR_KIT_ROOT` env var.

**"NDK 23.2.8568313 not found"**
Open Android Studio's SDK Manager, expand "NDK (Side by side)", and
install exactly that version. Other NDK versions will compile but link
against a different libc++ ABI than the precompiled `sdr-kit` libraries.

**Build hangs on "Configuring CMake project"**
First-build CMake configuration takes 2–4 minutes per ABI. Be patient.
If it sits longer than 8 minutes, kill it and check
`android/app/.cxx/Debug/<hash>/arm64-v8a/build.ninja.log` for errors.

**APK installs but crashes immediately on launch**
Almost always a missing `sdr-kit` library at runtime. Check `adb logcat
| grep -i "sdrpp\|dlopen"` and confirm every `.so` referenced by the app
exists under `sdr-kit/arm64-v8a/lib/`.

**Spectrum is blank but UI works**
The SDR isn't streaming. Confirm the OTG connection, that the radio
appears under the source selector, and that you've tapped **Start
Listening**.

**ImGui buttons feel too small to tap accurately**
Increase `style::uiScale` in `core/src/gui/style.cpp` from `3.0f` to
`3.25f` or `3.5f` and rebuild. The touch-friendly tweaks scale with it.

---

## Rebranding to Predator RF (optional)

To change the app label, package, and launcher icon from "SDR++" to
"Predator RF":

1. In `android/app/build.gradle`, change:
   ```gradle
   namespace "org.sdrpp.sdrpp"
   applicationId "org.sdrpp.sdrpp"
   ```
   to your desired identifier (e.g. `dev.predator.rf`).

2. In `android/app/src/main/AndroidManifest.xml`, change `android:label`
   to `Predator RF`.

3. Replace the launcher icons under
   `android/app/src/main/res/mipmap-*/ic_launcher.png` with your icon at
   the matching densities (mdpi, hdpi, xhdpi, xxhdpi, xxxhdpi).

4. Bump `versionCode` and `versionName` in `android/app/build.gradle`
   so re-installs are not blocked.

---

## Safety boundary reminder

The Android build inherits the same RX-only safety boundary as the
desktop build. There is no transmit code path in this fork. The
`OPT_BUILD_*` flags in `android/app/build.gradle` enable only receivers
(`rtl_sdr_source`, `hackrf_source`, `airspy_source`, etc.) and the
`network_sink` / `audio_sink` outputs — never the upstream
`pluto_transmitter` or any TX module. Confirm by inspecting the
`targets` list in `android/app/build.gradle` before publishing.
