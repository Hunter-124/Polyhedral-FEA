#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Export the advisor results report to docs/advisor/figures/.

``figures.py`` covers the training-time asset (per-epoch curves). This entry
point renders everything else the report needs -- the network it converged to
(``report_network``), what the mesh search actually bought
(``report_campaign``), pictures of the meshes themselves (``report_meshes``)
and the external-source comparison (``report_external``):

  network_layout.png    the final architecture, read out of the checkpoint:
                        grouped input columns, embedding tables, trunk width
                        and every head with its output width + parameter count
  mesh_progress.png     anytime curve -- best-so-far accuracy and geometric
                        fidelity against cumulative solver wall time, median
                        over cases with an inter-quartile band
  accuracy_vs_cost.png  accuracy_rel_err against n_dof and against solve_ms,
                        one point per successful row, coloured by mesher, with
                        the Pareto front drawn
  fidelity_vs_h.png     chamfer mean and surface-distance p99 against h_rel,
                        split by element order
  mesh_before_after.png real wireframe renders from the campaign warehouse:
                        the coarse baseline mesh beside the best-accuracy mesh
                        for three parts from three different families
  external_comparison.png
                        external mesh-source rel_err against active DOF,
                        split by case family and element order

Missing inputs skip the affected figure with a printed "no data yet" note --
the script still exits 0 so it is safe to run mid-campaign. Every number that
lands in a figure is also printed, so the values can be quoted directly.

Run from anywhere:

    python scripts/advisor/report.py
    python scripts/advisor/report.py --advisor-dir bench/advisor \
        --out-dir docs/advisor/figures
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import figstyle as fs  # noqa: E402
from advisor.paths import ADVISOR_DIR, CAMPAIGNS_DIR, REPO_ROOT  # noqa: E402
from advisor.report_campaign import accuracy_vs_cost, fidelity_vs_h, mesh_progress  # noqa: E402
from advisor.report_common import corpus_rows, read_rows  # noqa: E402
from advisor.report_external import external_comparison  # noqa: E402
from advisor.report_meshes import mesh_before_after  # noqa: E402
from advisor.report_network import network_layout  # noqa: E402

FIGURES_DIR = REPO_ROOT / "docs" / "advisor" / "figures"
EXTERNAL_RESULTS = REPO_ROOT / "bench" / "results" / "gmsh-peer.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--advisor-dir", type=Path, default=ADVISOR_DIR,
                        help="directory holding dataset.csv and runs/ "
                             "(default: bench/advisor)")
    parser.add_argument("--campaigns-dir", type=Path, default=CAMPAIGNS_DIR,
                        help="warehouse root holding <campaign>/runs/<cfg_id>/"
                             "<part>/t0/wire.png (default: bench/campaigns)")
    parser.add_argument("--out-dir", type=Path, default=FIGURES_DIR,
                        help="figure output directory (default: "
                             "docs/advisor/figures)")
    parser.add_argument("--external-results", type=Path, default=EXTERNAL_RESULTS,
                        help="Gmsh peer rows (default: bench/results/gmsh-peer.json)")
    parser.add_argument("--external-only", action="store_true",
                        help="render only external_comparison.png")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    fs.use("dark")  # house style (was the implicit light default)
    advisor_dir = args.advisor_dir
    out_dir = args.out_dir
    if args.external_only:
        written = int(external_comparison(args.external_results, out_dir))
        print(f"\n{written}/1 figures written to {out_dir}")
        return 0
    dataset_csv = advisor_dir / "dataset.csv"

    rows = read_rows(dataset_csv)
    if rows:
        print(f"dataset: {len(rows)} rows "
              f"({len(corpus_rows(rows))} advisor-row-v3) from {dataset_csv}")
    else:
        print(f"no data yet — {dataset_csv} is missing or empty")

    written = 0
    written += network_layout(advisor_dir, out_dir)
    written += mesh_progress(rows, out_dir)
    written += accuracy_vs_cost(rows, out_dir)
    written += fidelity_vs_h(rows, out_dir)
    written += mesh_before_after(rows, args.campaigns_dir, out_dir)
    written += external_comparison(args.external_results, out_dir)
    print(f"\n{written}/6 figures written to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
