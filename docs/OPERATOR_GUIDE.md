# Predator RF — Operator Guide

**Pick this up cold. Go from zero to a working RF picture in under 30 minutes.**

This is the single document you need. It assumes nothing — no SDR
experience, no Linux, no scripting. If you've used a smartphone and
plugged in a USB stick, you can run Predator RF.

---

## 1. What this is (in one paragraph)

Predator RF is a phone-first software-defined-radio cockpit. You plug
a USB SDR into an Android phone, point an antenna at the world, and
the app shows you what's transmitting in real time — frequency,
power, location (when GPS is up), and whether you've seen it before.
You can mark hits, record a "what's normally here" baseline so the
next sweep flags only what's *new*, and — optionally — link to other
phones / Raspberry Pi sensors over a private network to cover a
larger area as a small team or solo operator with distributed nodes.
There is **no transmit**. The build is RX-only by design.

---

## 2. The minimum kit (you need ALL of these to do anything)

| Item | Why |
|---|---|
| Android phone, Android 10 (API 28) or newer | Runs the app |
| **USB-C OTG cable or adapter** (USB-C → USB-A female) | Connects the SDR |
| **A USB SDR** — RTL-SDR Blog v4 ($40) is the cheapest, HackRF One ($340) is the upgrade | The actual radio |
| **An antenna** — even the rubber-duck that came with the SDR works | Without one you receive nothing |
| **The Predator RF APK** | The app itself — built from the GitHub repo or supplied by your team |

That's it. No internet required after install. No subscription. No
account.

A full tiered bill of materials (cheap → fleet) is in **§ 11**.

---

## 3. First-time install (sideload)

Predator RF is **not on the Play Store**. You sideload the APK.

1. On the phone: **Settings → About phone → tap "Build number" 7 times** until it says "Developer mode is ON".
2. **Settings → Developer options →** turn on **Install via USB** *and* **USB debugging**.
3. Get the APK onto the phone (USB cable from a computer, Google Drive, email — anything).
4. On the phone, open the file (Files app → Downloads → tap `app-debug.apk`).
5. When Android asks "Install unknown apps?" → **Allow** for whatever app you opened the APK from → **Install**.
6. Open **Predator RF**.

When the app first launches it will ask for these permissions —
**grant all of them**:

- **Location** ("While using the app") → for the GPS-stamped hit map
- **Storage / Files** → for saving baselines and exporting CSVs
- **USB device access** → pops up the first time you plug in your SDR; tap **Always allow for this device**

If you skip any of these the app still runs, but the matching feature
goes dead-quiet (no map fix, no exports, no SDR).

---

## 4. Plug in the radio and start receiving

1. Connect your SDR to the phone with the USB-C OTG adapter. Antenna goes on the SDR.
2. The first plug-in pops a system dialog: **"Open Predator RF when this USB device is connected?"** — check the box and tap **OK**.
3. In the app, on the left rail tap the **source dropdown** (top-left, says `None` until a radio is selected) → pick `RTL-SDR`, `Airspy`, `HackRF`, etc. matching what you plugged in.
4. Tap **▶ Start Listening** (big button under the source dropdown).
5. The waterfall on the right starts scrolling. **You're live.**

If the waterfall stays black, see § 10 (Troubleshooting).

---

## 5. The eight tabs (left-side rail, top to bottom)

The whole app is organized into eight tabs. They're labeled by their
3- or 4-letter code on the rail. Tap to switch.

| Code | Name | What it's for |
|---|---|---|
| **SPEC** | Spectrum | Live waterfall + tuner. Where you actually look at signals. |
| **HITS** | Hits & Events | Every signal the app has noticed, plus the running event log. |
| **NET** | Network | Catalog of known networks / TGs / aliases — your "rolodex of emitters." |
| **MAP** | Map | GPS-stamped hits on a map view. |
| **MIS** | Mission Config | Mission mode (Manual / Classify / Scan / QuickScan), search bands, targets, excludes, dwell timing. |
| **KUJ** | Kujhad Fleet | Link this phone to other Predator RF phones / Raspberry Pi sensors over a network. |
| **SYS** | System | App settings — modules, themes, decoder bridges, ATAK/CoT, baseline comparison. |
| **BASE** | Baseline | Record what the noise floor / normal traffic looks like, save it, then suppress it next time so only NEW signals fire as hits. |

