#!/usr/bin/env python3
"""Cross-check that the C++ port of CustodyElector
(`core/src/predator/custody_election.h`) and the Python implementation
(`backend/coordination/custody_election.py`) produce byte-identical
decisions for the shared scenarios in `tests/fixtures/custody_scenarios.json`.

The two implementations are kept in lockstep because:
  * The Python backend runs in the TOC, the C++ elector runs on a
    Controller-mode Android — and any divergence means the operator's
    on-device tasking decisions stop matching the TOC's.
  * The Python implementation's unit tests already pin the expected
    behaviour; this harness re-runs the same scenarios through C++.

Usage:
    python scripts/test_custody_parity.py
    python scripts/test_custody_parity.py --keep-build  # don't delete the built binary

The script will:
  1. g++-compile tests/custody_election_test.cpp (no-op if already built).
  2. Run the binary in --fixture mode and capture its JSON output.
  3. Run the Python elector against the same fixture.
  4. Diff the two outputs field-by-field with a small tolerance for
     floating-point drift on the soft-score components.
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests" / "fixtures" / "custody_scenarios.json"
CPP_SRC = ROOT / "tests" / "custody_election_test.cpp"
INCLUDE = ROOT / "core" / "src"

# Components are floats — accept tiny rounding deltas. Both sides round to
# 4 decimals when emitting JSON, so a 1e-4 epsilon is plenty.
FLOAT_TOL = 1e-4


def build_cpp(out_dir: Path) -> Path:
    binary = out_dir / "custody_election_test"
    cxx = shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        sys.exit("no C++ compiler found (need g++ or clang++)")
    cmd = [cxx, "-std=c++17", "-O2", f"-I{INCLUDE}",
           str(CPP_SRC), "-o", str(binary)]
    subprocess.run(cmd, check=True)
    return binary


def run_cpp(binary: Path) -> List[Dict[str, Any]]:
    out = subprocess.check_output([str(binary), "--fixture", str(FIXTURE)],
                                   text=True)
    return json.loads(out)


def run_python() -> List[Dict[str, Any]]:
    sys.path.insert(0, str(ROOT))
    from backend.coordination.custody_election import CustodyElector
    from backend.models.emitter_track import EmitterTrack
    from backend.models.sensor_node import SensorNodeTrust

    fixture = json.loads(FIXTURE.read_text())
    elector = CustodyElector(
        k_total=fixture.get("k_total", 3),
        handover_overlap_s=fixture.get("handover_overlap_s", 15.0),
        stale_gps_after_s=fixture.get("stale_gps_after_s", 300.0))
    out: List[Dict[str, Any]] = []
    for step in fixture["steps"]:
        t_in = step["track"]
        # Minimal EmitterTrack — only the fields the elector reads.
        track = EmitterTrack(emitter_id=t_in["track_id"])
        track.threat_level = t_in.get("threat_level", "low")
        if "estimated_lat" in t_in:
            track.estimated_lat = t_in["estimated_lat"]
            track.estimated_lon = t_in["estimated_lon"]
        track.protocol = t_in.get("protocol") or None
        for nid in t_in.get("detecting_nodes", []):
            if nid not in track.detecting_nodes:
                track.detecting_nodes.append(nid)

        nodes = []
        for n in step["nodes"]:
            sn = SensorNodeTrust(node_id=n["node_id"])
            sn.gps_synchronized = n.get("gps_synchronized", False)
            if "gps_lat" in n:
                sn.location_gps = (n["gps_lat"], n["gps_lon"])
            sn.location_gps_updated_ns = n.get("gps_updated_ns", 0)
            sn.sensitivity_trust = n.get("sensitivity_trust", 0.5)
            sn.available_decoders = list(n.get("available_decoders", []))
            sn.thermal_throttling_active = n.get("thermal_throttling_active", False)
            # Pin compute_trust_score → fixture's trust_score so both
            # sides see exactly the same value (the C++ port doesn't
            # reimplement compute_trust_score).
            ts = n.get("trust_score", 0.5)
            sn.compute_trust_score = lambda _ts=ts: _ts  # type: ignore[assignment]
            nodes.append(sn)

        loads = step.get("node_loads", {})
        d = elector.elect(track, nodes,
                           now_ns=step.get("now_ns"),
                           node_loads=loads)
        out.append(d.to_dict())
    return out


def _normalize(v: Any) -> Any:
    # C++ emits "" where Python emits None for "no primary" /
    # "no handover" — normalize both to None so the diff doesn't
    # flag the encoding difference as a behavioural mismatch.
    if v == "":
        return None
    return v


def cmp_value(a: Any, b: Any, path: str) -> List[str]:
    a = _normalize(a)
    b = _normalize(b)
    if isinstance(a, float) or isinstance(b, float):
        if a is None or b is None: return [f"{path}: {a!r} != {b!r}"]
        if math.isnan(a) and math.isnan(b): return []
        return [] if abs(float(a) - float(b)) <= FLOAT_TOL else [f"{path}: {a} != {b}"]
    if isinstance(a, dict) and isinstance(b, dict):
        diffs: List[str] = []
        for k in sorted(set(a) | set(b)):
            diffs += cmp_value(a.get(k), b.get(k), f"{path}.{k}")
        return diffs
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return [f"{path}: len {len(a)} != {len(b)}"]
        diffs = []
        for i, (x, y) in enumerate(zip(a, b)):
            diffs += cmp_value(x, y, f"{path}[{i}]")
        return diffs
    return [] if a == b else [f"{path}: {a!r} != {b!r}"]


def diff(cpp: List[Dict[str, Any]], py: List[Dict[str, Any]]) -> List[str]:
    if len(cpp) != len(py):
        return [f"step count mismatch: cpp={len(cpp)} py={len(py)}"]
    out: List[str] = []
    # Only compare the deterministic decision shape, not the full score
    # list — score components have minor float-rounding-order differences
    # that are noise (and both sides match within FLOAT_TOL anyway, so we
    # do compare them; just skip 'reason' which is a string template).
    keys = ("track_id", "primary", "backups", "tasked_nodes", "stand_down",
            "handover_from", "handover_until_ns", "scores")
    for i, (c, p) in enumerate(zip(cpp, py)):
        for k in keys:
            out += cmp_value(c.get(k), p.get(k), f"step[{i}].{k}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep-build", action="store_true")
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="custody_parity_"))
    try:
        binary = build_cpp(tmp)
        cpp_out = run_cpp(binary)
        py_out = run_python()
        diffs = diff(cpp_out, py_out)
        if diffs:
            print("PARITY MISMATCH:")
            for d in diffs[:50]:
                print(f"  {d}")
            return 1
        print(f"OK: {len(cpp_out)} scenarios, C++ and Python agree.")
        return 0
    finally:
        if not args.keep_build:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
