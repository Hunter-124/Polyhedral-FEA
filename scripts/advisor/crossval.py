#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Leakage-safe cross-validation of the advisor's *decisions*.

    python scripts/advisor/crossval.py --epochs 40 --seeds 5

What this answers that ``evaluate.py`` on a single checkpoint cannot:

1. **Does it generalize to an unseen geometry family?** The corpus is
   parametric -- ``box_hole_s0_c0`` and ``box_hole_s0_c1`` are the same CAD
   solid under a different load -- so a per-part split puts an identical
   geometry, and under the v3 corpus an identical (geometry, action) row, on
   both sides. This holds out whole families, one per fold.

2. **Does it beat a chooser with no parameters?** Three baselines the project
   was missing are scored in the same table: the best single constant config
   fitted on the training fold, a per-family lookup table, and a uniformly
   random feasible action.

3. **Does the number describe what we ship?** Two advisor choosers are scored
   side by side: ``advisor_argmin`` (enumerate the candidate actions, take the
   argmin of the predicted ``rel_err_rel`` head -- what ``evaluate.py`` has
   always reported) and ``advisor_policy`` (query the policy head once at the
   default action, decode, clamp, argmax -- what ``src/advisor/src/advisor.cpp``
   actually does in production).

4. **Is the number stable?** Every fold is trained at several seeds and the
   spread is reported, because a mean over a dozen held-out cases from one seed
   is not evidence.

Primary metric is regret **under a DOF budget**; see :mod:`advisor.regret` for
why an unconstrained accuracy oracle makes the problem degenerate.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import numpy as np
import torch

if __package__ in (None, ""):  # direct `python scripts/advisor/crossval.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from . import regret as R  # noqa: E402
from .dataset import (  # noqa: E402
    ADVISOR_DIR,
    CATEGORICAL_INDEX_COLUMNS,
    CONTINUOUS_ACTION_COLUMNS,
    CONTINUOUS_ACTION_DIMS,
    INPUT_COLUMNS,
    ORDER_CHOICES,
    SPLIT_MODES,
    AdvisorData,
    Split,
    group_of,
    load_dataset,
    provenance,
    split_groups,
)
from .model import AdvisorNet  # noqa: E402
from .train import (  # noqa: E402
    STAGE_A_WEIGHTS,
    PolicyObjective,
    SplitTensors,
    train_one_run,
)

REPORT_JSON = ADVISOR_DIR / "crossval.json"

#: Columns that make up an action, in a fixed order, used for the
#: nearest-measured-action match. Standardized, so a wide dial cannot dominate.
ACTION_COLUMNS: list[str] = CONTINUOUS_ACTION_COLUMNS + CATEGORICAL_INDEX_COLUMNS
ACTION_INDEX: list[int] = [INPUT_COLUMNS.index(name) for name in ACTION_COLUMNS]


# --------------------------------------------------------------------------- #
# per-split helpers
# --------------------------------------------------------------------------- #

def action_matrix(split: Split, cases: list[R.Case]) -> dict[str, np.ndarray]:
    """Standardized action vectors of every candidate row, keyed by case."""
    return {case.part: split.x[np.asarray(case.rows, dtype=int)][:, ACTION_INDEX].astype(np.float64)
            for case in cases}


def standardized_default_action(data: AdvisorData) -> np.ndarray:
    """The clamp-box default action, standardized into input space.

    Mirrors ``Impl::apply_action`` + ``Impl::encode`` in
    ``src/advisor/src/advisor.cpp``: only the six columns the C++ side actually
    writes are set, and every other action column keeps the imputed training
    median. That blindness is deployed behaviour, so reproducing it is the
    point -- scoring the policy with dials it never sets would flatter it.
    """
    mean = np.asarray(data.normalization["mean"], dtype=np.float64)
    std = np.asarray(data.normalization["std"], dtype=np.float64)
    impute = np.asarray(data.normalization["impute"], dtype=np.float64)
    raw = impute.copy()
    defaults = data.clamps["defaults"]
    raw[INPUT_COLUMNS.index("h_rel")] = float(defaults["h_rel"])
    raw[INPUT_COLUMNS.index("eta_target")] = float(defaults["eta_target"])
    raw[INPUT_COLUMNS.index("adapt_passes")] = float(defaults["adapt_passes"])
    # No p_elevate column: `order >= 2` is the p-elevation actuator.
    raw[INPUT_COLUMNS.index("order_idx")] = float(ORDER_CHOICES.index(int(defaults["order"])))
    raw[INPUT_COLUMNS.index("mesher_idx")] = float(
        data.mesher_choices.index(str(defaults["mesher"])))
    return (raw - mean) / std


