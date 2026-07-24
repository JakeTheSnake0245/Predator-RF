---
name: Predator events per-frame copy persistence
description: Why event-log rows can "twitch"/vanish and fleet-LOB wedges disappear — the events array is a per-frame config copy that must be saved same-frame by every ingest path.
---

# Predator events per-frame copy persistence

In `MainWindow::draw()`, `events` is a LOCAL copy of
`conf["predatorEvents"]`, reloaded from ConfigManager at the top of every
frame. Any inserted row that is not persisted with
`savePredatorEvents(events)` **in the same frame** evaporates on the next
frame's reload.

**Why:** the Kujhad peer-drain path inserted rows without saving — the
event log flashed rows for one frame ("twitching") and the fleet-LOB wedge
aggregator (which runs earlier in the frame) always saw zero KRAKEN_LOB
rows, so no wedges reached the map. A debounced save is equally broken:
a skipped save = rows permanently lost, not deferred.

**How to apply:** every ingest path that does `events.insert(...)` must
call `savePredatorEvents(events)` unconditionally in the same frame.
This is cheap — `release(true)` only marks the in-memory tree dirty; disk
I/O is batched by the ConfigManager autosave thread. Never reintroduce a
debounced/skippable event save. Also stamp `rxClock` (local receive time)
at insert so LOB freshness survives the save/reload round-trip.

## Map liveness: rendering pauses while the map screen is open

On Android, the map is a separate activity — bringing it up fires
APP_CMD_TERM_WINDOW and pauses the whole draw() loop, freezing every
map-facing push (a "snapshot" map). Fix: all peer draining + map bridges
(event markers, fleet LOB wedges, Kraken tune drain/status) live in
`MainWindow::backgroundMapTick()` (self-throttled 1 Hz), called from the
Android render loop unconditionally (even while paused) AND at the TOP of
draw() — it must run BEFORE draw()'s per-frame `events` snapshot, or
later same-frame saves of the stale copy erase the tick's inserts
(reviewer-caught regression of the evaporating-rows class). New map data
work belongs in the tick, not in draw().

Diagnostic that found it: `[FleetLOB] diag` heartbeat logging
events/kraken/noRaw/badPos/stale counters every 5 s — `events=0` while
peer drains logged steadily proved the two code paths saw different data.
