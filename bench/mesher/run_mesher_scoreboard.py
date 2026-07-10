#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Mesher scoreboard: wall time + elem/node counts for hybrid/graded/hex.

Usage (from repo root, after build):
  python3 bench/mesher/run_mesher_scoreboard.py \\
      --polymesh build/apps/cli/polymesh \\
      --geom bench/geometries/public/plate.stl \\
      -h 0.05 --out bench/results/mesher-scoreboard.json
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path


MESHERS = ("hex", "hybrid", "graded", "tet")


def run_one(polymesh: Path, geom: Path, mesher: str, h: float, feature: bool) -> dict:
    cmd = [str(polymesh), "mesh", str(geom), "-h", str(h), "--mesher", mesher]
    if feature:
        cmd.append("--feature")
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    dt_ms = (time.perf_counter() - t0) * 1000.0
    out = {
        "mesher": mesher,
        "geom": str(geom),
        "h": h,
        "feature": feature,
        "wall_ms": round(dt_ms, 3),
        "returncode": proc.returncode,
        "stdout": proc.stdout.strip(),
        "stderr": proc.stderr.strip(),
    }
    # Best-effort parse "N elements" / note lines from CLI.
    text = proc.stdout + "\n" + proc.stderr
    m = re.search(r"(\d+)\s+elem", text, re.I)
    if m:
        out["elems"] = int(m.group(1))
    m = re.search(r"(\d+)\s+nodes", text, re.I)
    if m:
        out["nodes"] = int(m.group(1))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--polymesh", type=Path, default=Path("build/apps/cli/polymesh"))
    ap.add_argument("--geom", type=Path, default=Path("bench/geometries/public/unit_box.stl"))
    ap.add_argument("-H", "--h", type=float, default=0.1, dest="h")
    ap.add_argument("--feature", action="store_true")
    ap.add_argument("--out", type=Path, default=Path("bench/results/mesher-scoreboard.json"))
    ap.add_argument("--meshers", default=",".join(MESHERS))
    args = ap.parse_args()

    if not args.polymesh.is_file():
        print(f"missing polymesh binary: {args.polymesh}", file=sys.stderr)
        return 2
    if not args.geom.is_file():
        print(f"missing geometry: {args.geom}", file=sys.stderr)
        return 2

    rows = []
    for mesher in args.meshers.split(","):
        mesher = mesher.strip()
        if not mesher:
            continue
        print(f"meshing {args.geom.name} mesher={mesher} h={args.h} ...", flush=True)
        rows.append(run_one(args.polymesh, args.geom, mesher, args.h, args.feature))
        print(f"  -> {rows[-1].get('wall_ms')} ms rc={rows[-1]['returncode']}", flush=True)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema": "polymesh-mesher-scoreboard/v1",
        "rows": rows,
    }
    args.out.write_text(json.dumps(payload, indent=2) + "\n")
    print(f"wrote {args.out}")
    return 0 if all(r["returncode"] == 0 for r in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