def decode_policy(policy: np.ndarray, data: AdvisorData) -> np.ndarray:
    """Decode a policy vector into a standardized action, exactly as C++ does.

    ``advisor.cpp:420-432``: three clamped continuous dims, then an argmax over
    the order and mesher logit blocks. The policy is 3 + n_order + n_mesher
    wide; there is no p-elevate logit to sign-test.
    """
    mean = np.asarray(data.normalization["mean"], dtype=np.float64)
    std = np.asarray(data.normalization["std"], dtype=np.float64)
    impute = np.asarray(data.normalization["impute"], dtype=np.float64)
    clamps = data.clamps

    def clamp(value: float, interval: list[float]) -> float:
        return float(min(max(float(value), float(interval[0])), float(interval[1])))

    raw = impute.copy()
    raw[INPUT_COLUMNS.index("h_rel")] = clamp(policy[0], clamps["h_rel"])
    raw[INPUT_COLUMNS.index("adapt_passes")] = round(clamp(policy[1], clamps["adapt_passes"]))
    raw[INPUT_COLUMNS.index("eta_target")] = clamp(policy[2], clamps["eta_target"])

    order_begin = len(CONTINUOUS_ACTION_DIMS)
    n_order = len(data.order_choices)
    mesher_begin = order_begin + n_order
    raw[INPUT_COLUMNS.index("order_idx")] = float(
        int(np.argmax(policy[order_begin:order_begin + n_order])))
    raw[INPUT_COLUMNS.index("mesher_idx")] = float(
        int(np.argmax(policy[mesher_begin:mesher_begin + len(data.mesher_choices)])))
    return ((raw - mean) / std)[ACTION_INDEX]


@torch.no_grad()
def advisor_scores(net: AdvisorNet, split: Split, cases: list[R.Case],
                   data: AdvisorData) -> dict[str, dict[str, np.ndarray]]:
    """Everything the advisor-side choosers need, keyed by chooser input.

    ``rel_err_rel`` and ``efficiency`` are per-candidate scores; ``failure`` is
    the per-candidate feasibility probability the veto already computes but
    currently only uses as a single 0.5 gate; ``requested`` is the action the
    shipped policy head asks for, one per case.
    """
    net.eval()
    predicted = net(torch.from_numpy(np.ascontiguousarray(split.x, dtype=np.float32)))
    score = predicted["rel_err_rel"].numpy().reshape(-1).astype(np.float64)
    # The model already predicts both terms of the efficiency objective, so a
    # chooser that optimises accuracy-per-DOF needs no retraining, no new head
    # and no change to the ONNX contract -- only a different combination of
    # heads at query time. Both are log10, so the sum is log10(rel_err x DOF).
    efficiency = score + predicted["dof"].numpy().reshape(-1).astype(np.float64)
    failure = torch.sigmoid(predicted["failure_logit"]).numpy().reshape(-1).astype(np.float64)
    default_action = standardized_default_action(data)[ACTION_INDEX]
    scores: dict[str, np.ndarray] = {}
    eff: dict[str, np.ndarray] = {}
    fail: dict[str, np.ndarray] = {}
    requested: dict[str, np.ndarray] = {}
    for case in cases:
        rows = np.asarray(case.rows, dtype=int)
        scores[case.part] = score[rows]
        eff[case.part] = efficiency[rows]
        fail[case.part] = failure[rows]
        # Pass 1 of advisor.cpp:recommend -- the policy is queried at the
        # default action, so the query row is this case's context with the
        # default action substituted in.
        query = split.x[rows[0]].copy()
        query[ACTION_INDEX] = default_action
        out = net(torch.from_numpy(query.reshape(1, -1).astype(np.float32)))
        requested[case.part] = decode_policy(
            out["policy"].numpy().reshape(-1).astype(np.float64), data)
    return {"rel_err_rel": scores, "efficiency": eff, "failure": fail,
            "requested": requested}


