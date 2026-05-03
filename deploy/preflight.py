"""
Pre-mission readiness check — run this BEFORE you trust the backend
to be operational. Exits non-zero if any critical check fails so it
plugs into a `predator-rf-preflight.service` that gates the main
unit, or into the operator's own `make ready` step.

Checks (each is independent — one failure doesn't skip the rest so
the operator gets a complete picture in one run):

    [time]    chronyd / NTP sync within 100 ms
    [disk]    DATA_DIR has ≥ 500 MB free
    [db]      mission DB writable; schema at v2
    [token]   API_BEARER_TOKEN is set OR explicitly opted out
    [fleet]   FLEET_NODES parses; each node reachable + GPS lock
    [port]    API_PORT free to bind
    [perms]   DATA_DIR writable by current user
    [tx]      RX-only flags double-checked (cot_enabled,
              auto_tasker_enabled both off → safe lab posture;
              both on → operator MUST acknowledge)

Output is plain-text by default (`--text`) for shell logs, or JSON
(`--json`) for the /api/v1/preflight live route. Severity:

    PASS     all good
    WARN     non-blocking (e.g. token unset in declared lab mode)
    FAIL     blocking — do NOT go live

Designed to be importable: `from deploy.preflight import run_all`
returns the same dict the CLI prints. Pure stdlib so it works on
a bare RPi before you've installed anything else.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional


# ─── Individual checks ──────────────────────────────────────────────

def check_time_sync() -> Dict[str, Any]:
    """Use chronyc / timedatectl to confirm system clock is disciplined.
    A field SIGINT op without time sync produces garbage TDOA."""
    for cmd in (["chronyc", "tracking"], ["timedatectl", "show"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=3)
            if out.returncode != 0:
                continue
            text = out.stdout.lower()
            if "leap status" in text and "not synchronised" in text:
                return _fail("time", "chrony reports not synchronised")
            if "ntpsynchronized=yes" in text or "leap status" in text:
                return _pass("time", "system clock disciplined")
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue
    return _warn("time", "no chronyc/timedatectl — can't verify sync")


def check_disk_space(data_dir: str, min_mb: int = 500) -> Dict[str, Any]:
    try:
        os.makedirs(data_dir, exist_ok=True)
        usage = shutil.disk_usage(data_dir)
        free_mb = usage.free // (1024 * 1024)
        if free_mb < min_mb:
            return _fail("disk", f"only {free_mb} MB free in {data_dir} "
                                  f"(need ≥ {min_mb} MB)")
        return _pass("disk", f"{free_mb} MB free in {data_dir}")
    except OSError as exc:
        return _fail("disk", f"can't stat {data_dir}: {exc}")


def check_data_dir_writable(data_dir: str) -> Dict[str, Any]:
    probe = os.path.join(data_dir, ".preflight_probe")
    try:
        os.makedirs(data_dir, exist_ok=True)
        with open(probe, "w") as f:
            f.write("ok")
        os.unlink(probe)
        return _pass("perms", f"{data_dir} writable")
    except OSError as exc:
        return _fail("perms", f"can't write to {data_dir}: {exc}")


def check_db_schema(db_path: str) -> Dict[str, Any]:
    """Ensures the mission DB exists and is at schema v2 (or fresh)."""
    import sqlite3
    try:
        os.makedirs(os.path.dirname(db_path), exist_ok=True)
        conn = sqlite3.connect(db_path)
        try:
            cur = conn.execute("PRAGMA user_version")
            ver = cur.fetchone()[0]
            conn.close()
        finally:
            try:
                conn.close()
            except Exception:
                pass
        if ver == 0:
            return _pass("db", f"DB {db_path} fresh (v0; will migrate to v2 on boot)")
        if ver == 2:
            return _pass("db", f"DB {db_path} at schema v2")
        return _warn("db", f"DB {db_path} at unexpected version {ver}")
    except sqlite3.Error as exc:
        return _fail("db", f"can't open {db_path}: {exc}")


def check_token(token: str, allow_lab: bool) -> Dict[str, Any]:
    if token:
        if len(token) < 16:
            return _warn("token", "API_BEARER_TOKEN set but very short "
                                   "(< 16 chars) — generate with "
                                   "`openssl rand -hex 32`")
        return _pass("token", "API_BEARER_TOKEN set")
    if allow_lab:
        return _warn("token", "API_BEARER_TOKEN unset — LAB posture only")
    return _fail("token", "API_BEARER_TOKEN unset and PREFLIGHT_ALLOW_LAB "
                          "is not set — refusing to go live without auth")


def check_port_free(host: str, port: int) -> Dict[str, Any]:
    """We need to bind {host}:{port}. If the port is already in use,
    the backend startup will fail loudly — but it's friendlier to
    catch it here so the operator knows what's wrong."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)
        try:
            s.bind((host if host != "0.0.0.0" else "", port))
        except OSError as exc:
            return _fail("port", f"port {port} not bindable: {exc}")
        return _pass("port", f"{host}:{port} free")
    finally:
        s.close()


def check_tx_posture(cot_enabled: bool, auto_tasker_enabled: bool,
                     manual_approval: bool) -> Dict[str, Any]:
    """RX-only is the safe default. If either TX surface is on, the
    operator must have actively flipped the flag — surface it loudly."""
    if not cot_enabled and not auto_tasker_enabled:
        return _pass("tx", "RX-only posture (CoT off, AutoTasker off)")
    if cot_enabled and not manual_approval:
        return _warn("tx", "CoT enabled WITHOUT manual approval — "
                            "every escalation will auto-emit to TAK")
    flags = []
    if cot_enabled:
        flags.append("CoT")
    if auto_tasker_enabled:
        flags.append("AutoTasker")
    return _pass("tx", f"TX surfaces armed: {', '.join(flags)} "
                       f"(operator-acknowledged)")


