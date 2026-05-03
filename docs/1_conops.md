# Predator-SDR — CONOPS

**Document 1 of 5** in the joint-sensing platform plan. This is the
doctrine document. Every architectural and engineering decision in the
remaining four documents is in service of what this one describes.

If a proposed feature does not serve a vignette below, it is out of
scope until further notice.

---

## 1. Purpose

Predator-SDR is a **joint sensing platform** that lets a small number
of operators (often one) drive collection, exploitation, and
prosecution of SIGINT-derived effects — fires and maneuver — for a
supported commander, with a deliberately small logistical and manpower
footprint.

It is not an SDR application that happens to be networked. It is a
networked sensing system in which the SDR is one of several roles a
node can play.

The headline metric we optimize for is:

> **Operator-hours per actionable, time-sensitive SIGINT report
> delivered to the commander or fires/maneuver pipeline.**

Lower is better. Every architectural choice — Kujhad HTTP wire
contract, role-composable nodes, priority-weighted retention,
deferred in-app PKI, manual selective exfil — is justified by its
impact on that ratio.

---

## 1.5 Implementation Status (snapshot)

This is what already exists in the repo as of this revision. Use it
as the baseline; do not re-plan around features that ship.

**C++ Android (in `core/`, `decoder_modules/`, `android/`)**
- Predator RF rebrand, spectrum-first shell, mission tabs, hits/events
  workflow, baseline tab + scan-against-baseline (v1.3.0)
- Kujhad Fleet console (7th tab) — hub-and-spoke peer linking with TLS
- ATAK CoT export (v1.2.0)
- Live spectrum mirroring + hit/marker overlays between fleet peers
- Native RTL433 ingestion (`predator::Rtl433Ingester`)
- DSD-FME P25 decoder bridge with always-on DSP chain (freeze fix)
- Decoder Bridges shell (P25 P1+P2, RTL433, POCSAG/FLEX, ADS-B, AIS)

**Python backend (in `backend/`, ~3.6k LOC)**
- `models/`: `RFEvent`, `EmitterTrack`, `SensorNodeTrust`
- `fusion/`: `HardwareAwareAssociator`, `TrackManager`,
  `ConfidenceEngine`, `TDOACoordinator`
- `intelligence/`: 6-method `AnomalyDetector`, `RFBaseline`
  (24 h learning), `DecisionEngine`
- `sensor/`: `HardwareAdaptiveDSP`, RTL/HackRF/Soapy interfaces,
  `HardwareCapabilities` registry, calibration scaffolding
- `coordination/`: `KujhadClient` async fleet poller
- `api/`: FastAPI server with `/tracks`, `/nodes`, `/events`,
  `/assessments`, `/health`
- `main.py`: orchestrator wiring it all together

**Build / deploy**
- `build_linux.sh` — Ubuntu/Debian/Fedora/Arch turnkey installer
- `Dockerfile.backend` + `docker-compose.yml` (backend + Postgres + InfluxDB)

**Known gaps to fill (candidates for next code commits)**
- `decoder_registry` / `detector_registry` are abstract bases only;
  no concrete decoder/detector implementations registered yet
- No end-to-end smoke test of the backend pipeline
- Backend has not been booted against a real or mocked Kujhad node
- C++ side does not yet emit `/v1/events` in the schema the Python
  `KujhadClient` expects (needs verification)
- No `replit.md` section yet describing path-1 architecture for
  future agents
- Calibration module is scaffolded but not exercised
- Multi-baseline support on the Python side (matches the C++ Baseline
  tab v1.3.0 work)

---

## 2. Headline Vignette: One Operator, Field of Sensors

> **One SIGINT operator with a phone, a couple dozen Raspberry Pis
> with SDRs and GPS dongles, drives the collection and prosecution of
> SIGINT-based fires and maneuver for the commander.**

Concrete picture:

- The operator carries a Samsung S22 (or similar) running the Predator
  app in the OPERATOR role. The S22 may or may not have an RTL-SDR /
  HackRF attached over USB-OTG; the operator's value to the network
  does not depend on whether it does.
- 8–24 Pi-class sensors are emplaced across the AO. Each carries an
  RTL-SDR or HackRF, a GPS dongle, and a Predator agent in the
  SENSOR role (one or two also carry the RELAY role).
- Sensors come up, find each other and the operator over whatever
  transports are available (LAN, Wi-Fi, cell, LoRa/RNS), and begin
  publishing classified, position-tagged detections.
