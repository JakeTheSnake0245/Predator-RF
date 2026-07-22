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

## Second, distinct stall cause: pathological RxVFO filter tap count
A VFO can stall the splitter EVEN WHEN its output IS drained. `RxVFO` builds a
low-pass via `taps::lowPass`, and `estimateTapCount() = 3.8 * sampleRate /
transWidth`, where `transWidth = (bandwidth/2) * 0.1 = bandwidth * 0.05`.
Creating a narrow-bandwidth VFO at the FULL SDR sample rate (multi-MHz) yields
tens of thousands to MILLIONS of taps (e.g. 200 Hz marker @ 8 MHz ≈ 3M taps),
which the downconverter can never compute in real time → it permanently blocks
the splitter → spectrum STALLED regardless of any Null-sink drain. The giant
tap allocation can also corrupt the heap / crash the render loop.
**Why it looked like the drain "didn't work":** the marker Null-sink drain was
correct, but the true bottleneck was RxVFO compute, not an unread output.
**How to apply:** never create a VFO at full Fs with a narrow bandwidth. Anchor
the channel sample rate to a small multiple of the bandwidth
(`sampleRate = max(bandwidth*2, floor)`); tap count then stays ~constant (~150)
because Fs and transWidth both scale with bandwidth. Marker VFOs in
`routeHitToVfo` use this. IQ recording still works (VFO kept intact).

## Known instances of this class
- **Classify/manual marker stall** (REAL fix): `routeHitToVfo` created
  `Predator M<n>` at full SDR Fs → multi-thousand/million-tap FIR → stall that
  the Null-sink drain could NOT resolve. Fixed by setting the marker channel
  rate to `max(bandwidth*2, 2000)`. The Null sink drain remains as defense.
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
