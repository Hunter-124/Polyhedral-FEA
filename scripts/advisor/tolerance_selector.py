#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Is "cheapest mesh within tolerance X" deliverable? Measured, per error model.

Training track 2b in ``docs/training/HANDOFF-3080ti.md`` asks for a learned
h-selector whose deliverable is *"cheapest mesh meeting tolerance X"*. The
selector itself needs no new head -- :func:`regret.tolerance_chooser` filters the
candidate actions by a predicted absolute ``rel_err`` and takes the cheapest
survivor -- so the whole question is whether any error model in this project
predicts ``rel_err`` well enough on an UNSEEN FAMILY to keep the promise the
deliverable makes.

That is what this script measures, and it measures it for both error models the
project has, on the same family-held-out folds and the same scorer
(:func:`regret.cost_at_tolerance`):

* ``net`` -- the shipped advisor's ``rel_err``/``dof`` heads, trained per fold.
* ``lgbm`` -- LightGBM on the same masked rows, which
  ``docs/advisor/0006-clean-data-retrain.md`` §3 already measured as the better
  cost model by 2.4x on DOF. If the selector fails on the net but works here,
  the limit is the net; if it fails on both, the limit is the label.

Each model is scored raw and margin-calibrated. The margin is fitted on the
fold's TRAINING cases only (:func:`regret.fit_tolerance_margin`) -- picking it on
the held-out cases would be choosing the operating point on the test set, and the
whole point of the exercise is out-of-family behaviour.

The reference to beat is ``finest_action``: the deployable trivial rule, "ask for
the finest mesh offered". A selector that violates the tolerance more often than
that rule is not shippable no matter how cheap its satisfying picks are, so the
tables are ordered by violation rate and cheapness is the tie-break.

Run from the repo root::

    python -m scripts.advisor.tolerance_selector --folds all
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

import numpy as np

if __package__ in (None, ""):  # direct `python scripts/advisor/tolerance_selector.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from . import regret as R  # noqa: E402
from .crossval import (  # noqa: E402
    ACTION_COLUMNS,
    action_matrix,
    advisor_scores,
    aggregate_tolerance,
    print_tolerance_table,
    train_fold,
)
from .dataset import (  # noqa: E402
    ADVISOR_DIR,
    SPLIT_MODES,
    AdvisorData,
    Split,
    group_of,
    load_dataset,
    provenance,
    split_groups,
)

REPORT_JSON = ADVISOR_DIR / "tolerance_selector.json"

#: The two heads a tolerance selector needs: what it promises, and what it
#: minimises. Nothing else is fitted here -- this is not a retrain.
SELECTOR_HEADS: tuple[str, str] = ("rel_err", "dof")

LGBM_PARAMS: dict[str, Any] = {
    "n_estimators": 300, "learning_rate": 0.05, "num_leaves": 31,
    "min_child_samples": 5, "n_jobs": 4, "verbose": -1,
}


def lgbm_predictions(data: AdvisorData, split: Split, cases: list[R.Case],
                     seed: int) -> dict[str, dict[str, np.ndarray]]:
    """Per-candidate ``rel_err`` and ``dof`` predictions from LightGBM.

    Fitted on the fold's training rows with each head's own mask, exactly as
    ``train.run_baseline`` does, so the comparison against the net is not
    confounded by a different row set.
    """
    from lightgbm import LGBMRegressor

    out: dict[str, dict[str, np.ndarray]] = {}
    for head in SELECTOR_HEADS:
        mask = data.train.masks[head]
        if int(mask.sum()) < 2:
            raise SystemExit(
                f"fold has {int(mask.sum())} training rows for head {head}; "
                "cannot fit a selector on it"
            )
        model = LGBMRegressor(random_state=seed, **LGBM_PARAMS)
        model.fit(data.train.x[mask], data.train.targets[head][mask])
        predicted = model.predict(split.x).astype(np.float64)
        out[head] = {case.part: predicted[np.asarray(case.rows, dtype=int)]
                     for case in cases}
    return out