- The operator sees the unified picture on their phone: a map with
  emitter markers, a tactical network/topology tree, a hits timeline,
  and a tasking surface to push the field as the situation develops.
- When the operator identifies a marker or out-of-baseline hit worth
  pushing up, they select it and exfil it to higher — typically a CoT
  message into a TAK federate, or a CoT file drop for offline higher.
- Higher echelon receives the SIGINT report in a format their existing
  pipeline (TAK, fires-cell tools) already understands, with
  geolocation, classification, time, and provenance attached.

This vignette is the design center. Everything in the remaining
documents either supports it directly or supports a closely related
variant in section 3.

---

## 3. Supporting Vignettes

These are the variants we explicitly support. Each is a real
deployment we expect customers to run.

### 3.1 Lone Wolf

One operator, one phone, one SDR. No remote sensors, no relays, no
network. The phone runs OPERATOR + SENSOR roles in-process.

- All decoding happens on the phone.
- All hits are stored locally.
- Selective exfil goes to a CoT file written to local storage, to be
  picked up later when comms are available.
- The Predator agent still runs, but with no peers — it just enables
  later "join a network" without restarting the app.

This vignette validates that the platform never penalizes the lone
operator for the multi-node features.

### 3.2 SIGINT Operator + Sensor Field (Headline)

See section 2.

### 3.3 Operator-Only Phone, No Local SDR

Same as the headline, except the operator's phone has no SDR
attached. The phone runs OPERATOR (and optionally RELAY) only. All
sensing comes from the Pi field.

This is the **manipulation-layer-without-collection** mode. It is
critical because:

- It frees the operator's phone from depending on USB-OTG hardware
  that may brown out under load on hot days.
- It lets the operator use a smaller / cheaper / more rugged device
  (e.g., an A-series Galaxy) without losing capability.
- It cleanly demonstrates that the OPERATOR UI is a real layer, not a
  thin wrapper over local DSP.

### 3.4 Vehicle TOC

One operator phone, one vehicle-mounted laptop in the RELAY role
(plus optional SENSOR role with its own SDR), and a sensor field. The
laptop is the persistent storage point and the gateway between the
field's tactical network (LoRa/RNS) and the vehicle's higher-bandwidth
backhaul (cell / starlink / FOB Wi-Fi).

- Operator can dismount and the vehicle laptop keeps the federation
  alive.
- Selective exfil goes through the laptop's higher-bandwidth path when
  available.
- The laptop is the CoT bridge to the broader TAK ecosystem.

### 3.5 Federated Team-of-Teams

Multiple sensor fields, each with its own operator and optional
vehicle TOC, federate so that operator A can see emitters that only
operator B's field has line-of-sight to.

- Each operator's local view is authoritative for their AO; cross-AO
  data is presented as such.
- RELAY-to-RELAY federation handles the cross-traffic; OPERATORs
  subscribe to whichever federates they have authority for.
- This is where multi-server-client patterns from TAK become directly
  relevant.

We design for this vignette but treat it as a v2 deployment target.
The MVP must work cleanly for vignettes 3.1–3.4 first.

---

## 4. Node Roles

Every Predator-capable device carries one or more of three roles.
Roles are **build-time feature flags** in the C++ core and the Rust
agent — a node's binary is compiled with exactly the roles it needs.

| Role         | Responsibility                                                              |
|--------------|------------------------------------------------------------------------------|
| **OPERATOR** | UI, map, network/topology tree, tasking, selective exfil, CoT export, audit |
| **SENSOR**   | DSP graph, decoder modules, position-tagged event classification + publish  |
| **RELAY**    | Store-and-forward, federation, persistence, retention policy enforcement    |

### 4.1 Composability

Any combination of roles can run on any device that has the
resources. Common configurations:

| Device                              | Roles carried              |
|-------------------------------------|----------------------------|
| Operator phone with SDR             | OPERATOR + SENSOR          |
| Operator phone without SDR          | OPERATOR (+ RELAY optional)|
| Sensor-only Pi (drop sensor)        | SENSOR                     |
| Persistent Pi at the operator       | SENSOR + RELAY             |
| Vehicle TOC laptop                  | OPERATOR + RELAY (+ SENSOR optional) |
| Headless aggregation server         | RELAY                      |

### 4.2 Cross-Cutting: the Kujhad Fleet API + Python Backend

Every C++ node (phone or Pi) exposes the **Kujhad Fleet HTTP API**
(`/v1/identify`, `/v1/gps`, `/v1/state`, `/v1/events?since=`,
`/v1/command`) over TLS with a per-node API key. This is the wire
contract between sensors and any operator-side intelligence layer.

