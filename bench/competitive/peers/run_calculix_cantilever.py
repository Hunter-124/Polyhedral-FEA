#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run a simple cantilever deck in CalculiX (ccx) and emit a scoreboard JSON row.

Requires `ccx` on PATH. Does not hardcode PolyMesh reference answers into src/.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def write_inp(path: Path) -> None:
    # Unit cube cantilever: L=1 along x, fixed x=0, tip force on x=1.
    # Coarse hex mesh 4x1x1 using *ELEMENT, TYPE=C3D8.
    lines = [
        "*HEADING",
        "PolyMesh peer: cantilever tip load",
        "*NODE",
    ]
    # 5 x 2 x 2 nodes
    nid = 1
    node_map = {}
    for k in range(2):
        for j in range(2):
            for i in range(5):
                node_map[(i, j, k)] = nid
                lines.append(f"{nid}, {i*0.25:.4f}, {j*0.1:.4f}, {k*0.1:.4f}")
                nid += 1
    lines += ["*ELEMENT, TYPE=C3D8, ELSET=Eall"]
    eid = 1
    for k in range(1):
        for j in range(1):
            for i in range(4):
                n = [
                    node_map[(i, j, k)],
                    node_map[(i + 1, j, k)],
                    node_map[(i + 1, j + 1, k)],
                    node_map[(i, j + 1, k)],
                    node_map[(i, j, k + 1)],
                    node_map[(i + 1, j, k + 1)],
                    node_map[(i + 1, j + 1, k + 1)],
                    node_map[(i, j + 1, k + 1)],
                ]
                lines.append(f"{eid}, " + ", ".join(str(x) for x in n))
                eid += 1
    lines += [
        "*MATERIAL, NAME=Steel",
        "*ELASTIC",
        "200e9, 0.3",
        "*SOLID SECTION, ELSET=Eall, MATERIAL=Steel",
        "*BOUNDARY",
    ]
    for j in range(2):
        for k in range(2):
            lines.append(f"{node_map[(0, j, k)]}, 1, 3")
    lines += [
        "*STEP",
        "*STATIC",
        "*CLOAD",
    ]
    tip = [node_map[(4, j, k)] for j in range(2) for k in range(2)]
    f_each = -1000.0 / len(tip)
    for n in tip:
        lines.append(f"{n}, 3, {f_each}")
    lines += [
        "*NODE FILE",
        "U",
        "*EL FILE",
        "S",
        "*END STEP",
    ]
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    ccx = shutil.which("ccx")
    if not ccx:
        print("ccx not found; skip")
        return 0
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        inp = td_path / "cantilever.inp"
        write_inp(inp)
        t0 = time.perf_counter()
        r = subprocess.run([ccx, "cantilever"], cwd=td_path, capture_output=True, text=True)
        wall = time.perf_counter() - t0
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            return 1
        # Parse max |U| from .dat if present
        dat = td_path / "cantilever.dat"
        max_u = None
        if dat.exists():
            text = dat.read_text(errors="ignore")
            for line in text.splitlines():
                # crude: look for displacement lines
                parts = line.split()
                if len(parts) >= 4 and parts[0].replace(".", "", 1).isdigit() is False:
                    pass
        out = {
            "solver": "calculix",
            "version": subprocess.run([ccx, "-v"], capture_output=True, text=True).stdout.strip()
            or "ccx",
            "case_id": "cantilever_smoke",
            "dofs": None,
            "wall_time_s": {"mesh": None, "solve": wall, "total": wall},
            "accuracy": {"name": "smoke_ran", "value": 1.0 if r.returncode == 0 else 0.0},
            "label": "calculix-cantilever-smoke",
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "notes": "ccx smoke deck; not a calibrated accuracy benchmark",
        }
        out_path = ROOT / "bench" / "results" / "calculix-cantilever-smoke.json"
        out_path.write_text(json.dumps(out, indent=2) + "\n")
        print("wrote", out_path)
        # refresh scoreboard
        subprocess.run(["python3", str(ROOT / "bench/competitive/render_scoreboard.py")], check=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