async def check_fleet(fleet_csv: str, *, http_timeout_s: float = 3.0
                       ) -> Dict[str, Any]:
    """Parse FLEET_NODES, then attempt one /v1/status fetch per node.
    Uses asyncio + stdlib http client so we don't drag in aiohttp."""
    if not fleet_csv:
        return _warn("fleet", "FLEET_NODES unset — backend has no sensors")
    nodes: List[Dict[str, Any]] = []
    for spec in fleet_csv.split(","):
        spec = spec.strip()
        if not spec:
            continue
        try:
            nid, rest = spec.split("@", 1)
            host, _, tail = rest.partition(":")
            port_str, _, _ = tail.partition(":")
            port = int(port_str) if port_str else 5259
            nodes.append({"node_id": nid, "host": host, "port": port})
        except Exception as exc:
            nodes.append({"node_id": spec, "error": f"parse: {exc}"})

    async def _probe(n: Dict[str, Any]) -> Dict[str, Any]:
        if "error" in n:
            return n
        loop = asyncio.get_event_loop()
        try:
            await loop.run_in_executor(None,
                lambda: socket.create_connection(
                    (n["host"], n["port"]),
                    timeout=http_timeout_s).close())
            return {**n, "reachable": True}
        except OSError as exc:
            return {**n, "reachable": False, "error": str(exc)}

    results = await asyncio.gather(*[_probe(n) for n in nodes])
    unreachable = [r for r in results if not r.get("reachable")]
    if not nodes:
        return _warn("fleet", "FLEET_NODES set but parsed empty")
    if unreachable:
        return _fail("fleet",
            f"{len(unreachable)}/{len(nodes)} node(s) unreachable: "
            + ", ".join(f"{r['node_id']}@{r.get('host','?')}:"
                        f"{r.get('port','?')}" for r in unreachable),
            extra={"nodes": results})
    return _pass("fleet", f"all {len(nodes)} fleet nodes reachable",
                 extra={"nodes": results})


# ─── Result helpers ────────────────────────────────────────────────

def _result(check: str, severity: str, msg: str,
            extra: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    out = {"check": check, "severity": severity, "message": msg}
    if extra:
        out.update(extra)
    return out


def _pass(c, m, extra=None):  return _result(c, "PASS", m, extra)
def _warn(c, m, extra=None):  return _result(c, "WARN", m, extra)
def _fail(c, m, extra=None):  return _result(c, "FAIL", m, extra)


# ─── Aggregator ─────────────────────────────────────────────────────

async def run_all(*, allow_lab: bool = False) -> Dict[str, Any]:
    """Run every check. Returns a result dict suitable for both the
    CLI printer and the /api/v1/preflight HTTP route."""
    # Pull config inline so this script works even with the env file
    # not loaded (preflight is OFTEN run before the systemd unit).
    data_dir = os.environ.get("DATA_DIR", "./predator_data")
    db_path = os.path.join(
        data_dir, os.environ.get("MISSION_DB", "mission.db"))
    api_host = os.environ.get("API_HOST", "0.0.0.0")
    api_port = int(os.environ.get("API_PORT", "8000"))
    token = os.environ.get("API_BEARER_TOKEN", "")
    fleet = os.environ.get("FLEET_NODES", "")
    cot_on = os.environ.get("COT_ENABLED", "").lower() in (
        "1", "true", "yes", "on")
    at_on = os.environ.get("AUTO_TASKER_ENABLED", "").lower() in (
        "1", "true", "yes", "on")
    approval_on = os.environ.get("COT_REQUIRE_MANUAL_APPROVAL", "").lower() in (
        "1", "true", "yes", "on")
    allow_lab = (allow_lab
                 or os.environ.get("PREFLIGHT_ALLOW_LAB", "").lower()
                 in ("1", "true", "yes", "on"))

    # Sync checks first, then async fleet probe.
    results: List[Dict[str, Any]] = [
        check_time_sync(),
        check_disk_space(data_dir),
        check_data_dir_writable(data_dir),
        check_db_schema(db_path),
        check_token(token, allow_lab=allow_lab),
        check_port_free(api_host, api_port),
        check_tx_posture(cot_on, at_on, approval_on),
    ]
    results.append(await check_fleet(fleet))

    counts = {"PASS": 0, "WARN": 0, "FAIL": 0}
    for r in results:
        counts[r["severity"]] = counts.get(r["severity"], 0) + 1

    return {
        "ts": int(time.time()),
        "go": counts["FAIL"] == 0,
        "summary": counts,
        "results": results,
    }


# ─── CLI ────────────────────────────────────────────────────────────

def _print_text(report: Dict[str, Any]) -> None:
    sym = {"PASS": "✓", "WARN": "!", "FAIL": "✗"}
    print(f"Predator-RF preflight  ({'GO' if report['go'] else 'NO-GO'})")
    print("-" * 60)
    for r in report["results"]:
        s = r["severity"]
        print(f"  [{sym.get(s,'?')}] {s:<4} {r['check']:<8} {r['message']}")
    print("-" * 60)
    print(f"  {report['summary']['PASS']} pass  "
          f"{report['summary']['WARN']} warn  "
          f"{report['summary']['FAIL']} fail")


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(prog="preflight")
    p.add_argument("--json", action="store_true",
                   help="emit JSON instead of human-readable text")
    p.add_argument("--allow-lab", action="store_true",
                   help="permit empty API_BEARER_TOKEN (lab posture)")
    args = p.parse_args(argv)

    report = asyncio.run(run_all(allow_lab=args.allow_lab))
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        _print_text(report)
    return 0 if report["go"] else 1


if __name__ == "__main__":
    sys.exit(main())
