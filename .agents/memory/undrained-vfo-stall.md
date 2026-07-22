---
name: Undrained VFO stalls the DSP splitter
description: Any VFO whose output IQ stream has no consumer back-pressures the DSP splitter and stalls the whole flowgraph, including the FFT/waterfall feed.
---

Creating a VFO via `sigpath::vfoManager.createVFO` always spins up a downconverter
that writes to `vfo->output`. The DSP `routing::Splitter` writes each branch
sequentially and each `stream::swap()` blocks until that branch is read/flushed.
If ANY VFO branch is undrained, the splitter blocks and starves every other
branch — the FFT/waterfall feed stops pushing and the SDR status goes STALLED
(playing==true, no FFT push).

**Why:** Predator classify auto-marker mode creates a real `Predator M<n>` VFO
with no decoder attached, so nothing drains it → stall on marker assign,
unstall on release, re-stall on reassign. (Scan mode sidesteps this by only
tracking, never creating a real VFO.)

**How to apply:** Every marker/orphan VFO with no decoder must get a drain.
Fix pattern: attach a `dsp::sink::Null<dsp::complex_t>` per marker VFO, created
in the `onVfoCreated` handler and torn down in an `onVfoDelete` handler
(`onVfoDelete` emits BEFORE `delete vfo`, so the stream is still alive to stop
the sink; `~VFO` then calls `dspVFO->stop()`). Keyed by VFO name.
Any future feature that creates a VFO without a downstream consumer will
re-trigger this stall.

## Known instances of this class
- **Classify auto-marker** (fixed): `routeHitToVfo` creates `Predator M<n>` with
  no decoder → drained via a Null sink attached in the onVfoCreated handler.
- **Hold to decode, deferred decoders** (fixed): `HoldManager.tick` creates a
  `Predator H<id>` VFO for every enabled+in-band entry, but `HoldDecoderBinder`
  only spawns a decoder when `decoderModuleName(kind)` is non-empty. Only
  `Native_RTL433` is non-empty today; DSD-FME/ADSB/Radio_* return "" (deferred).
  Fix: the hold createCb wire-up returns false (skip VFO) when the decoder has
  no module, so no orphan VFO is created. RTL433 unaffected.
- **DSD-FME manual/legacy load** (NOT yet fixed): the dsdfme_decoder module
  creates its own VFO in its CONSTRUCTOR while enabled_ defaults false and the
  draining pipeline only starts on enable(). So loading the module but not
  pressing Start leaves an undrained VFO → stall. RTL433 avoids this by creating
  its VFO in startPipeline (bound-mode aware), not the ctor. Refactoring DSD-FME
  to match (lazy VFO + bound mode) is the prerequisite for real hold-to-decode
  of DSD-FME.
