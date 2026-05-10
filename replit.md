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
- **`AndroidManifest.xml` MUST set `android:windowSoftInputMode="adjustNothing"`** — NOT `adjustResize`. With `adjustResize` the system shrinks the GL surface when the IME appears (`ContentRectChanged` from `b=1080` to `b=486`), which shrinks `ImGui::GetIO().DisplaySize`, which shrinks the main ImGui window AND every popup inside it regardless of how the popup was sized. The active `InputText` then gets clipped, ImGui clears its active id, `io.WantTextInput` goes false, and our `backend.cpp` IME-edge logic fires `HideSoftKeyboardInput` ~1.5s after the user tapped — operator sees the keyboard pop up briefly then auto-close, can't type. With `adjustNothing` the IME floats over the GL surface, `DisplaySize` stays constant, popups stay constant, and the InputText keeps its active id. The IME's bottom inset is still reported via `WindowInsets`/`getImeBottomInset()` and is used purely for `SetScrollHereY` biasing inside the modal body — never for resizing windows or popups. Diagnostic for regression: `adb logcat` showing `ContentRectChanged b=486` (or any value < full screen height) immediately after `ImeTracker onRequestShow` means the manifest was reverted to `adjustResize`.
- Android soft-keyboard text input is captured by a **4×4 px, alpha=0.01** `EditText` overlay (`imeCaptureView` in `MainActivity.kt`), NOT by `dispatchKeyEvent`. Modern IMEs (Gboard / SwiftKey / Samsung) commit letters via `InputConnection.commitText()` and never emit hardware `KeyEvent`s for letters. The TextWatcher walks inserted text by **Unicode code point** (`Character.codePointAt` + `charCount`, NOT `s[i].code`) into `unicodeCharacterQueue` for `PollUnicodeChars()` to drain. Deletion bridges to ASCII 0x08 via two paths that MUST be deduped: (a) the `(before>0, count==0)` TextWatcher branch fires for soft-IME `deleteSurroundingText` AND for hardware DEL on a non-empty buffer — count is taken from `pendingDeleteCodepoints` computed in `beforeTextChanged` (UTF-16-unit `before` over-deletes for emoji / surrogate pairs); (b) the `KEYCODE_DEL` `OnKeyListener` ONLY fires when `edit.text.isNullOrEmpty()` — otherwise the TextWatcher path would double every backspace. NativeActivity's `NativeContentView` aggressively reclaims focus, so `showSoftInput()` must `bringToFront` → `decorView.clearFocus` → `requestFocus` → `imm.restartInput` → `showSoftInput(SHOW_FORCED)`, post a one-frame-later focus reassert, AND install an `OnFocusChangeListener` that re-fights every later focus loss while `imeKeepFocus==true`. **Primary defence is the `nativeContentView` reference (located by iterating `android.R.id.content` for the non-EditText child in `installImeCaptureView`): `showSoftInput()` flips its `isFocusable` and `isFocusableInTouchMode` to `false` so NCV physically cannot win the focus race during the IME slide-up animation; `hideSoftInput()` restores both to `true`.** Touch dispatch does NOT require focusability (only key dispatch does), so taps still reach ImGui via NCV → native_app_glue. Without this, the OnFocusChangeListener re-fight loses the race against the IME's slide-up layout passes in modals with re-anchoring popups (e.g. RNS Add Interface), and the keyboard appears to launch then auto-close on first tap. The `@Volatile imeKeepFocus` flag is set true in `showSoftInput`/false in `hideSoftInput`, and BOTH posted reassert runnables (post-frame + focus-listener) MUST re-check `imeKeepFocus` inside the `post {}` body — without that gate, a fast show→hide transition reopens the IME after the operator dismissed it. The C++ side drives BOTH edges: `backend.cpp` `renderLoop` calls `ShowSoftKeyboardInput` on rising `WantTextInput` and `HideSoftKeyboardInput` on falling — without the falling-edge hide, the IME stays up after closing a modal and shrinks every popup that opens next (because `positionRnsModal` clamps to `display − ime`). The falling edge has TWO compounding defences (frame-count alone failed because the IME slide-up animation drops the GL thread to ~20-30fps, so 6 frames can be only 200-300ms — shorter than the rise+settle window): (1) **TIME-BASED debounce of 700ms wall clock** via `WantTextFalseSinceMs` (uses `std::chrono::steady_clock`, NOT a frame counter — survives any GL stutter during IME animation); (2) **IME-rising suppression** — when `imeBottomInset` transitions 0→positive, `ImeRoseAtMs` is recorded, and any `WantTextInput=false` within the next 1200ms is treated as a layout-shrink artifact and ignored (the debounce timer is reset, not the hide call fired). The shrink artifact happens because `ContentRectChanged b=486` from the IME rise resizes the GL surface, shrinking ImGui's `DisplaySize`, which shrinks the popup, which shrinks the BeginChild, which clips the still-active InputText, which makes ImGui drop `WantTextInput` for many frames. Without both defences the operator sees `ImeTracker: onRequestHide at ORIGIN_CLIENT reason HIDE_SOFT_INPUT` ~240ms after our show in adb logcat, and the keyboard auto-closes on first tap. Rising edge stays instant. Diagnostic via `adb logcat -s PredatorRF` — look for `imeCaptureView.requestFocus() returned false` (smoking gun if typing is dropped) or `imeCapture lost focus while IME wanted — re-fighting` (focus-fight is engaging). EditText alpha must be 0.01 (NOT 0.0) and size 4×4 (NOT 1×1) — IMEs reject fully invisible / zero-size views.
- Any `BeginPopupModal` in `main_window.cpp` that contains a text input MUST anchor itself with `SetNextWindowPos` **and** `SetNextWindowSize` (with `ImGuiCond_Always`) keyed off `backend::getSafeAreaInsets()` — otherwise the popup falls back to ImGui's tiny default window size. **DO NOT subtract `backend::getImeBottomInset()` from the popup height** — doing so re-anchors the popup smaller every IME slide-up animation, shrinks the inner `BeginChild`, clips the active `InputText`, and ImGui permanently clears its active id. The operator sees the keyboard pop up, stay for ~1.5s, then close (logcat: `ImeTracker: onRequestHide at ORIGIN_CLIENT reason HIDE_SOFT_INPUT` ~1.5s after our show), and cannot type. Instead: keep the popup at full safe-area height, place action buttons (Save/Cancel/Validate) in a **header bar at the TOP** of the modal so they remain tappable while the IME covers the bottom of the popup, and use `iv()` with `SetScrollHereY(0.25f)` (when `imeBottomInset > 0`) inside the body's `BeginChild` to keep the active field above the IME line. Action button presses must be **deferred via flags** (`bool doSave/doCancel/doValidate`) and executed AFTER `EndChild()` so the form fields below the header are read at their final values for the frame. Important: `SetNextWindowSizeConstraints` alone is NOT enough without `AlwaysAutoResize`; the popup falls back to ImGui's tiny default size. The `positionRnsModal` lambda (line ~6547) anchors against `DisplaySize` directly, NOT `GetWindowPos()`/`GetWindowSize()`, because RNS modals open while the call site is deep inside nested child windows whose position/size would yield a tiny popup in a corner. Pop width/height are clamped to the **true** visible rectangle — never forced larger than `disp − safeArea − ime`. Long forms wrap their body in a `BeginChild` with an explicit positive height that reserves space for the action button row.
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