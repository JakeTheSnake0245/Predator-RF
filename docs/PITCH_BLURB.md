# Predator RF — copy-paste pitch + tiered BOM

Drop the section you want straight into a text message, email, or Signal.
Everything below is plain text — no markdown formatting that'll break in SMS.

---

## SHORT (under 160 chars — fits one SMS)

```
Predator RF: phone-based SDR cockpit for solo SIGINT. Plug a USB radio into Android, see the spectrum, mark hits, sync to ATAK. RX-only. Starts at $50.
```

---

## MEDIUM (3-4 sentence elevator pitch)

```
Predator RF is a phone-first SDR cockpit built for one operator running RF collection in the field.
Plug a USB SDR into your Android phone, get a live waterfall, automated scans across bands you define,
GPS-stamped hits, baseline comparison so you only see what's new, and optional Kujhad fleet linking
so a Raspberry Pi sensor in your truck can feed the same screen. ATAK/TAK CoT export built in.
RX-only by design. Sideloads as an APK. Costs as little as $50 for the radio + cable + phone you already own.
```

---

## LONG (full pitch, drop in an email or Signal note)

```
Predator RF — solo-operator SIGINT cockpit for Android.

What it does:
- Live spectrum + waterfall on your phone from any USB SDR (RTL-SDR, Airspy, HackRF)
- Automated band scans with target / exclude lists, dwell control, mission modes
- Baseline recording — sweep an area once, then only NEW emitters generate hits
- GPS-stamped hits with on-board map and CSV export for after-action
- Optional Kujhad fleet linking — phones and Raspberry Pi sensors share one operator screen
- ATAK / TAK CoT integration — hits show up on the TAK map as chat alerts
- Decoder bridges: P25 P1+P2 (DSD-FME), RTL433, POCSAG/FLEX, ADS-B, AIS
- RX-only build, no transmit surfaces

What you need (minimum): an Android phone, an RTL-SDR Blog v4 ($40), a USB-C OTG cable ($8),
and an antenna. Sideload the APK and you're live in 30 minutes.

Built on the SDR++ core. Open codebase. No subscription, no account, no cloud dependency.
Operator guide and BOM at: docs/OPERATOR_GUIDE.md
```

---

## TIERED BILL OF MATERIALS (copy-paste version)

```
PREDATOR RF — TIERED BOM (USD, ballpark)

TIER 0 — Bare minimum (~$50)
  Phone you already own ......................... $0
  RTL-SDR Blog v4 ............................... $40
  USB-C OTG adapter ............................. $8
  Rubber-duck antenna (included with SDR) ....... $0
  ----------------------------------------------------
  TOTAL                                          ~$48
  Get: live spectrum, hits, baseline, scans up to 1.7 GHz.
  Skip: HF, transmit (we don't transmit anyway), distributed sensors.

TIER 1 — Solo field operator (~$330-540)
  Tier 0 kit .................................... $48
  Airspy Mini ($130) OR HackRF One ($340) ....... $130-340
  Diamond RH-77CA telescoping whip .............. $50
  External USB GPS (BU-353N5) ................... $35
  Powered USB-C OTG hub ......................... $25
  20,000 mAh USB-C PD power bank ................ $40
  ----------------------------------------------------
  TOTAL                                          ~$330 (Airspy) / $540 (HackRF)
  Get: HF coverage (HackRF), all-day battery, fast GPS lock, ATAK ready.

TIER 2 — Solo TOC + one remote sensor (~$1,400)
  Tier 1 kit (HackRF flavor) .................... $540
  Raspberry Pi 5 8GB + case + cooler + PSU ...... $130
  256 GB A2 microSD ............................. $25
  Second RTL-SDR Blog v4 ........................ $40
  GPS HAT or USB GPS for the Pi ................. $45
  Outdoor antenna + LMR-400 + pigtails .......... $80
  Diamond D130J discone ......................... $130
  Pelican 1450 case ............................. $130
  ZeroTier / Tailscale overlay (free tier) ...... $0
  ----------------------------------------------------
  TOTAL                                          ~$1,400
  Get: drop-and-walk sensor node, distributed collection, mission ledger,
  after-action exports.

TIER 3 — Small team / multi-node fleet ($5,000-10,000+)
  Tier 2 kit .................................... $1,400
  3 more RPi sensor nodes (each ~$500) .......... $1,500
  Cellular hotspot + data plan .................. $300
  Or mesh radio link (goTenna / RAJANT) ......... $500-5,000
  Commercial directional antennas (Yagi, dipoles) $400
  10-25 ft fiberglass mast ...................... $150
  Pelican / Storm cases per node ................ $400
  Operator laptop (rugged ThinkPad / Dell) ...... $500-2,000
  ----------------------------------------------------
  TOTAL                                          ~$5,000-10,000+
  Get: full-perimeter overwatch from one operator screen, ATAK to higher
  echelon, ledger across the fleet, real DF capability.
```

---

## ONE-LINER FOR EACH TIER (if you only have time for one)

```
TIER 0 ($50): A USB radio, your phone, a cable. Live spectrum, hits, scans.
TIER 1 ($330+): Add a real antenna, GPS, power bank. All-day field ready.
TIER 2 ($1,400): Add a Raspberry Pi sensor node. Drop and walk.
TIER 3 ($5,000+): Multi-node fleet, ATAK, cell/mesh comms. Small-team TOC.
```
