---
name: Android native module wiring
description: A native SDR++ module needs BOTH the top-level CMake add_subdirectory AND an explicit entry in android/app/build.gradle's externalNativeBuild targets list to ship in the APK.
---

# Android native module packaging requires two wiring points

Adding a `source_modules/` / `decoder_modules/` module to the top-level
`CMakeLists.txt` (option + `add_subdirectory`) is **not** enough for it to
appear in the Android APK. The Android Gradle build only compiles/packages
the CMake targets explicitly listed in
`android/app/build.gradle` → `android.defaultConfig.externalNativeBuild.cmake.targets`.

**Why:** `add_subdirectory` runs during CMake *configure* (so the target is
defined), but AGP builds only the named `targets`. A module missing from that
list configures cleanly and produces no error — it just silently never ships.
A feature can look fully wired (CMake option ON by default) yet be absent on
the phone.

**How to apply:** Whenever you add a native module intended for Android, add
its target name to that `targets` list in `android/app/build.gradle` in
addition to the top-level CMake wiring. Verify module sources are
Android/bionic-safe (POSIX socket headers guarded by `#ifndef _WIN32`; only
standard SDR++ module includes like `module.h`, `signal_path/*`, `imgui.h`).

`_DELETE_INSTANCE_` may be declared with either `void*` or
`ModuleManager::Instance*` — both are ABI-compatible because the loader
`dlsym`-casts uniformly. Not a correctness concern.
