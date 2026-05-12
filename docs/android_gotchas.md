# Android Gotchas

This file holds the deeply Android-specific footguns for Predator RF —
NativeActivity quirks, IME plumbing, EGL/ImGui warm-restart, JNI life-cycle
leaks, and DSD-FME native-decoder traps. They were extracted from
`replit.md` because they are large, change rarely, and only matter when
you are working inside `MainActivity.kt`, `backend.cpp`, the
`AndroidManifest.xml`, or the native decoder modules.

If you are touching any of those files, read this whole document before
making changes — every section here is a scar from a real incident.

---

## Manifest: `windowSoftInputMode="adjustNothing"`

`AndroidManifest.xml` MUST set `android:windowSoftInputMode="adjustNothing"`
— NOT `adjustResize`.

With `adjustResize` the system shrinks the GL surface when the IME appears
(`ContentRectChanged` from `b=1080` to `b=486`), which shrinks
`ImGui::GetIO().DisplaySize`, which shrinks the main ImGui window AND
every popup inside it regardless of how the popup was sized. The active
`InputText` then gets clipped, ImGui clears its active id,
`io.WantTextInput` goes false, and our `backend.cpp` IME-edge logic fires
`HideSoftKeyboardInput` ~1.5s after the user tapped — operator sees the
keyboard pop up briefly then auto-close, can't type.

With `adjustNothing` the IME floats over the GL surface, `DisplaySize`
stays constant, popups stay constant, and the InputText keeps its active
id. The IME's bottom inset is still reported via
`WindowInsets`/`getImeBottomInset()` and is used purely for
`SetScrollHereY` biasing inside the modal body — never for resizing
windows or popups.

**Diagnostic for regression:** `adb logcat` showing
`ContentRectChanged b=486` (or any value < full screen height) immediately
after `ImeTracker onRequestShow` means the manifest was reverted to
`adjustResize`.

---

## Soft-keyboard text input via 4×4 EditText overlay

Android soft-keyboard text input is captured by a **4×4 px, alpha=0.01**
`EditText` overlay (`imeCaptureView` in `MainActivity.kt`), NOT by
`dispatchKeyEvent`. Modern IMEs (Gboard / SwiftKey / Samsung) commit
letters via `InputConnection.commitText()` and never emit hardware
`KeyEvent`s for letters.

### TextWatcher → unicode queue

The TextWatcher walks inserted text by **Unicode code point**
(`Character.codePointAt` + `charCount`, NOT `s[i].code`) into
`unicodeCharacterQueue` for `PollUnicodeChars()` to drain.

### Backspace de-duplication

Deletion bridges to ASCII `0x08` via two paths that MUST be deduped:
1. The `(before>0, count==0)` TextWatcher branch fires for soft-IME
   `deleteSurroundingText` AND for hardware DEL on a non-empty buffer —
   count is taken from `pendingDeleteCodepoints` computed in
   `beforeTextChanged` (UTF-16-unit `before` over-deletes for emoji /
   surrogate pairs).
2. The `KEYCODE_DEL` `OnKeyListener` ONLY fires when
   `edit.text.isNullOrEmpty()` — otherwise the TextWatcher path would
   double every backspace.

### Focus race against NativeContentView

NativeActivity's `NativeContentView` aggressively reclaims focus, so
`showSoftInput()` must `bringToFront` → `decorView.clearFocus` →
`requestFocus` → `imm.restartInput` → `showSoftInput(SHOW_FORCED)`, post
a one-frame-later focus reassert, AND install an `OnFocusChangeListener`
that re-fights every later focus loss while `imeKeepFocus==true`.

**Primary defence is the `nativeContentView` reference** (located by
iterating `android.R.id.content` for the non-EditText child in
`installImeCaptureView`): `showSoftInput()` flips its `isFocusable` and
`isFocusableInTouchMode` to `false` so NCV physically cannot win the
focus race during the IME slide-up animation; `hideSoftInput()` restores
both to `true`. Touch dispatch does NOT require focusability (only key
dispatch does), so taps still reach ImGui via NCV → native_app_glue.
Without this, the OnFocusChangeListener re-fight loses the race against
the IME's slide-up layout passes in modals with re-anchoring popups
(e.g. RNS Add Interface), and the keyboard appears to launch then
auto-close on first tap.

