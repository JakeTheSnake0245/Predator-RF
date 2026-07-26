---
name: Sensor state must round-trip commands
description: Python sensor /v1/state must echo live sweep config; hardcoded empty mission arrays make controller UI look broken.
---
The controller's Mission tab, when peer-active, renders searchBands/targets/excludes from the peer's GET /v1/state — NOT from spectrum frames (those only feed the waterfall overlay). Any state field hardcoded empty makes accepted commands (e.g. mission.setSearchBands) look like they were never sent.

**Why:** Field report "bands missing, commands not sending" — commands were accepted and retasked the sweep, but /v1/state returned searchBands: [] so the UI showed nothing.

**How to apply:** When adding a command that mutates sensor config, make /v1/state reflect the new value in the same shape the C++ mission UI reads ({start, stop, enabled, name?}). Operator band names are session-scoped, keyed by normalized range string, and pruned on non-mission retasks so stale names never leak onto reused ranges.
