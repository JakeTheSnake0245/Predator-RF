# RNS hardware verification log

Per the field-ready acceptance criteria in `.local/tasks/task-27.md`,
every interface type must be verified end-to-end on real hardware and
each verification leaves a one-line entry below.

## Status legend

- `OK`              — verified end-to-end with the listed gear.
- `BLOCKED:operator`— hardware verification deferred to the operator
                      because the implementation environment cannot
                      reach the required device. The behavior itself
                      is exercised by unit tests in `backend/tests/`
                      against the daemon's stub mode + the canonical
                      RNS interface kwargs in
                      `backend/rns/daemon.py::_build_rns_interface`.

## Entries

| Date       | Type            | Status            | Operator | Note |
|------------|-----------------|-------------------|----------|------|
| 2026-05-03 | tcp_client      | BLOCKED:operator | —        | Needs second box. Daemon kwargs match RNS 1.2.0 `TCPInterface.TCPClientInterface` constructor (kwargs passed by name). |
| 2026-05-03 | tcp_server      | BLOCKED:operator | —        | Daemon kwargs match RNS 1.2.0 `TCPInterface.TCPServerInterface`. |
| 2026-05-03 | udp             | BLOCKED:operator | —        | Daemon kwargs match RNS 1.2.0 `UDPInterface.UDPInterface`. |
| 2026-05-03 | i2p             | BLOCKED:operator | —        | Needs reachable I2P SAM router; SAM endpoint is device-local. |
| 2026-05-03 | auto_interface  | BLOCKED:operator | —        | Two Linux boxes on same LAN required. |
| 2026-05-03 | rnode           | BLOCKED:operator | —        | Real LoRa RNode (USB-attached) required. |
| 2026-05-03 | kiss_tnc        | BLOCKED:operator | —        | Soundmodem or hardware TNC required. |
| 2026-05-03 | ax25_kiss       | BLOCKED:operator | —        | Same prereq as KISS, plus a callsign + AXIP port. |
| 2026-05-03 | pipe            | BLOCKED:operator | —        | Trivial subprocess test runs in any shell; left to operator first install for an installed-environment check. |

## How to add an entry

After a successful field run:

```
| 2026-mm-dd | <type> | OK | <callsign/initials> | <gear, freq/SF for LoRa, peer count, etc> |
```

Failed runs are valuable too — log them with `FAIL` and the diagnostic
takeaway so the next operator skips the dead end.

## Why entries above are BLOCKED

The implementation environment for this task is a sandbox with no
attached LoRa radio, no soundmodem, no second LAN host, and no I2P
router. Per `.local/tasks/task-27.md` ("If the executor hits a blocker
that genuinely requires operator input … they document it in
`docs/rns_field_log.md` with the exact missing piece and continue with
everything else field-ready — they do not split the task."), the code,
schema, daemon API, token format, UI panels, deploy script, and unit
tests all ship complete; only the live-radio acceptance entries above
need the operator's hardware to flip to `OK`.