A **Python intelligence backend** (FastAPI + asyncio, in `backend/`)
runs on the operator's device or vehicle TOC and:

- polls every fleet node's `/v1/events` over `KujhadClient`
- fuses incoming `RFEvent`s into `EmitterTrack`s via
  `HardwareAwareAssociator` and `TrackManager`
- runs the 6-method `AnomalyDetector` against each updated track
- produces `AssessmentReport`s via `DecisionEngine`
- exposes everything to the operator UI over its own REST/SSE API
  (`/api/v1/tracks`, `/nodes`, `/events`, `/assessments`)

Optional persistence: PostgreSQL (track history) and InfluxDB
(time-series RF events, 30 d retention) via the shipped
`docker-compose.yml`. Persistence is opt-in — the backend works
in-memory for lone-wolf and tactical deployments.

Multi-transport (RNS/LoRa, mesh failover) is a future addition that
sits **under** the Kujhad HTTP layer (e.g., a transport-multiplexer
agent that exposes the same HTTP surface over alternate carriers).
v1 ships with TCP/TLS over IP only.

Document 2 details the wire protocol, SensorNodeTrust model, and the
backend's internal architecture.

---

## 5. Bill of Materials Tiers

Each tier is a known-good shopping list that supports a subset of
vignettes. Tiers are additive — Tier 2 includes everything in Tier 1.

### Tier 1 — Lone Wolf / Bench Validation
Supports vignettes 3.1.

| Item                          | Qty | Approx USD | Notes                              |
|-------------------------------|-----|------------|------------------------------------|
| Samsung S21/S22/S23 (used)    | 1   | 200–400    | Operator phone. Existing for CJ.   |
| RTL-SDR Blog v4               | 1   | 35         | Confirmed working on S21/S22.      |
| HackRF One                    | 1   | 320        | Confirmed working on S21/S22.      |
| USB-OTG cable + powered hub   | 1   | 15         | HackRF can brown out without hub.  |
| Telescoping antenna kit       | 1   | 25         | RTL-SDR Blog kit is fine.          |

**~$600 entry cost**, runs the entire Predator OPERATOR + SENSOR
stack on one device.

### Tier 2 — Sensor Field / Headline CONOPS
Adds support for vignettes 3.2, 3.3, and limited 3.4.

| Item                                | Qty | Approx USD ea | Notes                                           |
|-------------------------------------|-----|---------------|-------------------------------------------------|
| Raspberry Pi 4 (4 GB) **or** Pi 5 (4 GB) | 8–24 | 55 / 80   | Pi 5 preferred for HackRF. Pi 4 fine for RTL.   |
| MicroSD 64 GB A2                    | 8–24 | 10            | Sized for ~1 month of Tier-MED retention.       |
| RTL-SDR v4 **or** HackRF One        | 8–24 | 35 / 320      | Mix freely; agent handles heterogeneous fleet.  |
| USB GPS dongle (uBlox 7/8 NMEA)     | 8–24 | 20            | Sub-100 ms time accuracy.                       |
| GPS HAT with PPS pin (Adafruit Ult.)| 2–4  | 45            | At least 2 per AO for future TDOA pairing.      |
| Pi PoE+ HAT or 5V/4A USB-C supply   | 8–24 | 20            | Whichever matches your power plan.              |
| Weather-sealed enclosure (drop kit) | 8–24 | 20–60         | Pelican 1050 / Apache 2800 / 3D-printed.        |

**Pi class is an organizational planning consideration**, not an
engineering one. The agent and the Predator core run identically on
Pi 4, Pi 5, and (for SENSOR-only roles with RTL-SDR) Pi Zero 2W. The
choice is about:

- **Pi Zero 2W** — cheapest, smallest, USB-2 only. Good for cheap drop
  sensors with RTL-SDR. Cannot keep up with HackRF wide bandwidth.
- **Pi 4 4 GB** — mature, plentiful, runs anything you'll run on it.
  Default recommendation for general sensor duty.
- **Pi 5 4 GB** — best CPU per dollar, USB-3, more thermal budget.
  Preferred for HackRF nodes, RELAY nodes, or anywhere DSP load is
  high (multiple parallel decoders).

### Tier 3 — Federated / TOC
Adds support for vignettes 3.4 and 3.5.

