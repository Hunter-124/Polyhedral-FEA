#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Emit / refresh PolyMesh gate1-p1 scoreboard JSON from the GATE 1 report.

Best-effort: accuracy values and wall times come from
`bench/reports/p1-gate1-convergence.md` and historical ctest notes already
recorded in `bench/results/polymesh-gate1-p1.json`. Active DOFs are estimated
from the structured-mesh dimensions used in the Catch2 Tier-1 tests (hex20
promotion), not measured by re-running the solver.

Usage:
  python3 bench/competitive/emit_polymesh_gate1.py
  python3 bench/competitive/emit_polymesh_gate1.py --write
  python3 bench/competitive/emit_polymesh_gate1.py --write --render

Does not touch src/. Reference closed forms stay in bench/reference/.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "bench" / "results" / "polymesh-gate1-p1.json"


def hex20_node_count(nx: int, ny: int, nz: int) -> int:
    """Structured hex20 node count for nx×ny×nz cells (8 corners + 12 edge mids)."""
    # corners
    c = (nx + 1) * (ny + 1) * (nz + 1)
    # edge midpoints
    ex = nx * (ny + 1) * (nz + 1)
    ey = (nx + 1) * ny * (nz + 1)
    ez = (nx + 1) * (ny + 1) * nz
    return c + ex + ey + ez


def face_nodes_hex20(n_perp: int, na: int, nb: int) -> int:
    """Nodes on one structured face with n_perp==0 index, na×nb cells on face.

    Face with fixed i=0: corners (1)*(na+1)*(nb+1) + edge mids along a and b
    (no edges in the fixed-i direction on the face plane for i-edges that leave
    the face). For a face of na × nb cells: serendipity quad8 grid.
    """
    # quad8 structured: corners + edge mids (no face center)
    return (na + 1) * (nb + 1) + na * (nb + 1) + (na + 1) * nb


def gate1_rows() -> list[dict]:
    """Labeled gate1-p1 points for scoreboard (Lamé, Kirsch, cantilever)."""
    # Mesh dims from tests/test_*.cpp (hex8 cells before promote_to_quadratic).
    # Cantilever: box_hex_mesh(16, 2, 2); fix all DOF on x=0 face.
    cant_nodes = hex20_node_count(16, 2, 2)
    cant_fixed = face_nodes_hex20(0, 2, 2) * 3  # 3 DOF per root-face node
    cant_dofs = cant_nodes * 3 - cant_fixed

    # Lamé: nr=4, nt=12, nz=1; plane-strain u_z=0 all nodes + symmetry planes.
    # Active free DOFs after BC elimination are not trivial to count without a
    # solve; report total kinematic DOFs (3×nnodes) as a transparent estimate
    # and note that BC-eliminated free DOFs are lower.
    lame_nodes = hex20_node_count(4, 12, 1)
    kirsch_nodes = hex20_node_count(6, 10, 1)

    # Metrics from docs/progress.md / bench/reports/p1-gate1-convergence.md.
    # Wall times are approximate ctest walls retained from the original snapshot.
    ts = "2026-07-10T00:00:00Z"
    common = {
        "schema_version": 1,
        "solver": "PolyMesh",
        "version": "0.1.0",
        "label": "gate1-p1",
        "timestamp": ts,
    }

    return [
        {
            **common,
            "case_id": "timoshenko-cantilever",
            "dofs": cant_dofs,
            "wall_time_s": {"mesh": None, "solve": None, "total": 0.45},
            "accuracy": {
                "name": "tip_rel_err_pct",
                "value": 1.50,
                "unit": "percent",
            },
            "notes": (
                f"hex20 gravity cantilever; tip err from GATE1 report. "
                f"dofs≈{cant_dofs} free (hex20 16×2×2, root face fixed; estimate). "
                "total ≈ ctest wall (structured mesh + solve)."
            ),
        },
        {
            **common,
            "case_id": "lame-cylinder",
            "dofs": lame_nodes * 3,
            "wall_time_s": {"mesh": None, "solve": None, "total": 0.32},
            "accuracy": {
                "name": "u_r_rel_err_pct",
                "value": 0.0068,
                "unit": "percent",
            },
            "notes": (
                f"hex20 sector nr=4 nt=12 nz=1; u_r at inner wall. "
                f"dofs={lame_nodes * 3} (=3×nnodes; free DOFs after BC lower). "
                "Companion hoop err 1.36% (see hoop entry). total ≈ ctest wall."
            ),
        },
        {
            **common,
            "case_id": "lame-cylinder",
            "dofs": lame_nodes * 3,
            "wall_time_s": {"mesh": None, "solve": None, "total": 0.32},
            "accuracy": {
                "name": "hoop_rel_err_pct",
                "value": 1.36,
                "unit": "percent",
            },
            "notes": (
                f"hex20 sector; hoop stress at inner wall. Same run as u_r. "
                f"dofs={lame_nodes * 3} (=3×nnodes estimate)."
            ),
        },
        {
            **common,
            "case_id": "kirsch-plate",
            "dofs": kirsch_nodes * 3,
            "wall_time_s": {"mesh": None, "solve": None, "total": 0.39},
            "accuracy": {
                "name": "scf_rel_err_pct",
                "value": 1.87,
                "unit": "percent",
            },
            "notes": (
                f"hex20 annulus nr=6 nt=10 nz=1, exact field BC; SCF 3.056 vs 3. "
                f"dofs={kirsch_nodes * 3} (=3×nnodes estimate). total ≈ ctest wall."
            ),
        },
    ]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--write",
        action="store_true",
        help=f"write {OUT.relative_to(ROOT)} (default: print JSON to stdout)",
    )
    ap.add_argument(
        "--render",
        action="store_true",
        help="after --write, run render_scoreboard.py",
    )
    args = ap.parse_args()

    rows = gate1_rows()
    text = json.dumps(rows, indent=2) + "\n"

    if not args.write:
        sys.stdout.write(text)
        print(
            f"# {len(rows)} rows; re-run with --write to update "
            f"{OUT.relative_to(ROOT)}",
            file=sys.stderr,
        )
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT} ({len(rows)} rows)", file=sys.stderr)

    if args.render:
        render = ROOT / "bench" / "competitive" / "render_scoreboard.py"
        r = subprocess.run([sys.executable, str(render)])
        return r.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
