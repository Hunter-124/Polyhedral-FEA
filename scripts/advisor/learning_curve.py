#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""How many geometry families does the advisor need before it generalises?

    python scripts/advisor/learning_curve.py --seeds 3

The question this answers
-------------------------
Adding fifteen real, correct geometric descriptors made held-out-family regret
*worse*, and the mechanism is measured: a 1-nearest-neighbour classifier
recovers the family from those descriptors alone at 24/24 = 100 %. On a
six-family corpus, per-part geometry IS family identity, so under
leave-one-family-out a geometric feature cannot transfer -- it can only help the
model memorise the families it has seen.

That is a statement about corpus width, not about descriptor quality, and it
implies a concrete question before spending machine-weeks on new geometry: does
held-out-family regret actually improve as families are added, and at what rate?

Method
------
Hold out one family. Train on ``k`` of the remaining families, for
``k = 1 .. n_families - 1``, with the kept subset chosen by a seeded shuffle so
that averaging over seeds averages over *which* families were kept. Dropped
families leave the training split entirely -- including the imputer and the
standardiser, or the curve would measure a model with partial sight of data it
was denied.

Reported per ``k``: regret at the primary budget, for the gated chooser we
intend to ship and for the trivial rule it must beat. A curve that is flat in
``k`` says more families will not help and the ceiling is elsewhere. A curve
still descending at ``k = 5`` says the corpus is the binding constraint, and its
slope is the basis for estimating how many more are needed.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import numpy as np
import torch

if __package__ in (None, ""):  # direct invocation
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from . import regret as R  # noqa: E402
from .crossval import build_choosers, train_fold  # noqa: E402
from .dataset import (  # noqa: E402
    ADVISOR_DIR,
    SPLIT_MODES,
    load_dataset,
    provenance,
    split_groups,
)

REPORT_JSON = ADVISOR_DIR / "learning_curve.json"

#: Choosers tracked across the curve. The advisor variants should improve with
#: more families; the trivial rules cannot, because they do not learn -- which
#: makes them the control that proves the curve is measuring learning and not
#: some artefact of the shrinking training set.
TRACKED = ["advisor_gated_0.05", "advisor_argmin", "advisor_policy",
           "finest_action", "constant_config", "default"]