| Item                          | Qty | Approx USD | Notes                              |
|-------------------------------|-----|------------|------------------------------------|
| Vehicle-mount laptop (Linux)  | 1+  | 600+       | RELAY + optional OPERATOR + SENSOR.|
| LoRa USB radio (RAK / Ebyte)  | 4–8 | 25–40      | For RNS-over-LoRa transport.       |
| Cellular hotspot or modem     | 1+  | 200+       | For Skinny-class IP backhaul.      |
| External BUC/LNB (sat IP)     | 0–1 | 1500+      | Optional, for true OOB Skinny IP.  |
| GPS-disciplined oscillator    | 0–2 | 200–500    | Future TDOA reference.             |

---

## 6. Manpower Model

The platform is designed so that **one trained operator** can:

| Activity                                  | Time Budget     |
|-------------------------------------------|-----------------|
| Bring up a new SENSOR node from cold      | 2 minutes       |
| Re-add a node that has roamed back online | Automatic       |
| Recognize an emitter and exfil to higher  | 30 seconds      |
| Re-task the field to a new freq range     | < 1 minute      |
| Daily health audit of the field           | < 5 minutes     |

Initial training to operator-proficient: target **half a day** for
someone already comfortable with TAK + a software-defined radio.

### 6.1 Enrollment

A new SENSOR Pi joins the network by:

1. Receiving power.
2. The agent autostarts.
3. The agent announces on every available transport.
4. The OPERATOR's phone shows a "new node available" prompt.
5. Operator taps Accept. Done.

For the v1 trust model, "Accept" is sufficient. (Future v2 adds
operator-issued node certs via an enrollment ceremony — see doc 2.)

### 6.2 Deployment

Drop sensors are emplaced once, expected to run for hours to weeks
depending on power source. The platform makes no demand on the
emplacer beyond physical placement and power.

---

## 7. Pipeline Integration: SIGINT to Effects

A Predator detection becomes an actionable product through the
following sequence:

1. **Detection** — a SENSOR classifies an RF event and publishes it
   with position, time, classification, and confidence.
2. **Track** — RELAY (or local OPERATOR) associates the detection
   with prior detections to form a track. Track state machine and
   association math live in document 2.
3. **Marker** — track crosses a threshold (configurable, or operator
   manual) and is promoted to a marker visible on the operator map.
4. **Operator review** — the operator inspects the marker, possibly
   tasks the field for additional collection (more nodes, narrower
   freq, deep IQ snapshot) to confirm.
5. **Selective exfil** — operator selects the marker (and optionally
   relevant supporting hits) and pushes to higher via:
   - CoT to a connected TAK server / federate, or
   - CoT file written to a sync folder, or
   - Future Predator higher-echelon node (out of MVP scope).
6. **Audit** — every exfil is recorded with operator identity, target,
   destination, time, and reason. This is local + RELAY-replicated.

Steps 1–4 happen automatically and continuously across the field.
Step 5 is **always operator-initiated in v1**. Auto-exfil is
explicitly out of MVP scope (see section 9).

---

## 8. Key MVP Commitments

These are non-negotiable for the v1 platform release. They derive
directly from the vignettes and from customer requirements.

1. **Kujhad Fleet API is the wire contract.** Every SENSOR node
   exposes `/v1/identify`, `/v1/gps`, `/v1/state`, `/v1/events?since=`,
   `/v1/command` over TLS with per-node API key. The Python backend
   is the canonical client; third-party clients are welcome to use
   the same API. v1 ships TCP/TLS over IP only; multi-transport
   (RNS/LoRa) is post-MVP and slots in **under** the HTTP layer.
2. **Phone runs without an SDR.** The OPERATOR role is fully
   functional with no local DSP. Vignette 3.3 must work end to end.
3. **Manual selective exfil to higher.** Operator can tag any marker
   or out-of-baseline hit and push it to a CoT destination in 30 s
   or less. Audit trail is automatic. (CoT export already shipped
   on the C++ side in v1.2.0.)
4. **CoT export.** First-class output format for both interactive
   exfil and bulk file dumps. Compatibility validated against at
   least one real TAK federate.
5. **Priority-weighted retention.** When persistence is enabled
   (PostgreSQL + InfluxDB via `docker-compose.yml`), events age out
   by priority class first, wall-clock second. High-value signals
   (encrypted P25, mil air, anomalous emitters, manually flagged)
   persist far longer than routine traffic (commercial ADS-B,
   broadcast FM, NOAA wx). Class weights are config-driven. Defaults
   shipped per `docs/5_retention_policy.md`. In-memory mode (no
   docker stack) is the default for lone-wolf / tactical deployments.