def fold_predictions(csv: Path | None, split_mode: str, fold: int,
                     n_folds: int | None, args: argparse.Namespace) -> dict[str, Any]:
    """One fold's held-out cases and every error model's predictions for them.

    Held-out only. The margin is calibrated later, across folds, so nothing here
    may be an in-sample prediction: LightGBM in particular fits the training rows
    almost exactly, and a margin fitted on those lands at 0.00 decades and calls
    an uncalibrated selector calibrated.
    """
    data = load_dataset(csv, split=split_mode, fold=fold, n_folds=n_folds)
    grouping = (lambda part: group_of(part, data.split_mode))
    cases = R.build_cases(data.val, grouping)

    predictions: dict[str, dict[str, dict[str, np.ndarray]]] = {}
    if "lgbm" in args.models:
        predictions["lgbm"] = lgbm_predictions(data, data.val, cases, args.seed)
    if "net" in args.models:
        net = train_fold(data, args.seed, args.epochs, args.batch_size, args.learning_rate)
        scores = advisor_scores(net, data.val, cases, data)
        predictions["net"] = {head: scores[head] for head in SELECTOR_HEADS}

    return {
        "fold": fold,
        "held_out_groups": list(data.val_groups),
        "cases": cases,
        "actions": action_matrix(data.val, cases),
        "predictions": predictions,
    }


def calibrate_across_folds(folds: list[dict[str, Any]], model: str, target: float,
                           max_violation: float) -> dict[int, tuple[float, float]]:
    """Leave-one-fold-out margin: fit on the OTHER folds' held-out cases.

    Nested retraining would be the textbook answer and is not needed here: every
    fold already holds out a whole family, so pooling the other folds' held-out
    predictions is calibration data this fold's model never saw either. What it
    does assume is that the folds are exchangeable -- which is exactly the
    assumption a deployed selector makes when a new part arrives, so if it fails,
    the number that reports the failure is the one worth having.
    """
    out: dict[int, tuple[float, float]] = {}
    for held in folds:
        cases: list[R.Case] = []
        error: dict[str, np.ndarray] = {}
        cost: dict[str, np.ndarray] = {}
        for other in folds:
            if other["fold"] == held["fold"]:
                continue
            for case in other["cases"]:
                cases.append(case)
                error[case.part] = other["predictions"][model]["rel_err"][case.part]
                cost[case.part] = other["predictions"][model]["dof"][case.part]
        out[held["fold"]] = R.fit_tolerance_margin(
            cases, error, cost, target, max_violation=max_violation)
    return out


def score_fold(held: dict[str, Any], models: list[str],
               margins: dict[str, dict[float, dict[int, tuple[float, float]]]],
               ) -> dict[str, Any]:
    """Score one fold's held-out cases: references, raw selectors, calibrated ones."""
    cases = held["cases"]
    choosers: dict[str, R.Chooser] = {
        "oracle": R.oracle_chooser("rel_err"),
        "finest_action": R.finest_action_chooser(
            held["actions"], ACTION_COLUMNS.index("h_rel")),
        "spend_budget": R.spend_budget_chooser("dof"),
    }
    fitted: dict[str, Any] = {}
    for model in models:
        error = held["predictions"][model]["rel_err"]
        cost = held["predictions"][model]["dof"]
        fitted[model] = {}
        for target in R.DOF_TARGETS:
            margin, calibration_violation = margins[model][target][held["fold"]]
            choosers[f"{model}_tol_{target:g}"] = R.tolerance_chooser(error, cost, target)
            choosers[f"{model}_tol_{target:g}_cal"] = R.tolerance_chooser(
                error, cost, target, margin)
            fitted[model][f"rel_err<={target:g}"] = {
                "margin_decades": margin,
                "calibration_violation_rate": calibration_violation,
            }
    return {
        "fold": held["fold"],
        "held_out_groups": held["held_out_groups"],
        "n_val_cases": len(cases),
        "cost_at_tolerance": R.cost_at_tolerance(cases, choosers),
        "tolerance_margins": fitted,
    }


