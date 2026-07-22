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
