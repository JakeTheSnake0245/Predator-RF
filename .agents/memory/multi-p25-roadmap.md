---
name: Multi-P25 decode roadmap (future)
description: User-requested future feature — 5-8 concurrent P25 feeds; demod capacity outranks spectrum GUI.
---

User wants 5-8 concurrent P25 feeds (future, not now). Required work:
- Make dsdfme_decoder multi-instance (currently 1-instance limit in SDRPP_MOD_INFO).
- Refactor DSD-FME to lazy VFO creation on start (like rtl433 startPipeline), killing the ctor-undrained-VFO stall footgun.
- Wire Native_DSDFME_P25 into decoderModuleName in hold_decoder_binder.h so hold auto-spawns it.
- HoldManager already allows 8 entries; real ceiling is CPU on Pi/Android — needs device testing.

**Priority (user-stated):** running active demods > spectrum GUI. Headless/no-GUI
operation is acceptable if the GUI is unstable or costs decode capacity.
