---
name: Waterfall VFO auto-selection
description: SDR++ waterfall auto-selects the first VFO; helper/marker VFOs must be excluded or they hijack spectrum drags.
---

Rule: any programmatically-created helper VFO (Predator markers) must never
become `gui::waterfall.selectedVFO`. Use `waterfall.selectionSkipPrefix`
(set to "Predator M" at init) — `selectFirstVFO()` skips that prefix and
only sets `selectedVFOChanged` when the selection actually changes, because
`WaterFall::draw()` re-calls it every frame while selection is empty.

**Why:** With a selected VFO, FFT-area drags call `selVfo->setOffset()`
instead of tuning/panning — the spectrum appears frozen the moment a marker
VFO is created in a setup with no receiver ("Radio") VFO.

**How to apply:** When creating new helper VFO classes, give them a
skippable name prefix; when deleting VFOs, remember the destructor calls
`selectFirstVFO()` too. The per-frame guard in `MainWindow::draw()`
deselects entirely when only marker VFOs exist.

Related touch lessons: ImGui popups float over children — any custom
touch-scroll claim must require `IsWindowHovered(ChildWindows)` or it
ClearActiveID()s popup slider drags; and `lockWaterfallControls` should
include `IsPopupOpen(AnyPopupId|AnyPopupLevel)` so popup drags don't leak
into waterfall input.

Touch-scroll claim guard refinement: plain `IsWindowHovered()` returns
false whenever ANY item is active — a finger pressing a button killed all
panel scrolling. Correct guard: reject only when `ActiveIdWindow->RootWindow`
differs from this window's root (popup slider protection), and hover-check
with `ChildWindows | AllowWhenBlockedByActiveItem`.

Status bar layout: variable-width items (SDR source name badge) can push
trailing buttons off-screen; right-anchor critical buttons unconditionally
via SetCursorPosX and truncate variable labels.
