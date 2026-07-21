---
name: Remote Fox Hunt (foxbeacon) design
description: Why networked fox-hunt beacon tasking uses a non-tx.* command class and how its gates are layered
---

# Remote Fox Hunt vs the tx.* jamming rejects

Networked "Remote Fox Hunt" tasks a fleet node to run a LOCAL CW fox-hunt
beacon for sanctioned ham hunts. It uses command class **`foxbeacon`**, which
is deliberately **NOT** a `tx.*` class.

**Why:** the whole RX-only wire posture depends on `tx.*` being hard-rejected at
every ingress (Kujhad HTTP, RNS cmd.v1, web /api/command, control socket). If
remote beaconing were expressed as `tx.*`, enabling it would mean weakening
those rejects — which would also open the door to jamming/EW (explicitly out of
scope). A separate `foxbeacon` class keeps all `tx.*` rejects fully intact.

**How to apply / gate layering (defense in depth):**
- `foxbeacon` must be added to BOTH allowlists: Kujhad HTTP dispatch
  (`kujhad_fleet.h`) and RNS cmd.v1 (`backend/rns/cmd.py`). Never to the tx.* set.
- Per-node opt-in `remoteFoxHuntEnabled`, default OFF, config-persisted. The
  Kujhad server holds it as `std::atomic<bool>` pushed every GUI frame via
  `setRemoteFoxHuntEnabled`; the wire gate returns 403 when off.
- Anything reading the opt-in on the SERVER WORKER THREAD (e.g. the identify
  provider) must read the server atomic `remoteFoxHuntEnabled()`, NOT the plain
  UI-thread `bool` member — the latter is a data race.
- Command ACK truthfulness: the command handler queues execution to the GUI
  thread (pending-drain), so pre-reject deterministic failures IN THE HANDLER
  (`#ifndef OPT_BUILD_FOXHUNT`, empty TxDriverRegistry, bad args) so the HTTP/RNS
  response isn't a false "started". Residual device/engine failures surface via
  `remoteFoxHuntStatus` after the drain.
- Execution `MainWindow::applyRemoteFoxBeacon` is fully under
  `#ifdef OPT_BUILD_FOXHUNT`; forces `TxSource::CW_BEACON`. CW_BEACON requires a
  non-empty callsign or the engine refuses start.