# --------------------------------------------------------------------------- #
# one (fold, seed)
# --------------------------------------------------------------------------- #

def train_fold(data: AdvisorData, seed: int, epochs: int,
               batch_size: int, learning_rate: float) -> AdvisorNet:
    """Train one model from scratch. No warm start: a fold must be independent."""
    torch.manual_seed(seed)
    net = AdvisorNet.from_config(data.model_config(), seed=seed)
    optimizer = torch.optim.Adam(net.parameters(), lr=learning_rate)
    train_one_run(
        net, optimizer, SplitTensors(data.train), SplitTensors(data.val),
        dict(STAGE_A_WEIGHTS), PolicyObjective(data.mesher_choices),
        beta=1.0, run=seed, epochs=epochs, batch_size=batch_size,
    )
    return net


GATE_THRESHOLDS: tuple[float, ...] = (0.05, 0.1, 0.2, 0.35, 0.5, 0.8)


def build_choosers(net: AdvisorNet, data: AdvisorData, seed: int, objective: str,
                   budget_head: str = "dof", include_failures: bool = False,
                   ) -> tuple[dict[str, R.Chooser], list[R.Case], dict[str, Any]]:
    """Every chooser scored in one table, plus the held-out cases."""
    grouping = (lambda part: group_of(part, data.split_mode))
    val_cases = R.build_cases(data.val, grouping, include_failures=include_failures)
    train_cases = R.build_cases(data.train, grouping)
    val_actions = action_matrix(data.val, val_cases)
    train_actions = action_matrix(data.train, train_cases)

    predictions = advisor_scores(net, data.val, val_cases, data)
    scores = predictions["rel_err_rel"]
    efficiency = predictions["efficiency"]
    requested = predictions["requested"]
    risk = predictions["failure"]

    constant, constant_train_regret = R.fit_constant_action(train_cases, train_actions, objective)
    group_table = R.fit_group_actions(train_cases, train_actions, objective)
    lookup, hit_rate = R.group_lookup_chooser(group_table, constant, val_actions)

    choosers: dict[str, R.Chooser] = {
        "oracle": R.oracle_chooser(objective),
        "advisor_argmin": R.score_chooser(scores),
        "advisor_efficiency": R.score_chooser(efficiency),
        "advisor_policy": R.nearest_action_chooser(requested, val_actions),
        "default": R.fixed_action_chooser(
            standardized_default_action(data)[ACTION_INDEX], val_actions),
        "random": R.random_chooser(seed),
        "spend_budget": R.spend_budget_chooser(budget_head),
        "finest_action": R.finest_action_chooser(
            val_actions, ACTION_COLUMNS.index("h_rel")),
        "family_lookup": lookup,
    }
    if constant is not None:
        choosers["constant_config"] = R.fixed_action_chooser(constant, val_actions)

    # Feasibility-gated argmin: filter candidates by predicted failure risk, then
    # rank the survivors. Swept rather than fixed at the shipped 0.5, because the
    # calibration work measured mean ECE 0.4795 -- the head's probabilities do not
    # mean what they say, so the right operating point is an empirical question.
    for threshold in GATE_THRESHOLDS:
        choosers[f"advisor_gated_{threshold:g}"] = R.gated_score_chooser(
            scores, risk, threshold)

    meta = {
        "constant_train_regret": constant_train_regret,
        "family_lookup_hit_rate": hit_rate,
        "n_train_cases": len(train_cases),
    }
    return choosers, val_cases, meta