---

## 6. The five workflows you'll actually use

### 6.1 Just look around (passive recon)

1. **SPEC** tab → set the source → ▶ Start Listening.
2. Drag the waterfall left/right to retune. Pinch to zoom the view bandwidth.
3. Tap a peak to drop a marker on it.
4. To name what you just found: tap the marker → **Assign Marker** → tap **"Tap to edit"** in the popup → type a label.

### 6.2 Mark a hit and send it to your team

1. With a marker on a signal, tap **Assign Marker** if it isn't already.
2. The marker becomes a **hit** — visible on the **HITS** tab.
3. To send it to ATAK / TAK: **SYS** tab → **ATAK / CoT** section → enter your TAK server IP + port → **Enable CoT**. The hit appears on the ATAK map as a friendly contact at your phone's current GPS location plus a chat message with the freq + power.

### 6.3 Record a baseline so you only see NEW signals

The first time you set up in a new area, run a baseline so the app
learns what's "normally here" (paging towers, nearby commercial
radios, broadcast leakage). Then any signal NOT in the baseline is
flagged as new.

1. **BASE** tab → **Frequency Ranges** → set Range Name, Start Hz, Stop Hz → **+ Add Range**. Repeat for each band you care about. (Tap **"From Current View"** to grab whatever the waterfall is showing right now.)
2. **Recording** section → leave filename blank for auto-naming, OR tap to set a custom one → tap **▶ START RECORDING** (green).
3. Let it run for at least 5 minutes (longer = better — 30 min for a thorough sweep).
4. **■ STOP RECORDING** → **💾 Save to File**.
5. **SYS** tab → **Baseline Comparison** → load the file you just saved → enable **Scan against baseline**. Set a threshold (default 6 dB). Now only signals exceeding the baseline by that margin become hits.

### 6.4 Run an automated scan across multiple bands

1. **MIS** tab → **Mission Mode** → **Scan**.
2. **Search Bands** section → add the bands you want swept (Name + Start Hz + Stop Hz). Tap **+ Add Band**.
3. **Targets** section → optional — frequencies you specifically want to flag as priority.
4. **Excludes** section → optional — frequencies to ignore (your own gear, known broadcast carriers).
5. **Dwell** + **QuickScan Delay** + **Duration** — defaults are sane. Increase dwell if you're chasing weak intermittent signals.
6. Back to **SPEC** → **▶ Start Listening**. The app will now sweep your bands automatically and drop hits on the **HITS** tab as it finds them.

### 6.5 Link a second phone or RPi sensor

This is the Kujhad Fleet feature — distributed sensors with one
operator screen.

**Make this phone discoverable:**

1. **KUJ** tab → **Listen** section → set Listen port (default 8080), Device name, API key (use a long random string — anything works, just make it match on both sides).
2. Toggle **Listen** on. The phone now serves its state to peers.

**Add a peer (another phone or RPi running the same protocol):**

