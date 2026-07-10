#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run a simple cantilever deck in CalculiX (ccx) and emit a scoreboard JSON row.

Requires `ccx` on PATH. If `ccx` is missing, prints a skip message and exits 0
so CI does not fail on peer-optional machines.

Does not hardcode PolyMesh reference answers into src/.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def write_inp(path: Path) -> tuple[int, int]:
    """Write coarse C3D8 cantilever deck. Returns (nnodes, n_fixed_nodes)."""
    # Unit-ish beam: L=1 along x, fixed x=0, tip force on x=1.
    # Coarse hex mesh 4x1x1 using *ELEMENT, TYPE=C3D8.
    lines = [
        "*HEADING",
        "PolyMesh peer: cantilever tip load",
        "*NODE",
    ]
    # 5 x 2 x 2 nodes
    nid = 1
    node_map: dict[tuple[int, int, int], int] = {}
    for k in range(2):
        for j in range(2):
            for i in range(5):
                node_map[(i, j, k)] = nid
                lines.append(f"{nid}, {i * 0.25:.4f}, {j * 0.1:.4f}, {k * 0.1:.4f}")
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
    n_fixed = 0
    for j in range(2):
        for k in range(2):
            lines.append(f"{node_map[(0, j, k)]}, 1, 3")
            n_fixed += 1
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
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(node_map), n_fixed


def ccx_version(ccx: str) -> str:
    r = subprocess.run([ccx, "-v"], capture_output=True, text=True)
    text = (r.stdout or "") + (r.stderr or "")
    m = re.search(r"Version\s+([0-9.]+)", text, re.I)
    if m:
        return m.group(1)
    first = next((ln.strip() for ln in text.splitlines() if ln.strip()), "")
    return first or "ccx"


def main() -> int:
    ccx = shutil.which("ccx")
    if not ccx:
        print("ccx not found; skip CalculiX peer (install optional, see peers/calculix_stub.md)")
        return 0

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        inp = td_path / "cantilever.inp"
        nnodes, n_fixed = write_inp(inp)
        # All 3 DOFs fixed on root face nodes.
        free_dofs = nnodes * 3 - n_fixed * 3

        t0 = time.perf_counter()
        r = subprocess.run(
            [ccx, "cantilever"],
            cwd=td_path,
            capture_output=True,
            text=True,
        )
        wall = time.perf_counter() - t0
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr, file=sys.stderr)
            print(f"error: ccx failed with exit {r.returncode}", file=sys.stderr)
            return 1

        out = {
            "schema_version": 1,
            "solver": "calculix",
            "version": ccx_version(ccx),
            "case_id": "cantilever_smoke",
            "dofs": free_dofs,
            "wall_time_s": {"mesh": None, "solve": wall, "total": wall},
            "accuracy": {
                "name": "smoke_ran",
                "value": 1.0,
                "unit": "bool",
            },
            "label": "calculix-cantilever-smoke",
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "notes": (
                f"ccx C3D8 4x1x1 smoke deck ({nnodes} nodes, {free_dofs} free DOF); "
                "not a calibrated accuracy benchmark vs PolyMesh tip err"
            ),
        }
        out_path = ROOT / "bench" / "results" / "calculix-cantilever-smoke.json"
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
        print("wrote", out_path)

        # Best-effort scoreboard refresh (non-fatal if missing).
        render = ROOT / "bench" / "competitive" / "render_scoreboard.py"
        if render.is_file():
            subprocess.run([sys.executable, str(render)], check=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
