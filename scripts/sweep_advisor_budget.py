#!/usr/bin/env python3
"""Advisor budget sweep -- what the learned mesh advisor picks as the DOF cap tightens.

Runs the real CLI (`polymesh solve --advisor ... [--advisor-max-dof N]`) over a
few in-distribution corpus parts and a ladder of DOF budgets, and records the
decision JSON the CLI prints for every run. The showcase chart
(scripts/plot_benchmarks.py, key `advisor_budget`) parses the committed
output of this script; nothing about the figure is typed in by hand.

Usage:
    python3 scripts/sweep_advisor_budget.py            # writes bench/results/advisor-budget-sweep.json
"""

from __future__ import annotations

import datetime as _dt
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CLI = REPO / "build-fpoff/apps/cli/polymesh"
if not CLI.is_file():
    CLI = REPO / "build/apps/cli/polymesh"
ADVISOR = REPO / "bench/advisor"
OUT = REPO / "bench/results/advisor-budget-sweep.json"
VTU_DIR = Path("/tmp/advisor_budget_sweep")

# In-distribution families from the v4 corpus (the shipped model refuses OOD
# parts, which is correct behaviour but makes an empty chart).
PARTS = [
    "bench/geometries/corpus/primitives/box_hole_s0.step",
    "bench/geometries/corpus/primitives/plate_notch_s0.step",
    "bench/geometries/corpus/primitives/stepped_shaft_s0.step",
]

# 0 = unfiltered (the shipped default), then a tightening ladder.
BUDGETS = [0, 2000, 4000, 8000, 16000, 32000, 64000]

ADVISOR_LINE = re.compile(r"^advisor: (\{.*\})$", re.MULTILINE)


def git_rev() -> str:
    return subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=REPO, capture_output=True, text=True, check=True,
    ).stdout.strip()


def main() -> int:
    if not CLI.is_file():
        raise SystemExit(f"no built CLI at {CLI} — build first")
    VTU_DIR.mkdir(parents=True, exist_ok=True)
    records: list[dict] = []
    for part in PARTS:
        step = REPO / part
        if not step.is_file():
            raise SystemExit(f"missing corpus part {part}")
        for budget in BUDGETS:
            argv = [
                str(CLI), "solve", str(step),
                "-o", str(VTU_DIR / f"{step.stem}_{budget}.vtu"),
                "--advisor", str(ADVISOR),
            ]
            if budget > 0:
                argv += ["--advisor-max-dof", str(budget)]
            print(f"[{step.stem} budget={budget or 'none'}]", flush=True)
            done = subprocess.run(argv, cwd=REPO, capture_output=True, text=True,
                                  timeout=900)
            match = ADVISOR_LINE.search(done.stdout)
            if match is None:
                raise SystemExit(
                    f"no advisor JSON in stdout for {part} budget={budget}:\n"
                    f"{done.stdout}\n{done.stderr}")
            decision = json.loads(match.group(1))
            records.append({
                "part": step.stem,
                "max_dof": budget,
                "vetoed": bool(decision.get("vetoed")),
                "budget_refusal": bool(decision.get("budget_refusal")),
                "ood_distance": decision.get("ood_distance"),
                "mesher": decision.get("mesher"),
                "order": decision.get("order"),
                "h_rel": decision.get("h_rel"),
                "adapt_passes": decision.get("adapt_passes"),
                "predicted_dof": decision.get("predicted_dof"),
                "predicted_rel_err_rel": decision.get("predicted_rel_err_rel"),
                "note": decision.get("note") or "",
                "cli_exit": done.returncode,
            })
            print(f"    -> {decision.get('mesher')} order={decision.get('order')} "
                  f"dof={decision.get('predicted_dof')} "
                  f"score={decision.get('predicted_rel_err_rel')} "
                  f"vetoed={decision.get('vetoed')} "
                  f"budget_refusal={decision.get('budget_refusal')}", flush=True)

    payload = {
        "generated_utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_rev": git_rev(),
        "cli": str(CLI.relative_to(REPO)),
        "advisor_dir": "bench/advisor",
        "budgets": BUDGETS,
        "records": records,
    }
    OUT.write_text(json.dumps(payload, indent=2) + "\n")
    print(f"\nwrote {OUT.relative_to(REPO)} ({len(records)} runs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
