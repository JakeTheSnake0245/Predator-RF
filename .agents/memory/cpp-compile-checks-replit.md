---
name: C++ compile checks in the Replit env
description: How to syntax-check the C++ codebase here despite missing volk/fftw3, and known pre-existing stub-only errors in the web backend TU.
---

# C++ compile checks in the Replit env

The Replit container has g++ but NOT volk or fftw3, so any TU that pulls in
`signal_path/signal_path.h` won't compile out of the box.

**How to apply:**
- Header-only predator headers (`core/src/predator/*.h`) syntax-check cleanly
  with `g++ -std=c++17 -fsyntax-only -Icore/src <tu>.cpp`.
- For `core/backends/web/backend.cpp`, stub the missing system headers into a
  temp include dir: a `volk/volk.h` with `lv_32fc_t = std::complex<float>`,
  `lv_cmake`, and variadic-template no-op stubs for every `volk_*` function
  used under `core/src/dsp` (grep `volk_[a-z0-9_]+\(` to generate them), plus
  a minimal `fftw3.h`. Then
  `g++ -fsyntax-only -I/tmp/stubinc -Icore/src -Icore/backends/web backend.cpp`.
- Under this stub setup, backend.cpp has **4 pre-existing errors** (atomic→json
  assignments and `SourceManager::setFrequency`) that do NOT occur in the real
  predator-rfd build. Compare error counts against `git show HEAD:` of the file
  to prove your edit adds zero new errors instead of chasing them.

**Why:** verifying C++ edits here is otherwise guesswork; this gives a
deterministic zero-new-errors check without the real SDR toolchain.
