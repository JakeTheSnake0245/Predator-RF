# Fox Hunt TX tab

Local-hardware transmit for hiding a fox (practice beacon) or replaying a
recorded IQ file. This is the **only** transmit surface in Predator RF, and it
is deliberately local-only: the Kujhad HTTP, RNS `cmd.v1`, web `/api/command`,
and `predator-rfctl` control-socket `tx.*` hard-rejects are all untouched — no
remote peer can ever reach this code path.

## Legal responsibility

**The operator is solely responsible for legality of any transmission.**
Predator RF does not know your license class, your band plan, or your local
regulations. Before ARMing:

- Transmit only on frequencies you are licensed to use, at legal power.
- Station identification is the operator's legal obligation. The CW ID feature
  (periodic Morse callsign insertion) is a convenience, not a compliance
  guarantee — verify the period and callsign meet your jurisdiction's rules
  (e.g. FCC Part 97: ID every 10 minutes and at end of transmission).
- Replaying a recorded IQ file may re-transmit someone else's signal —
  in most jurisdictions this is illegal outside of your own test signals.

## Build flags

- `OPT_BUILD_FOXHUNT` — CMake option, default **ON**. All Fox Hunt code (tab
  UI, replay engine, TX drivers) is compiled out entirely when OFF.
- Forced **OFF** whenever `OPT_BACKEND_WEB=ON` — the headless `predator-rfd`
  daemon stays strictly RX-only regardless of what the caller passed.
- `build_linux.sh --no-foxhunt` — convenience switch for an RX-only GUI build.
- Android: `-DOPT_BUILD_FOXHUNT=ON` is passed in
  `android/app/build.gradle`. No new Gradle native target is needed (see
  driver design below).

## Architecture

- `core/src/predator/foxhunt/tx_driver.h` + `tx_driver.cpp` —
  `predator::foxhunt::TxDriver` abstract driver + process-wide
  `TxDriverRegistry` (the `.cpp` anchors the singleton in `sdrpp_core` so all
  shared objects resolve the same instance).
- `core/src/predator/foxhunt/replay_engine.h` — header-only
  `ReplayEngine`: loads WAV (RF64-aware) or raw `.cs16/.sc16` IQ fully into
  memory, or generates a tone / CW beacon; worker thread paces writes via
  driver backpressure.
- Drivers live inside the source modules that already link the vendor libs
  and self-register at shared-object load time:
  - `source_modules/soapy_source/src/foxhunt_tx.cpp` — SoapySDR TX
    (Linux/RPi: HackRF, Pluto via Soapy, etc.).
  - `source_modules/plutosdr_source/src/foxhunt_tx.cpp` — libiio TX
    (Android network path — the Android sdr-kit ships libiio/libad9361 but
    not SoapySDR).
  - `source_modules/hackrf_source/src/foxhunt_tx.cpp` — libhackrf TX
    (**the Android USB path**: Soapy is OFF on Android, so before this
    driver a HackRF plugged into the phone could not be selected in the
    Fox Hunt tab). Ring-buffered `hackrf_start_tx` callback; underruns pad
    silence; TX VGA 0–47 dB, RF amp forced off; Android fd re-acquired via
    `backend::getDeviceFD` at open() time (never cached — stale fds
    segfault in libusb). Device is single-user: stop the HackRF RX source
    before opening TX.
  These files are picked up by the modules' `file(GLOB src/*.cpp)` and are
  fully `#ifdef OPT_BUILD_FOXHUNT`-guarded, so no CMakeLists changes were
  needed inside the modules and no new build.gradle target exists.
- UI: `Fox Hunt TX` tab in `core/src/gui/main_window.cpp`
  (`PREDATOR_TAB_FOXHUNT`), guarded by the same define.

## Features

- **Sources:** IQ file (folder picker over `<root>/recordings` by default),
  built-in tone, or CW beacon keyed from the callsign field.
- **Playback:** once / repeat / duty cycle (on/off seconds).
- **Clipping warning:** file load reports `clipFraction` (samples with
  |I| or |Q| > 0.999); the UI warns when nonzero.
- **CW ID:** optional periodic Morse callsign insertion (period + WPM
  configurable) on top of file/tone playback.
- **Safety:**
  - **ARM switch** — TX controls are inert until armed; ARM state is
    **never persisted** (every launch starts disarmed).
  - **Dead-man timer** — hard cap on continuous session length
    (default 600 s, `foxhuntDeadManSec`, 0 = off); engine stops with state
    `STOPPED_DEADMAN`.
  - Gain is clamped to the driver-reported device range on start.
- **Android numeric IME:** frequency/duty/timer fields raise a numeric
  keyboard via `backend::setImeNumeric()` → `MainActivity.setImeNumeric()`.

## Persistence

All settings persist under `foxhunt*` keys in `config.json`
(frequency MHz, bandwidth kHz, gain dB, sample rate, repeat, duty on/off,
callsign, CW ID enable/period/WPM, dead-man seconds, source mode, folder).
Loaded with `.value()` defaults so configs from older builds load clean.
The ARM state is intentionally excluded.

## Threading contract

`enumerate()/open()/start()/stop()/close()` are UI-thread only. `write()` and
`setGain()` are called from the replay worker; drivers must make `write()`
blocking (backpressure paces the worker) and `setGain()` safe concurrently
with `write()`.

## Tests

```
g++ -std=c++17 -O2 -Icore/src tests/foxhunt_replay_engine_test.cpp -o /tmp/frt && /tmp/frt
```

45 checks: WAV/raw loaders, clipping fraction, CW generator timing, duty
cycle, dead-man, repeat semantics, error states.
