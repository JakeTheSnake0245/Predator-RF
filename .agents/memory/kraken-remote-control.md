---
name: KrakenSDR remote-control quirks
description: Field-verified unit and HTTP quirks of the krakensdr_doa miniserve remote-control interface (:8081)
---

# KrakenSDR remote control (:8081 miniserve) — field-verified quirks

- **`center_freq` in settings.json is MHz** (e.g. `97.9`), while `vfo_freq` array entries are **Hz**. Writing Hz into `center_freq` silently mis-tunes and the readback never matches → tune always "fails".
- **miniserve answers a successful `POST /upload?path=/` with `303 See Other`**, not 201/2xx. A 2xx-only success check reports "POST failed" on every successful upload.
- Readback match tolerance should be ~100 Hz (MHz floats round-trip through JSON).
- **Why:** both bugs were only caught by running curl on the operator's actual Pi; the docs/assumptions said Hz + 201. Trust field captures over API guesses.
- **How to apply:** any code touching the DoA settings blob must convert MHz↔Hz at the boundary and accept 303 as upload success.
