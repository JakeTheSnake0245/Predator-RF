---
name: Predator spectrum gesture extension pattern
description: How to add touch gestures to the SDR++ waterfall without editing the upstream widget, and the detect-then-defer constraint it forces.
---

# Adding spectrum gestures to the Predator UI

Add spectrum/waterfall touch gestures by binding a handler to
`gui::waterfall.onInputProcess` (an `Event<WaterFall::InputHandlerArgs>` emitted
from inside `WaterFall::draw()`), NOT by editing the upstream
`WaterFall::processInputs`.

**Why:** the waterfall widget is vendored upstream (SDR++). Editing
processInputs creates merge pain and risks the shared drag/retune paths. The
onInputProcess event exists specifically as a clean pre-hook; setting
`gui::waterfall.inputHandled = true` inside the handler makes upstream skip its
own default handling for that frame.

**Detect-then-defer (the non-obvious constraint):** the handler must be a bound
static fn (`EventHandler` takes `void(*)(T,void*)`), so it canNOT see the marker
/tune/save helpers — those are all lambdas *local to* `MainWindow::draw()`
(`tunePredatorFrequency`, `routeHitToVfo`, `assignedMarkerCount`, `openPendEdit`,
`saveMissionConfig`, `savePredatorHits`, `readJsonDouble`, and the frame-local
`hits`/`targets`/`excludes` json vectors). The handler therefore only records
intent on MainWindow member flags; the mutation is applied right after
`gui::waterfall.draw()` in `draw()` where those lambdas are in scope.

**How to apply:**
- Handler geometry: `InputHandlerArgs` gives fft/waterfall rects + `lowFreq` +
  `pixelToFreqRatio`; tapped freq = `lowFreq + (mouse.x - fftRectMin.x) *
  pixelToFreqRatio`.
- Long-press idiom: `io.MouseDownDuration[0] > ~0.45s` + `GetMouseDragDelta`
  stationary, one-shot latched (reset when the finger lifts).
- Deferred `openPendEdit` callbacks run a frame LATER — never capture draw-local
  lambdas or frame-local json vectors; re-acquire configManager and edit the
  config key (e.g. `predatorHits`) directly, then let draw()'s top-of-frame
  reload pick it up.
- `ImGui::GetItemID()` is NOT in this ImGui vendor drop. For per-row hold
  latches (network tree), use an int keyed on the row's own unique id (`treeUid`).
- Out of C++ scope: DF/Map gestures are Android-native (`MapActivity.kt`); the
  Event Log is a flat filtered text list, not an ImGui table, so "customize
  columns" has no header to long-press; pinch needs real multi-touch (ImGui here
  gets touch as one mouse).