def run_fold_seed(csv: Path | None, split: str, fold: int, n_folds: int | None,
                  seed: int, args: argparse.Namespace) -> dict[str, Any]:
    data = load_dataset(csv, split=split, fold=fold, n_folds=n_folds)
    net = train_fold(data, seed, args.epochs, args.batch_size, args.learning_rate)
    choosers, cases, meta = build_choosers(net, data, seed, args.objective,
                                           args.budget_head, args.include_failures)

    result = R.score(cases, choosers, budget_head=args.budget_head,
                     bands=[(0.4, 0.6), (0.7, 0.9)],
                     allow_failed=args.include_failures)
    result["dof_to_target"] = R.dof_to_target(cases, choosers)
    result["fold"] = fold
    result["seed"] = seed
    result["split_mode"] = data.split_mode
    result["held_out_groups"] = list(data.val_groups)
    result["n_val_rows"] = data.val.n_rows
    result["family_lookup_hit_rate"] = meta["family_lookup_hit_rate"]()
    result["constant_train_regret"] = meta["constant_train_regret"]

    # Paired tests at the primary budget. `finest_action` is included because it
    # is the DEPLOYABLE trivial rule: `spend_budget` ranks by measured DOF and so
    # can never pick an action that failed, which is hindsight no shipped policy
    # could have. Beating spend_budget is not required; beating finest_action is.
    result["paired"] = {}
    gated = [name for name in choosers if name.startswith("advisor_gated_")]
    for challenger in ["advisor_argmin", "advisor_efficiency", "advisor_policy", *gated]:
        for reference in ("default", "constant_config", "finest_action", "spend_budget",
                          "advisor_argmin"):
            if reference not in choosers or reference == challenger:
                continue

            left, right = R.paired_regrets(
                cases, choosers[challenger], choosers[reference], args.objective,
                budget_head=args.budget_head, quantile=args.primary_quantile,
                allow_failed=args.include_failures)
            test = R.sign_test(left, right)
            test["mean_regret_challenger"] = float(np.mean(left)) if left else float("nan")
            test["mean_regret_reference"] = float(np.mean(right)) if right else float("nan")
            result["paired"][f"{challenger}_vs_{reference}"] = test
    return result


# --------------------------------------------------------------------------- #
# aggregation
# --------------------------------------------------------------------------- #

