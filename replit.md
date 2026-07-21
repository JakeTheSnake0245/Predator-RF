# Predator RF

Predator RF is a joint sensing platform for a solo SIGINT operator using
Raspberry Pi/SDR/GPS sensors for RX-only signal logging and mapping.

**Convention:** this file is a compact keyed index. All component detail and
gotchas live in `docs/REPO_MAP.md`; deep dives live in the `docs/*.md` files
listed under each key. When adding new work: detail goes in `docs/REPO_MAP.md`
(or a topic doc), and only a one-line key entry is added here.

## Stack

- **C++ app:** CMake, Dear ImGui, OpenGL/GLES3, FFTW3, Volk; Kotlin/JNI Android wrapper.
- **Python backend:** FastAPI, asyncio.

## Architecture decisions (summary)

- Two-tier: Python backend consumes the Kujhad Fleet HTTP API from C++ nodes.
- RX-only wire posture: `tx.*` hard-rejected at Kujhad HTTP, RNS cmd.v1, web `/api/command`, and the control socket. Fox Hunt TX is local-hardware-only.
- `preview.html` is the canonical dashboard; every dashboard endpoint must exist on BOTH backends (Python :8073 and C++ predator-rfd :5555). Spectrum is C++-only.
- CoT fans out over TAK UDP/TCP and RNS simultaneously; CoT export is operator-initiated in v1.
- The Replit environment only serves the landing page + UI mockup (`server.py`); it cannot run the Android build, Python backend, or predator-rfd.

## Key index

Each key → where the detail lives. Read `docs/REPO_MAP.md` first for anything not listed.

- `REPO_MAP` → `docs/REPO_MAP.md` — full "where things live" + ALL gotchas (Android, web backend, hold system, TDOA, custody, RNS, Fox Hunt, KrakenSDR).
- `ANDROID` → `docs/android_build.md`, `docs/android_gotchas.md` — APK build; read gotchas before touching `MainActivity.kt`, `backend.cpp`, `AndroidManifest.xml`, or native decoder modules.
- `WEB_BACKEND` → `docs/linux_web_frontend.md` — headless predator-rfd, API surface, parity contract, CLI.
- `HOLD` → `docs/predator_hold.md` — multi-VFO hold + decoder auto-activation; sacred call order.
- `TDOA` → `docs/tdoa_controller.md` — C++ Controller-mode TDOA pipeline + parity stance.
- `CUSTODY` → `docs/custody_election.md` — sensor election, C++↔Python byte-parity harness.
- `STATIONARITY` → `docs/stationarity_gate.md` — TDOA fix sanity filter + motion classifier.
- `RNS` → `docs/rns_commanding.md`, `docs/rns_parity.md`, `backend/rns/README.md` — Reticulum CoT + cmd.v1 tasking.
- `KRAKEN` → `docs/krakensdr.md` — LOB decoder, triangulator, five footguns.
- `FOXHUNT` → `docs/foxhunt.md` — local TX tab + networked Remote Fox Hunt (`foxbeacon`, per-node opt-in, NOT tx.*), replay engine, drivers, OPT_BUILD_FOXHUNT flag rules.
- `COCKPIT` → `docs/remote_cockpit.md` — predator_node_source Controller/Device split, tx.* unlock model.
- `GESTURES` → `docs/REPO_MAP.md` (Diablo touch-gesture parity) — RX/analysis spectrum + network-tree gestures via `waterfall.onInputProcess` detect-then-defer; map/columns out of C++ scope.
- `REPOSITORY` → `docs/signal_repository.md` — signal store, fingerprinter, REST routes.
- `CORRELATION` → `docs/correlation_engine.md` — correlation rules, fleet state, IQ capture.
- `OPS` → `docs/OPERATOR_RUNBOOK.md`, `docs/MISSION_READY_CHECKLIST.md`, `docs/1_conops.md` — field operation.
- `COT` → `docs/ATAK_COT_FORMAT.md` — CoT XML format.
- Upstream base: [SDR++ GitHub](https://github.com/AlexandreRouma/SDRPlusPlus).

## Run & Operate

_Populate as you build_

## User preferences

- Keep `replit.md` as this compact keyed index; save all detail to `docs/REPO_MAP.md` and topic docs, never inline here.
- All keyboard-editable fields in the Android UI must stay visible above the soft keyboard: use the pendEdit popup (numeric variant for numbers) in Predator tabs, `iv()`-style scroll in modals, and `ImGui::Input*IME` wrappers / SmGui in menus (`core/src/gui/widgets/ime_scroll.h`).