def synthetic_case(part: str, errors: list[float], costs: list[float],
                   delivered: list[bool]) -> R.Case:
    """One case with hand-written log10 outcomes. Undelivered actions are invalid."""
    valid = np.asarray(delivered, dtype=bool)
    case = R.Case(part=part, group=part, rows=list(range(len(errors))),
                  failed=~valid)
    for head, values in (("rel_err", errors), ("dof", costs)):
        case.outcomes[head] = np.asarray(values, dtype=np.float64)
        case.valid[head] = valid.copy()
    return case


def run_self_test() -> int:
    """The three contracts the tolerance scoring rests on, on synthetic outcomes.

    No dataset, no model, no randomness -- these are the properties that make the
    reported violation rate mean what 0007 says it means, so they are checked
    where a stray sign or mask cannot hide behind a plausible-looking table.
    """
    log = math.log10
    # Action 0 is cheap and inaccurate, 1 is mid, 2 is dear and accurate.
    case = synthetic_case("part", [log(0.2), log(0.04), log(0.001)],
                          [log(1e3), log(1e4), log(1e5)], [True, True, True])
    # Optimistic but correctly ORDERED: every action is predicted inside 5 %, so
    # an uncalibrated selector sees no reason not to take the cheapest.
    optimistic = {"part": np.asarray([log(0.0032), log(0.0016), log(0.001)])}
    cost = {"part": case.outcomes["dof"].copy()}

    # 1. An optimistic error model picks the cheapest action, which misses a 5 %
    #    tolerance: the violation is charged against the MEASURED outcome.
    raw = R.tolerance_chooser(optimistic, cost, 0.05)
    assert raw(case, np.ones(3, dtype=bool)) == 0, "raw selector must take the cheapest"
    scored = R.cost_at_tolerance([case], {"raw": raw}, targets=[0.05])["rel_err<=0.05"]["raw"]
    assert scored["attempted"] == 1 and scored["satisfied"] == 0, scored
    assert scored["violation_rate"] == 1.0, scored

    # 2. A margin wide enough to reject all three predictions falls back to the
    #    most accurate candidate, which satisfies, and costs 1 decade more than
    #    the cheapest satisfying action (action 1 at 1e4 DOF).
    calibrated = R.tolerance_chooser(optimistic, cost, 0.05, margin=2.0)
    assert calibrated(case, np.ones(3, dtype=bool)) == 2, "margin must reject all three"
    scored = R.cost_at_tolerance([case], {"cal": calibrated},
                                 targets=[0.05])["rel_err<=0.05"]["cal"]
    assert scored["satisfied"] == 1 and scored["violation_rate"] == 0.0, scored
    assert abs(scored["mean_cost_regret"] - 1.0) < 1e-12, scored

    # 3. An undelivered pick is a violation, not a missing datum. Same tolerance,
    #    but the cheap action produced no mesh, so it carries no measured error.
    undelivered = synthetic_case("part", [float("nan"), log(0.04), log(0.001)],
                                 [float("nan"), log(1e4), log(1e5)],
                                 [False, True, True])
    scored = R.cost_at_tolerance([undelivered], {"raw": raw},
                                 targets=[0.05])["rel_err<=0.05"]["raw"]
    assert scored["attempted"] == 1 and scored["violation_rate"] == 1.0, scored
    assert scored["mean_cost_regret"] != scored["mean_cost_regret"], scored

    # 4. The margin sweep returns the SMALLEST passing margin, not the widest.
    #    1.5 decades puts the bar at 1.6e-3, which admits only action 2.
    margin, violation = R.fit_tolerance_margin(
        [case], optimistic, cost, 0.05, max_violation=0.0,
        grid=[0.0, 0.5, 1.0, 1.5, 2.0])
    assert margin == 1.5 and violation == 0.0, (margin, violation)

    print("self-test: violation accounting, margin fallback, undelivered picks, "
          "smallest-margin fit — all pass")
    return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--csv", type=Path, default=None)
    parser.add_argument("--split", choices=list(SPLIT_MODES), default="family")
    parser.add_argument("--folds", default="all",
                        help="'all' or a comma-separated fold list")
    parser.add_argument("--n-folds", type=int, default=None)
    parser.add_argument("--models", default="net,lgbm",
                        help="comma-separated error models to score: net, lgbm")
    parser.add_argument("--max-violation", type=float, default=0.1,
                        help="training violation rate the fitted margin aims for")
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--learning-rate", type=float, default=3e-3)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--self-test", action="store_true",
                        help="check the tolerance scoring contracts on synthetic "
                             "outcomes; touches no dataset and no model")
    args = parser.parse_args(argv)
    args.models = [name.strip() for name in args.models.split(",") if name.strip()]
    unknown = [name for name in args.models if name not in ("net", "lgbm")]
    if unknown:
        raise SystemExit(f"unknown error model(s): {', '.join(unknown)}")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        return run_self_test()

    probe = load_dataset(args.csv, split=args.split, fold=0, n_folds=args.n_folds)
    n_groups = len(split_groups(probe.val.parts + probe.train.parts, args.split))
    total_folds = n_groups if args.n_folds is None else int(args.n_folds)
    folds = (list(range(total_folds)) if args.folds == "all"
             else [int(value) for value in args.folds.split(",")])

    print(f"dataset : {probe.csv_path} ({probe.n_rows} rows)")
    print(f"split   : {args.split}, {len(folds)} of {total_folds} folds")
    print(f"models  : {', '.join(args.models)}   "
          f"margin fitted leave-one-fold-out to <={args.max_violation:.0%} violations")

    predicted: list[dict[str, Any]] = []
    for fold in folds:
        held = fold_predictions(args.csv, args.split, fold, args.n_folds, args)
        predicted.append(held)
        print(f"  fold {fold} ({','.join(held['held_out_groups'])}): "
              f"{len(held['cases'])} held-out cases")

    if len(predicted) < 2:
        raise SystemExit(
            "leave-one-fold-out calibration needs at least 2 folds; "
            f"got {len(predicted)}"
        )

    margins = {
        model: {target: calibrate_across_folds(predicted, model, target, args.max_violation)
                for target in R.DOF_TARGETS}
        for model in args.models
    }
    runs = [score_fold(held, args.models, margins) for held in predicted]

    tolerance = aggregate_tolerance(runs)
    payload = {
        "provenance": provenance(probe, seed=args.seed, epochs=args.epochs, seeds=1,
                                 include_failures=False),
        "split_mode": args.split,
        "n_folds": total_folds,
        "folds": folds,
        "models": args.models,
        "max_violation": args.max_violation,
        "cost_at_tolerance": tolerance,
        "runs": runs,
    }
    out = args.out or REPORT_JSON
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    for label in sorted(tolerance):
        print_tolerance_table(tolerance, label)

    print("\nFitted margins (decades subtracted from the tolerance before filtering), "
          "median over folds")
    for model in args.models:
        labels = sorted({key for run in runs for key in run["tolerance_margins"][model]})
        for label in labels:
            fits = [run["tolerance_margins"][model][label] for run in runs]
            values = [fit["margin_decades"] for fit in fits]
            achieved = [fit["calibration_violation_rate"] for fit in fits
                        if fit["calibration_violation_rate"] == fit["calibration_violation_rate"]]
            achieved_text = (f"{float(np.median(achieved)):.1%}" if achieved
                             else "no reachable calibration case")
            print(f"  {model:>5} {label:>16}: margin {float(np.median(values)):.2f} "
                  f"(range {min(values):.2f}-{max(values):.2f}), "
                  f"violation on the calibration folds {achieved_text}")

    print(f"\nwrote {out}")
    print("A selector is shippable only if it violates the tolerance no more often than "
          "finest_action.\nCheapness among satisfying picks is the tie-break, never the "
          "headline.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
