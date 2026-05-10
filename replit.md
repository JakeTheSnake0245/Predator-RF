# Predator RF

Predator RF is a joint sensing platform for a solo SIGINT operator using Raspberry Pi/SDR/GPS sensors for RX-only signal logging and mapping.

## Run & Operate

_Populate as you build_

## Stack

**C++ Application:**
- **Build System:** CMake
- **UI Framework:** Dear ImGui
- **Graphics:** OpenGL / GLES 3
- **DSP:** FFTW3, Volk
- **Android Wrapper:** Kotlin/JNI

**Python Backend:**
- **Framework:** FastAPI, asyncio

## Where things live

- `server.py`: Python HTTP server for the informational landing page (`index.html`) and an operator UI mockup (`preview.html`).
- `core/src/predator/kujhad_fleet.h`: Header-only Kujhad Fleet hub-and-spoke peer protocol (HTTP/TLS server).
- `core/src/predator/decoder_ingest.h`: Header-only receive-only decoder ingestion base class.
- `decoder_modules/rtl433_decoder/`: Native rtl_433 ISM decoder module.
- `core/src/gui/style.cpp`: Contains `applyTouchFriendlyTweaks()` for Android UI adjustments.
- `docs/android_build.md`: End-to-end APK build guide.
- `android/sdr-kit/arm64-v8a/`: Prebuilt native SDR libraries for Android.
- `scripts/fetch-sdr-kit.sh`: Script to refresh `android/sdr-kit/`.
- `CMakeLists.txt`: CMake build configuration for the C++ application.
- `backend/`: Python intelligence backend.
- `deploy/`: Deployment scripts and configurations for the Python backend.
- `docs/`: Project documentation, including API contracts and integration guides.

## Architecture decisions

- **Two-tier system:** Python backend consumes the Kujhad Fleet HTTP API from C++ Predator-RF nodes.
- **RX-only focus:** The C++ build hard-rejects `tx.*` commands at the wire layer for security and simplicity.
- **Multi-transport for CoT:** CoT XML fans out over both IP (TAK UDP/TCP) and RNS (`predatorrf/cot.v1`) simultaneously.
- **Manual CoT export:** CoT export is operator-initiated only in v1, even if the DecisionEngine advises escalation.
- **Android UI scaling:** Dynamic UI scaling with touch-friendly tweaks applied based on device detection.

## Product

- **Signal Intelligence:** DSP, decoders (RTL433, P25, ADS-B), and hit/event management.
- **Mission System:** Manual, Classify, Scan, and QuickScan modes with search bands, targets/excludes, and peak detection.
- **Kujhad Fleet Console:** Hub-and-spoke peer protocol for linking multiple Predator RF instances, with Controller and Device roles. Includes spectrum mirroring.
- **Native Decoders:** Integrated native modules for RTL433 and DSD-FME (P25).
- **Mapping:** MapLibre GL JS with 2D/3D views, layer toggles, and compass.
- **Intelligence Backend (Python):** Consumes Kujhad API, manages tracks, detects anomalies, handles operator missions, approvals, and overrides.
- **CoT Export:** Exports data in Cursor-on-Target (CoT) format for external systems like ATAK.

## User preferences

- _Populate as you build_

## Gotchas

