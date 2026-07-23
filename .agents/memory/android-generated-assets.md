---
name: Android generated assets dir
description: android/app/assets is a build artifact copied from root/ — never edit it directly.
---
`android/app/build.gradle` has `deleteTempAssets` (deletes `android/app/assets`) and `copyResources` (copies `../../root/` → `assets/`). Anything under `android/app/assets/` is regenerated on every build.

**Why:** An agent once implemented map JS in `android/app/assets/res/maps/index.html` — a stale old Leaflet copy — while the real, MapLibre-based source is `root/res/maps/index.html` (the file MapActivity loads via `file:///android_asset/res/maps/index.html` after the build copy). The edit would have been silently wiped.

**How to apply:** Edit map HTML/JS and any other packaged resources under `root/`, never under `android/app/assets/`. The MapLibre map uses `styleReady` + GeoJSON sources/layers (`map.getSource(...).setData`) — follow that idiom, not Leaflet.