def run_point(csv: Path | None, split: str, fold: int, k: int, seed: int,
              args: argparse.Namespace) -> dict[str, Any] | None:
    try:
        data = load_dataset(csv, split=split, fold=fold,
                            max_train_groups=k, group_seed=seed)
    except SystemExit:
        # A small subset can land entirely on families with no trusted accuracy
        # rows -- `channel` has none at all. load_dataset refuses that rather
        # than export an untrained head, which is right; here it just means this
        # (k, fold, seed) point is unmeasurable and is dropped. The count of
        # surviving points is reported per k so a thin row is visible.
        return None
    if data.train.n_rows == 0:
        return None
    net = train_fold(data, seed, args.epochs, args.batch_size, args.learning_rate)
    choosers, cases, _ = build_choosers(net, data, seed, args.objective,
                                        args.budget_head, include_failures=True)
    if not cases:
        return None
    scored = R.score(cases, choosers, heads=[args.objective],
                     budget_head=args.budget_head,
                     budget_quantiles=[args.primary_quantile],
                     bands=[(0.4, 0.6)], allow_failed=True)
    level = scored["levels"][f"q{args.primary_quantile:g}"]
    band = scored["levels"]["band0.4-0.6"]
    return {
        "fold": fold, "k": k, "seed": seed,
        "held_out": list(data.val_groups),
        "n_train_rows": data.train.n_rows,
        "n_train_groups": len(split_groups(data.train.parts, split)),
        "regret": {name: stats["mean_regret"]
                   for name, stats in level["heads"][args.objective].items()},
        "matched_cost_regret": {name: stats["mean_regret"]
                                for name, stats in band["heads"][args.objective].items()},
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--csv", type=Path, default=None)
    parser.add_argument("--split", choices=list(SPLIT_MODES), default="family")
    parser.add_argument("--seeds", type=int, default=3)
    parser.add_argument("--seed0", type=int, default=1234)
    parser.add_argument("--epochs", type=int, default=25)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--learning-rate", type=float, default=3e-3)
    parser.add_argument("--objective", default="rel_err")
    parser.add_argument("--budget-head", default="dof")
    parser.add_argument("--primary-quantile", type=float, default=0.5)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args(argv)
    torch.set_num_threads(max(1, int(args.threads)))

    probe = load_dataset(args.csv, split=args.split, fold=0)
    groups = split_groups(probe.train.parts + probe.val.parts, args.split)
    n_groups = len(groups)
    seeds = [args.seed0 + i for i in range(max(1, args.seeds))]
    print(f"{n_groups} {args.split} groups: {groups}")
    print(f"sweeping k = 1..{n_groups - 1} training groups x {n_groups} folds "
          f"x {len(seeds)} seeds")

    runs: list[dict[str, Any]] = []
    for k in range(1, n_groups):
        for fold in range(n_groups):
            for seed in seeds:
                point = run_point(args.csv, args.split, fold, k, seed, args)
                if point is not None:
                    runs.append(point)
        done = [r for r in runs if r["k"] == k]
        if done:
            values = [r["regret"].get("advisor_gated_0.05", float("nan")) for r in done]
            values = [v for v in values if v == v]
            print(f"  k={k}: {len(done)} points, gated regret "
                  f"{np.mean(values):.4f}" if values else f"  k={k}: no scorable points")

    curve: dict[str, Any] = {}
    for k in sorted({r["k"] for r in runs}):
        subset = [r for r in runs if r["k"] == k]
        entry: dict[str, Any] = {"n_points": len(subset),
                                 "mean_train_rows": float(np.mean(
                                     [r["n_train_rows"] for r in subset]))}
        for name in TRACKED:
            values = [r["regret"][name] for r in subset if name in r["regret"]]
            band = [r["matched_cost_regret"][name] for r in subset
                    if name in r["matched_cost_regret"]]
            if values:
                entry[name] = {
                    "mean_regret": float(np.mean(values)),
                    "std": float(np.std(values, ddof=1)) if len(values) > 1 else 0.0,
                    "matched_cost": float(np.mean(band)) if band else float("nan"),
                    "n": len(values),
                }
        curve[str(k)] = entry

    out = args.out or REPORT_JSON
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"provenance": provenance(probe, seed=args.seed0,
                                                    epochs=args.epochs),
                               "split": args.split, "objective": args.objective,
                               "curve": curve, "runs": runs}, indent=2) + "\n",
                   encoding="utf-8")

    print(f"\nregret at q{args.primary_quantile:g} vs number of training "
          f"{args.split} groups")
    print(f"{'k':>3} {'rows':>7}" + "".join(f"{n[:14]:>15}" for n in TRACKED))
    for k, entry in curve.items():
        cells = "".join(
            f"{entry[n]['mean_regret']:>15.4f}" if n in entry else f"{'-':>15}"
            for n in TRACKED)
        print(f"{k:>3} {entry['mean_train_rows']:>7.0f}{cells}")

    print(f"\nmatched-cost (band 0.4-0.6) regret vs k")
    print(f"{'k':>3}" + "".join(f"{n[:14]:>15}" for n in TRACKED))
    for k, entry in curve.items():
        cells = "".join(
            f"{entry[n]['matched_cost']:>15.4f}" if n in entry else f"{'-':>15}"
            for n in TRACKED)
        print(f"{k:>3}{cells}")

    print("\nThe trivial rules do not learn, so their columns should be flat in k;")
    print("any slope there is an artefact of the shrinking candidate pool, not learning.")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