- The Replit environment only serves an informational landing page and interactive UI mockup (`server.py`). It does **not** run the Android build or the Python backend.
- `X-Kujhad-Key` header is required for authentication on all `/v1/*` Kujhad API calls.
- The `predatorrf/cot.v1` RNS Destination is additive, not a replacement for TCP/TLS Kujhad control-plane transport.
- For Android builds, `assembleDebug` is the documented happy path as `release` is unsigned by design.
- Android soft-keyboard text input is captured by a **4×4 px, alpha=0.01** `EditText` overlay (`imeCaptureView` in `MainActivity.kt`), NOT by `dispatchKeyEvent`. Modern IMEs (Gboard / SwiftKey / Samsung) commit letters via `InputConnection.commitText()` and never emit hardware `KeyEvent`s for letters. The TextWatcher walks inserted text by **Unicode code point** (`Character.codePointAt` + `charCount`, NOT `s[i].code`) into `unicodeCharacterQueue` for `PollUnicodeChars()` to drain. Deletion bridges to ASCII 0x08 via two paths that MUST be deduped: (a) the `(before>0, count==0)` TextWatcher branch fires for soft-IME `deleteSurroundingText` AND for hardware DEL on a non-empty buffer — count is taken from `pendingDeleteCodepoints` computed in `beforeTextChanged` (UTF-16-unit `before` over-deletes for emoji / surrogate pairs); (b) the `KEYCODE_DEL` `OnKeyListener` ONLY fires when `edit.text.isNullOrEmpty()` — otherwise the TextWatcher path would double every backspace. NativeActivity's `NativeContentView` aggressively reclaims focus, so `showSoftInput()` must `bringToFront` → `decorView.clearFocus` → `requestFocus` → `imm.restartInput` → `showSoftInput(SHOW_FORCED)`, post a one-frame-later focus reassert, AND install an `OnFocusChangeListener` that re-fights every later focus loss while `imeKeepFocus==true`. The `@Volatile imeKeepFocus` flag is set true in `showSoftInput`/false in `hideSoftInput`, and BOTH posted reassert runnables (post-frame + focus-listener) MUST re-check `imeKeepFocus` inside the `post {}` body — without that gate, a fast show→hide transition reopens the IME after the operator dismissed it. The C++ side drives BOTH edges: `backend.cpp` `renderLoop` calls `ShowSoftKeyboardInput` on rising `WantTextInput` and `HideSoftKeyboardInput` on falling — without the falling-edge hide, the IME stays up after closing a modal and shrinks every popup that opens next (because `positionRnsModal` clamps to `display − ime`). Diagnostic via `adb logcat -s PredatorRF` — look for `imeCaptureView.requestFocus() returned false` (smoking gun if typing is dropped) or `imeCapture lost focus while IME wanted — re-fighting` (focus-fight is engaging). EditText alpha must be 0.01 (NOT 0.0) and size 4×4 (NOT 1×1) — IMEs reject fully invisible / zero-size views.
- Any `BeginPopupModal` in `main_window.cpp` that contains a text input MUST anchor itself with `SetNextWindowPos` **and** `SetNextWindowSize` (with `ImGuiCond_Always`) keyed off `backend::getImeBottomInset()` and `backend::getSafeAreaInsets()` — otherwise the soft keyboard covers the field on Android. Important: `SetNextWindowSizeConstraints` alone is NOT enough without `AlwaysAutoResize`; the popup falls back to ImGui's tiny default size. The `positionRnsModal` lambda (line ~6547) anchors against `DisplaySize` directly, NOT `GetWindowPos()`/`GetWindowSize()`, because RNS modals open while the call site is deep inside nested child windows whose position/size would yield a tiny popup in a corner. Pop width/height are clamped to the **true** visible rectangle — never forced larger than `disp − safeArea − ime`. Long forms wrap their body in a `BeginChild` with an explicit positive height that reserves space for the action button row.
- Even with correct popup sizing, `BeginChild` shrinks when the IME rises and the *active* InputText can fall under the keyboard. The RNS Add/Edit modal uses an `iv()` lambda (defined right after `BeginChild`, called after every `Input*` widget) that calls `SetScrollHereY(0.5f)` on either `IsItemActivated()` (just-tapped) or `imeJustRose && IsItemActive()` (keyboard arrived after tap). ImGui 1.87 has no public `GetItemID()` — that's why the lambda relies on the immediately-after-Input call site rather than ID comparison.
- `backend::SafeAreaInsets` is declared in the cross-platform `core/src/backend.h`. The Android implementation is in `backend.cpp` (publishes the values written by `MainActivity.installInsetListener`); GLFW returns all zeros.

## Pointers

- [SDR++ GitHub](https://github.com/AlexandreRouma/SDRPlusPlus)
- `backend/rns/README.md`
- `docs/rns_parity.md`
- `docs/rns_field_log.md`
- `docs/1_conops.md`
- `docs/android_build.md`
- `docs/OPERATOR_RUNBOOK.md`
- `docs/MISSION_READY_CHECKLIST.md`
- `docs/ATAK_COT_FORMAT.md`
- `docs/ANDROID_INTEGRATION.md`
- `docs/SIDELOAD_README.md`