1. **KUJ** tab → **Add Peer** → Name (anything memorable), Host (the peer's IP), Port (their listen port), API Key (must match theirs), tap **Add peer**.
2. The peer appears in the peer list. Toggle the **Mirror peer spectrum** switch to view *their* waterfall on *your* screen while still controlling your local SDR.

For the full Kujhad architecture (TLS, ZeroTier overlays, RPi node
setup) see `docs/OPERATOR_RUNBOOK.md` — it covers the Linux / Python
backend side of this same fleet.

---

## 7. Editing fields on a touchscreen

Every editable field (peer host, port, frequency, range name, target,
filename, etc.) works the same way:

1. Tap the field. A popup opens at the **top of the screen**, above the keyboard.
2. The keyboard slides up automatically.
3. Type. You can see what you're typing — the popup is intentionally pinned high so the keyboard never covers it.
4. Tap **OK** (or hit Enter) to commit. **Cancel** to discard.

If a field doesn't open a popup when tapped, that's a bug — restart
the app once and try again. If it persists, file it.

---

## 8. The orange / yellow / red status bar at the top

| Color | Meaning |
|---|---|
| Green / dim grey | Nominal |
| Orange | Thermal warning — the SoC is heating up; expect frame drops |
| Red | Severe — back off (lower sample rate, close other apps, get the phone out of direct sun) |

The phone will throttle itself before it cooks, but reduce load
proactively when you see orange.

---

## 9. Field-day checklist (print this)

**Before you leave:**

- [ ] Phone charged + power bank packed
- [ ] APK version verified (open app → SYS → check version against your team's current build)
- [ ] SDR + antenna + USB-C OTG cable in the bag
- [ ] At least one **baseline file** for the area you're going to (record it ahead if possible — see § 6.3)
- [ ] If using ATAK: TAK server reachable + credentials entered + a successful test beacon yesterday
- [ ] If using Kujhad fleet: peers added, all on the same overlay network (ZeroTier / Tailscale / LAN), API keys match

**On site, bring-up:**

1. Antenna up first — give it sky if you can.
2. Plug SDR into phone → confirm USB permission dialog accepts.
3. Open Predator RF → SPEC → pick source → ▶ Start.
4. Verify waterfall is moving + GPS lock indicator is green (top status bar).
5. **MIS** → set mission mode for the day's job.
6. If using baseline: **SYS** → Baseline Comparison → load file → enable.
7. If using Kujhad: **KUJ** → toggle Listen → confirm each peer shows green/connected.
8. If using ATAK: **SYS** → ATAK/CoT → enable. Verify your icon appears on the TAK map within 30 s.

**During mission (every 30 min):**

- Glance at the thermal indicator
- Glance at GPS lock
- Glance at hit count on **HITS** — climbing = system working
- Phone battery > 20% — swap to power bank before you hit 10%

**End of mission:**

1. **MIS** → mode back to Manual.
2. **HITS** → export hits to CSV (button at the top of the tab).
3. **BASE** → if you recorded a fresh baseline, save it before powering down.
4. **▶ Stop Listening** → close app → unplug SDR.

---

## 10. Troubleshooting

| Symptom | Most likely cause | Fix |
|---|---|---|
| Waterfall is black after Start Listening | SDR not selected as source, or USB permission was denied | Source dropdown → re-pick. Unplug + replug SDR → tap "Always allow" on the dialog. |
| App says "device busy" | Another app (or a previous Predator RF session) has the SDR | Force-stop Predator RF in Settings → Apps; unplug + replug |
| Touch feels unresponsive / wrong widget fires | Old build — this was fixed in the touch-passthrough + ID-stack patches | Update the APK to the current build |
| Soft keyboard covers the input field | Old build — fixed by IME-inset clamp | Update APK |
| GPS never locks | Phone needs sky view; indoor / urban canyon = no fix | Step outside; wait 60 s |
| Kujhad peer shows red / disconnected | Network not reachable, or API keys don't match | Ping the peer from a terminal app; confirm both phones have the same API key character-for-character |
| ATAK marker never appears | Wrong server IP/port, or the TAK server is filtering by UID prefix | Check TAK server logs; verify CoT enabled and not in dry-run |
| Phone hot, framerate drops | Thermal throttling | Get out of the sun; reduce sample rate (SPEC → source settings) |
| App crashes on startup after install | APK was sideloaded over a different signing key | Uninstall the old version completely (Settings → Apps → Predator RF → Uninstall), then reinstall |

---

## 11. Bill of materials — pick your tier

Prices are USD, May 2026, ballpark.

### Tier 0 — Bare minimum (~$50)

You probably already own most of this.

| Item | Approx | Notes |
|---|---|---|
| Android phone you already own | $0 | Anything Android 10+ |
| RTL-SDR Blog v4 | $40 | Cheapest viable SDR. 500 kHz – 1.75 GHz. |
| USB-C OTG adapter | $8 | USB-C male → USB-A female |
| Rubber-duck antenna | included | Comes with the v4 |
| **Total** | **~$48** | |

What you can do: receive everything from FM broadcast through ~1.7 GHz, mark hits, record baselines, run scans. No HF, no transmitting. No fleet.

### Tier 1 — Solo field operator (~$300-450)

Adds wider coverage, better front-end, and external GPS so the map works indoors-ish.

| Item | Approx | Notes |
|---|---|---|
| Tier 0 kit | $48 | |
| Airspy Mini *or* HackRF One | $130 / $340 | Mini = better dynamic range up to 1.7 GHz; HackRF = 1 MHz – 6 GHz, the workhorse |
| Telescoping whip antenna (Diamond RH-77CA) | $50 | Vastly better than rubber-duck |
| External USB GPS (BU-353N5 or similar) | $35 | Works through the same OTG hub; faster lock than phone GPS |
| Powered USB-C OTG hub | $25 | So the phone doesn't drain its battery powering the SDR |
| 20,000 mAh USB-C PD power bank | $40 | A full mission day |
| **Total** | **~$330** (Airspy) / **~$540** (HackRF) | |

What you can do: the above plus HF (HackRF), full ATAK integration, all-day battery life, GPS that locks fast.

### Tier 2 — Solo TOC with one remote sensor (~$1,400-1,800)

Adds a Raspberry Pi sensor node running the Path 1 Python backend so
you can drop a sensor somewhere and walk the perimeter while it
collects.

| Item | Approx | Notes |
|---|---|---|
| Tier 1 kit (HackRF flavor) | $540 | |
| Raspberry Pi 5 8GB + case + active cooler + PSU | $130 | The sensor node |
| 256 GB A2 microSD | $25 | |
| Second RTL-SDR Blog v4 (for the Pi) | $40 | |
| GPS HAT (Adafruit Ultimate GPS HAT, or USB GPS) | $45 | Sensor node needs its own fix |
| Linux laptop (anything you already own) | $0–$500 | Operator workstation runs the same Python backend OR you skip it and run from the phone |
| ZeroTier or Tailscale (overlay network) | $0 | Free tier covers a small fleet |
| Outdoor antenna mount + 25 ft LMR-400 + N-to-SMA pigtails | $80 | Get the antenna off the truck |
| Discone or wideband vertical (Diamond D130J) | $130 | The actual perimeter antenna |
| Pelican 1450 case | $130 | So the Pi survives the field |
| **Total** | **~$1,400** + laptop | |

What you can do: above plus distributed collection, long-duration unattended overwatch, mission ledger, post-mission CSV export & after-action.

### Tier 3 — Small team / multi-node fleet (~$5,000+)

Repeat Tier 2's RPi sensor as many times as you have positions to
cover, plus higher-end antennas + comms gear.

| Item | Approx | Notes |
|---|---|---|
| Tier 2 kit | $1,400 | |
| 3× additional RPi sensor nodes (each tier-2 RPi+SDR+GPS+case) | $1,500 | |
| Cellular hotspot (Netgear M6 Pro) + month of data | $300 | If LAN/overlay won't cover the area |
| Or: small mesh radio link (goTenna Pro X2, RAJANT, etc.) | $500–$5,000 | When cell is denied |
| Commercial-grade band-specific antennas (VHF dipole, UHF Yagi for direction-finding) | $400 | When you know what band you live in |
| Tactical mast (10–25 ft fiberglass, military surplus) | $150 | Get the antenna into the air |
| Ruggedized Pelican / Storm cases × N | $400 | One per node |
| Operator laptop (Lenovo ThinkPad / Dell rugged) | $500–$2,000 | If your phone-only setup isn't enough |
| **Total** | **~$5,000–$10,000** | Scales with fleet size |

What you can do: above plus full-perimeter overwatch from a single
operator screen, ATAK integration to higher-echelon TOC, after-action
ledger across all nodes, and a credible answer to "where else is that
emitter being heard from."

---

## 12. Where to go next

- **`docs/OPERATOR_RUNBOOK.md`** — the Linux Python backend (Path 1) deep-dive. Read this when you set up RPi sensor nodes or the operator workstation.
- **`docs/MISSION_READY_CHECKLIST.md`** — the longer pre-mission checklist with the Python-side commands. Print before a real op.
- **`docs/SIDELOAD_README.md`** — step-by-step APK install from a Windows box.
- **`docs/ATAK_COT_FORMAT.md`** — the exact CoT XML this app emits (for TAK plugin authors).
- **`docs/1_conops.md`** — the doctrine document. Read this if you're deciding whether Predator RF fits a problem at all.

If something in this guide is wrong or out of date, fix it and commit.
This document is the contract with the operator.