### `imeKeepFocus` re-check

The `@Volatile imeKeepFocus` flag is set true in `showSoftInput`/false
in `hideSoftInput`, and BOTH posted reassert runnables (post-frame +
focus-listener) MUST re-check `imeKeepFocus` inside the `post {}` body
— without that gate, a fast show→hide transition reopens the IME after
the operator dismissed it.

### Show / Hide edge driving from C++

The C++ side drives BOTH edges: `backend.cpp` `renderLoop` calls
`ShowSoftKeyboardInput` on rising `WantTextInput` and
`HideSoftKeyboardInput` on falling — without the falling-edge hide, the
IME stays up after closing a modal and shrinks every popup that opens
next (because `positionRnsModal` clamps to `display − ime`).

The falling edge has TWO compounding defences (frame-count alone failed
because the IME slide-up animation drops the GL thread to ~20-30fps, so
6 frames can be only 200-300ms — shorter than the rise+settle window):

1. **TIME-BASED debounce of 700ms wall clock** via
   `WantTextFalseSinceMs` (uses `std::chrono::steady_clock`, NOT a frame
   counter — survives any GL stutter during IME animation).
2. **IME-rising suppression** — when `imeBottomInset` transitions
   0→positive, `ImeRoseAtMs` is recorded, and any `WantTextInput=false`
   within the next 1200ms is treated as a layout-shrink artifact and
   ignored (the debounce timer is reset, not the hide call fired). The
   shrink artifact happens because `ContentRectChanged b=486` from the
   IME rise resizes the GL surface, shrinking ImGui's `DisplaySize`,
   which shrinks the popup, which shrinks the BeginChild, which clips
   the still-active InputText, which makes ImGui drop `WantTextInput`
   for many frames.

Without both defences the operator sees
`ImeTracker: onRequestHide at ORIGIN_CLIENT reason HIDE_SOFT_INPUT`
~240ms after our show in adb logcat, and the keyboard auto-closes on
first tap. Rising edge stays instant.

**Diagnostic** via `adb logcat -s PredatorRF` — look for
`imeCaptureView.requestFocus() returned false` (smoking gun if typing is
dropped) or `imeCapture lost focus while IME wanted — re-fighting`
(focus-fight is engaging). EditText alpha must be 0.01 (NOT 0.0) and
size 4×4 (NOT 1×1) — IMEs reject fully invisible / zero-size views.

---

## `BeginPopupModal` sizing rules

Any `BeginPopupModal` in `main_window.cpp` that contains a text input
MUST anchor itself with `SetNextWindowPos` **and** `SetNextWindowSize`
(with `ImGuiCond_Always`) keyed off `backend::getSafeAreaInsets()` —
otherwise the popup falls back to ImGui's tiny default window size.

**DO NOT subtract `backend::getImeBottomInset()` from the popup
height** — doing so re-anchors the popup smaller every IME slide-up
animation, shrinks the inner `BeginChild`, clips the active `InputText`,
and ImGui permanently clears its active id. The operator sees the
keyboard pop up, stay for ~1.5s, then close (logcat:
`ImeTracker: onRequestHide at ORIGIN_CLIENT reason HIDE_SOFT_INPUT`
~1.5s after our show), and cannot type.

Instead: keep the popup at full safe-area height, place action buttons
(Save/Cancel/Validate) in a **header bar at the TOP** of the modal so
they remain tappable while the IME covers the bottom of the popup, and
use `iv()` with `SetScrollHereY(0.25f)` (when `imeBottomInset > 0`)
inside the body's `BeginChild` to keep the active field above the IME
line. Action button presses must be **deferred via flags**
(`bool doSave/doCancel/doValidate`) and executed AFTER `EndChild()` so
the form fields below the header are read at their final values for the
frame.

`SetNextWindowSizeConstraints` alone is NOT enough without
`AlwaysAutoResize`; the popup falls back to ImGui's tiny default size.
The `positionRnsModal` lambda (line ~6547) anchors against `DisplaySize`
directly, NOT `GetWindowPos()`/`GetWindowSize()`, because RNS modals
open while the call site is deep inside nested child windows whose
position/size would yield a tiny popup in a corner. Pop width/height are
clamped to the **true** visible rectangle — never forced larger than
`disp − safeArea − ime`. Long forms wrap their body in a `BeginChild`
with an explicit positive height that reserves space for the action
button row.