6. **Heterogeneous sensor fleet.** A deployment may mix RTL-SDR and
   HackRF, Pi 4 and Pi 5 and Zero 2W, with and without GPS HATs, and
   the platform handles it without manual per-node tuning.
   `SensorNodeTrust` carries hardware-aware sensitivity, frequency
   stability, and timing trust factors derived from the
   `HardwareCapabilities` registry.
7. **Position and time on every event.** No event leaves a SENSOR
   without `(lat, lon, alt, t_utc, pos_uncertainty_m,
   t_uncertainty_us, node_id)`. `RFEvent` already carries
   `node_lat/node_lon/node_alt_m/timestamp_ns/node_id`.
8. **Trust at two layers.** Per-node API key + TLS at the Kujhad
   layer (already shipped). Network overlay (none / VLAN / RNS /
   ZeroTier) chosen per deployment. v1 ships **without** an in-app
   certificate authority; enrollment is operator "Accept" + key
   exchange. Document 2 details the threat model.
9. **Graceful degradation.** Loss of any single node — including the
   operator phone, including any RELAY — must not take down the
   platform. Lone-wolf vignette is a corollary: zero peers = a
   working platform. Backend runs in-memory with no docker stack;
   degrades to Android-only standalone if the backend is offline.

---

## 9. Non-Goals (v1)

These are deliberately out of scope. Each may become in-scope later,
but only after the MVP commitments are concrete and tested.

- **Auto-exfil.** Algorithmic decisions to push a marker to higher
  without operator involvement. The `DecisionEngine` produces
  `AssessmentReport`s and may set `escalate_to_atak=True`, but actual
  CoT transmission is always operator-initiated in v1.
- **In-app PKI / certificate authority.** Per-node API keys + TLS are
  the v1 trust mechanism. An app-managed CA with rotation/revocation
  is v2.
- **TDOA geolocation in production.** `TDOACoordinator` exists in
  the backend and the BOM hedges for it (GPS HAT spec in Tier 2),
  but production-grade multi-node TDOA needs PPS time sync that is
  realistic only for Pi-class nodes. v1 ships TDOA as **opt-in
  experimental**, not a default workflow.
- **TX / direction-finding / jamming / EW.** Predator is RX-only,
  log-and-map-only. Any TX capability of carried hardware (HackRF)
  is disabled in firmware-equivalent fashion.
- **Cloud-hosted central server.** The platform is field-resident.
  Cloud aggregation may be added behind a configuration toggle later
  if a customer asks for it explicitly.
- **Decoder algorithm research.** We adapt existing decoders
  (DSD-FME, RTL433, dump1090, etc.). New decoder R&D is out of scope.
- **Multi-transport agent (RNS / LoRa).** Wire contract is HTTP/TLS
  over IP in v1. Alternate transports slot in below the Kujhad API
  in a later release.

---

## 10. Glossary

| Term            | Meaning in this document                                                  |
|-----------------|---------------------------------------------------------------------------|
| Operator        | The human(s) running the platform, typically one per AO.                  |
| Field           | The set of SENSOR nodes deployed for an operation.                        |
| AO              | Area of operations.                                                       |
| Higher          | The echelon above the operator: TOC, fires cell, intel section, etc.     |
| TAK / CoT       | Team Awareness Kit / Cursor on Target — open tactical comms ecosystem.   |
| Kujhad Fleet API| The HTTP/TLS contract every C++ node exposes. See section 4.2 + doc 2.   |
| Backend         | The Python intelligence layer in `backend/`. See section 4.2 + doc 2.    |
| RFEvent         | Atomic detection from one node: freq, power, SNR, time, position, IDs.   |
| EmitterTrack    | Time-aggregated emitter formed by associating RFEvents.                  |
| SensorNodeTrust | Backend's hardware-aware trust model for one node. See `backend/models/`.|
| AnomalyDetector | Six-method anomaly screen on each updated track.                         |
| AssessmentReport| `DecisionEngine` output: threat level, confidence, recommended action.   |
| OPERATOR        | Predator role: UI + tasking + exfil. See section 4.                      |
| SENSOR          | Predator role: DSP + decoders + event publish. See section 4.            |
| RELAY           | Predator role: store-and-forward + federation + retention. See section 4.|
| Marker          | Promoted track presented to the operator on the map.                     |
| Selective exfil | Operator-initiated push of a marker / hit to higher.                     |
| Tier 1/2/3 BOM  | Hardware shopping lists. See section 5.                                  |
| RTAK-V2 / RNS   | *Post-MVP reference for multi-transport.* Not in v1.                     |