def aggregate(runs: list[dict[str, Any]], heads: list[str]) -> dict[str, Any]:
    """Macro-mean over folds of the per-seed mean, plus the seed spread.

    Macro over folds, never micro over cases: folds differ in size and a micro
    average would let the biggest family decide the headline.
    """
    out: dict[str, Any] = {"levels": {}}
    levels = sorted({label for run in runs for label in run["levels"]})
    for label in levels:
        head_block: dict[str, Any] = {}
        for head in heads:
            per_chooser: dict[str, list[float]] = defaultdict(list)
            seed_spread: dict[str, list[float]] = defaultdict(list)
            by_fold: dict[int, dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
            for run in runs:
                block = run["levels"].get(label, {}).get("heads", {}).get(head, {})
                for name, stats in block.items():
                    by_fold[run["fold"]][name].append(stats["mean_regret"])
            for fold, chooser_values in by_fold.items():
                for name, values in chooser_values.items():
                    per_chooser[name].append(float(np.mean(values)))
                    if len(values) > 1:
                        seed_spread[name].append(float(np.std(values, ddof=1)))
            head_block[head] = {
                name: {
                    "macro_mean_regret": float(np.mean(values)),
                    "fold_std": float(np.std(values, ddof=1)) if len(values) > 1 else 0.0,
                    "mean_seed_std": (float(np.mean(seed_spread[name]))
                                      if seed_spread.get(name) else 0.0),
                    "n_folds": len(values),
                }
                for name, values in sorted(per_chooser.items())
            }
        out["levels"][label] = head_block

    paired: dict[str, dict[str, Any]] = defaultdict(lambda: {"wins": 0, "losses": 0, "ties": 0})
    for run in runs:
        for key, test in run.get("paired", {}).items():
            for field in ("wins", "losses", "ties"):
                paired[key][field] += test[field]
    for key, totals in paired.items():
        totals["n_paired"] = totals["wins"] + totals["losses"]
        totals["p_value"] = R.sign_test(
            [0.0] * totals["wins"] + [1.0] * totals["losses"],
            [1.0] * totals["wins"] + [0.0] * totals["losses"])["p_value"]
    out["paired_pooled"] = dict(paired)
    return out


# --------------------------------------------------------------------------- #
# reporting
# --------------------------------------------------------------------------- #

def print_table(summary: dict[str, Any], head: str, label: str) -> None:
    block = summary["levels"].get(label, {}).get(head)
    if not block:
        return
    print(f"\n{head} regret, budget level {label} "
          f"(log10; 0 = chose the best feasible action)")
    print(f"{'chooser':>16} | {'macro mean':>10} {'+- fold':>8} {'+- seed':>8} | {'x worse':>8}")
    print("-" * 62)
    for name, stats in sorted(block.items(), key=lambda kv: kv[1]["macro_mean_regret"]):
        print(f"{name:>16} | {stats['macro_mean_regret']:>10.4f} "
              f"{stats['fold_std']:>8.4f} {stats['mean_seed_std']:>8.4f} | "
              f"{R.decades_to_factor(stats['macro_mean_regret']):>8.2f}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--csv", type=Path, default=None)
    parser.add_argument("--split", choices=list(SPLIT_MODES), default="family")
    parser.add_argument("--folds", default="all",
                        help="'all' or a comma-separated fold list")
    parser.add_argument("--n-folds", type=int, default=None)
    parser.add_argument("--seeds", type=int, default=5)
    parser.add_argument("--seed0", type=int, default=1234)
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--learning-rate", type=float, default=3e-3)
    parser.add_argument("--objective", default="rel_err",
                        help="outcome the choosers optimise and are scored on")
    parser.add_argument("--budget-head", default="dof", choices=R.BUDGET_HEADS)
    parser.add_argument("--include-failures", action="store_true",
                        help="offer actions the engine could not deliver, and charge a "
                             "chooser that picks one (also reports pick_failure_rate)")
    parser.add_argument("--primary-quantile", type=float, default=0.5,
                        help="budget level used for the paired sign tests")
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--out", type=Path, default=None)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    torch.set_num_threads(max(1, int(args.threads)))

    probe = load_dataset(args.csv, split=args.split, fold=0, n_folds=args.n_folds)
    n_groups = len(split_groups(probe.val.parts + probe.train.parts, args.split))
    total_folds = n_groups if args.n_folds is None else int(args.n_folds)
    folds = (list(range(total_folds)) if args.folds == "all"
             else [int(value) for value in args.folds.split(",")])
    seeds = [args.seed0 + i for i in range(max(1, args.seeds))]

    print(f"dataset   : {probe.csv_path} ({probe.n_rows} rows)")
    print(f"split     : {args.split}, {total_folds} folds x {len(seeds)} seeds "
          f"= {len(folds) * len(seeds)} models")
    print(f"objective : {args.objective}   budget axis: {args.budget_head}")

    runs: list[dict[str, Any]] = []
    for fold in folds:
        for seed in seeds:
            run = run_fold_seed(args.csv, args.split, fold, args.n_folds, seed, args)
            runs.append(run)
            primary = run["levels"].get(f"q{args.primary_quantile:g}", {})
            block = primary.get("heads", {}).get(args.objective, {})
            adv = block.get("advisor_argmin", {}).get("mean_regret", float("nan"))
            con = block.get("constant_config", {}).get("mean_regret", float("nan"))
            n_cases = run["levels"]["unconstrained"]["n_cases"]
            print(f"  fold {fold} ({','.join(run['held_out_groups'])}) seed {seed}: "
                  f"{n_cases} cases, "
                  f"advisor_argmin {adv:.4f} vs constant {con:.4f} @q{args.primary_quantile:g}")
            if n_cases == 0:
                print(f"    WARNING fold {fold} contributes nothing: no held-out case has "
                      f"{R.MIN_ACTIONS}+ measured actions. Every macro mean below is over "
                      f"the remaining folds, and this fold's family is unmeasured.")

    summary = aggregate(runs, [args.objective] + [h for h in R.SCORED_HEADS
                                                  if h != args.objective])
    payload = {
        "provenance": provenance(probe, seed=args.seed0, epochs=args.epochs,
                                 seeds=len(seeds), include_failures=args.include_failures),
        "split_mode": args.split,
        "n_folds": total_folds,
        "folds": folds,
        "seeds": seeds,
        "objective": args.objective,
        "budget_head": args.budget_head,
        "epochs": args.epochs,
        "summary": summary,
        "runs": runs,
    }
    out = args.out or REPORT_JSON
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    for label in ("unconstrained", f"q{args.primary_quantile:g}", "q0.25"):
        print_table(summary, args.objective, label)

    print("\npaired sign tests, pooled over folds and seeds "
          f"(budget q{args.primary_quantile:g}, {args.objective})")
    for key, totals in sorted(summary["paired_pooled"].items()):
        print(f"  {key:>38}: {totals['wins']}W-{totals['losses']}L-{totals['ties']}T "
              f"p={totals['p_value']:.4f}")

    print(f"\nwrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