### `iv()` lambda for active-field scroll

Even with correct popup sizing, `BeginChild` shrinks when the IME rises
and the *active* InputText can fall under the keyboard. The RNS
Add/Edit modal uses an `iv()` lambda (defined right after `BeginChild`,
called after every `Input*` widget) that calls `SetScrollHereY(0.5f)`
on either `IsItemActivated()` (just-tapped) or
`imeJustRose && IsItemActive()` (keyboard arrived after tap). ImGui
1.87 has no public `GetItemID()` — that's why the lambda relies on the
immediately-after-Input call site rather than ID comparison.

### `SafeAreaInsets` declaration

`backend::SafeAreaInsets` is declared in the cross-platform
`core/src/backend.h`. The Android implementation is in `backend.cpp`
(publishes the values written by `MainActivity.installInsetListener`);
GLFW returns all zeros.

---

## CoT enable bridge (C++ JSON ↔ Python env)

The C++ UI checkbox writes `cotEnabled` (camelCase) to
`${filesDir}/config.json` via `core::configManager`, but the Python
backend's `config.cot_enabled` reads the `COT_ENABLED` env var **once at
startup** (default `False`) and never sees the JSON file. Without
bridging, ticking "Enable TAK CoT reporting" in the UI fires the C++
`CotReporter` but the Python `CoTEmitter` stays off (logcat:
`CoTEmitter disabled (cot_enabled=false)`).

`PredatorBackendService.bridgeCppConfigToEnv()` parses `config.json` and
exports `COT_ENABLED` / `COT_DEST_HOST` / `COT_DEST_PORT` via
`android.system.Os.setenv()` **before** `Python.start()` — must run
before, because Chaquopy boots the interpreter and dataclass
`default_factory` evaluates env on first import. Toggling the checkbox
after launch requires an app restart for the backend side to pick it
up; the C++ CotReporter still updates live every frame via
`lastCotCfg` diff.

---

## Warm-restart SIGABRT in `ImGui_ImplOpenGL3_Init`

When MainActivity is destroyed and recreated while the process stays
alive (e.g. user backs out + reopens, screen lock + unlock, foreground
service notification interaction), `android_main` is called a second
time and takes the `if (backend::initialized)` branch at backend.cpp
~line 783 → `doPartialInit()` → `backend::init()`. If
`APP_CMD_TERM_WINDOW` did not fire `backend::end()` before the
warm-restart, the previous ImGui context and EGL context handles are
still in static state. `ImGui::CreateContext()` then runs on top of
stale state, and `ImGui_ImplOpenGL3_Init()` SIGABRTs at `+380`
(gl3w/glad loader runs against a destroyed GL context).

**Fix:** defensive teardown at the top of `backend::init()` —
- if `ImGui::GetCurrentContext() != nullptr` call
  `ImGui_ImplOpenGL3_Shutdown` + `ImGui_ImplAndroid_Shutdown` +
  `ImGui::DestroyContext`;
- if `_EglDisplay != EGL_NO_DISPLAY` `eglMakeCurrent(EGL_NO...)` and
  destroy `_EglContext`/`_EglSurface` (but NOT the display —
  `eglInitialize` is idempotent and terminating the display breaks
  Samsung OneUI IME).
- DO NOT call `ANativeWindow_release(app->window)` in this teardown —
  `backend::app->window` has already been swapped to the NEW window by
  `android_main` before `init()` runs, and releasing it corrupts the
  new window's refcount.

**Diagnostic** via `adb logcat`: stack `ImGui_ImplOpenGL3_Init+380` →
`backend::init+588` → `backend::doPartialInit+108` → `android_main+88`
with SIGABRT (signal 6) means the warm-restart teardown was lost.
Symptom for operator: app crashes ~every few minutes during normal use,
RF "stops" because each crash + auto-restart cycle re-enumerates the
HackRF (`Could not open HackRF fake_serial: HackRF not found` while USB
descriptor recovers).

---

## DSD-FME decoder freeze (app appears to stop listening)

Four-way conspiracy in `decoder_modules/dsdfme_decoder/`:

1. **Busy-spin in empty input ring.** `predator_dsd_pull_input_sample`
   (vendor/.../predator_dsdfme_stubs.c) used to return silence with no
   backoff when the input ring was empty — `getSymbol()` then
   busy-spins at 100% on one core, which under Android thermal throttle
   starves the SDR DSP graph (no new RF samples push into the ring) AND
   the audio sink chain (`rawVoice_.swap()` blocks waiting for the sink
   to drain), so the operator sees the entire app freeze. **Fix:**
   500 µs `nanosleep` in the empty-ring path.
2. **Per-frame allocator contention.** `sampleHandler` in `src/main.cpp`
   allocated a fresh `std::vector<int16_t>(count)` per RF DSP frame at
   48 kHz, contending with the SDR++ DSP allocator under pressure.
   **Fix:** pre-allocated `sampleScratch_` member buffer (reserve 4096,
   grow on overshoot, never shrink).
3. **6 MB malloc on GUI thread at module load.**
   `predator_dsd_init_decoder()` was called in the module
   **constructor** on the GUI thread — that init does ~6 MB of mallocs
   (`initState()` allocates dibit_buf, dmr_payload_buf, audio_out_buf,
   mbe parm structs). On a memory-pressured Android the GUI thread
   stutters or OOMs at module load. **Fix:** removed the constructor
   call entirely — `predator_dsd_run_decoder_loop()` already calls
   `predator_dsd_init_decoder()` from the worker thread (it's
   mutex-guarded and idempotent), so the cost is paid off the GUI
   thread on first enable().
4. **ANR on disable join.** `disable()` joined `decoderWorker_` and
   `voicePump_` on the GUI thread — if `liveScanner` took >5 s to
   unwind, Android's ANR watchdog fired and killed the app. **Fix:**
   spawn `cleanupThread_` from `disable()` so the GUI returns
   immediately; `enable()` and `~DsdFmeDecoderModule()` join
   `cleanupThread_` before touching pipeline state. `lifecycleMtx_`
   serializes enable/disable/destructor so the cleanup-thread handle is
   race-safe even from non-GUI callers (the destructor takes the mutex
   briefly to fence against concurrent enable/disable, then releases it
   before joining cleanupThread_ to avoid self-deadlock).

VFO null-check at top of constructor with actionable error message
("Pick a source on the SDR++ source dropdown and reload this module
from SYS > Modules") instead of silent dead module.

**Diagnostic:** `adb logcat -s PredatorRF` —
`[DSDFME] worker join took NNN ms` warning when join >1 s indicates a
regression in `liveScanner` exit latency.

### `flog::warn` and `long long` on the NDK

`std::chrono::milliseconds::count()` returns `long long` on the Android
NDK (r23). `flog::__toString__` template specialises for `int64_t`
(which is `long`, not `long long`, on aarch64 NDK), and falls back to
`(std::string)value` which **does not compile** (`no matching
conversion for C-style cast from 'const long long' to 'std::string'`).
At every flog call site that takes a chrono duration count, cast to
`int64_t` explicitly:

```cpp
const int64_t elapsed_ms = (int64_t)std::chrono::duration_cast<
    std::chrono::milliseconds>(now - t0).count();
flog::warn("[DSDFME] worker join took {} ms", elapsed_ms);
```

This is preferable to extending `flog.h` itself because `flog.h` is
upstream SDR++ code we want to keep diff-clean.

---

## `usbReceiver` leak in `MainActivity.onDestroy`

`registerReceiver(usbReceiver, ...)` runs in `onCreate` (~line 296) but
`onDestroy` previously only removed the thermal listener. Every
activity teardown leaked an IntentReceiver (logcat:
`Activity org.sdrpp.sdrpp.MainActivity has leaked IntentReceiver ...
MainActivityKt$usbReceiver$1`), and over a long session the leaked
receivers accumulate, slow broadcast dispatch, and contribute to ANR
pressure (Input dispatching timed out > 10s).

**Fix:** `try { unregisterReceiver(usbReceiver) } catch (e:
IllegalArgumentException) {}` in `onDestroy` — wrapped because Android
throws `IllegalArgumentException` if the receiver was never registered
(e.g. `onCreate` threw before line 296).
