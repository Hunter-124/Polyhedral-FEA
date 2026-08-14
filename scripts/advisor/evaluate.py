#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Does the advisor actually choose a better mesh than the alternatives?

Every other metric in this project scores a *prediction*. This one scores a
*decision*, which is the only thing the advisor exists to make.

For each held-out case the campaign ran a known set of actions, so the best
achievable outcome for that case is known exactly. Regret is how much worse the
chosen action is than that best, in log10 units, so 0.30 of regret means
"0.30 decades = 2x worse than the best mesh this case could have had".

Two properties of this script matter more than the numbers it prints:

* **It scores the shipped rule.** ``advisor_policy`` reproduces what
  ``src/advisor/src/advisor.cpp`` does in production -- query the policy head
  once at the clamp-box default action, decode, clamp, argmax -- next to
  ``advisor_argmin``, which enumerates the candidates and takes the argmin of
  the predicted ``rel_err_rel`` head. Those are different policies and only one
  of them is deployed, so reporting only the second would describe software we
  do not ship.

* **The baselines include a zero-parameter one.** A learned policy that cannot
  beat the single best constant configuration has not earned its weights.

This is the one-checkpoint view. For the leakage-safe, multi-fold, multi-seed
view with variance -- which is what any headline number should come from -- use
``scripts/advisor/crossval.py``. Both share :mod:`advisor.regret`, so they
cannot drift into reporting different things.

    python scripts/advisor/evaluate.py
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np
import torch

if __package__ in (None, ""):  # direct `python scripts/advisor/evaluate.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from . import regret as R  # noqa: E402
from .crossval import build_choosers  # noqa: E402
from .dataset import ADVISOR_DIR, add_split_args, group_of, load_from_args  # noqa: E402
from .model import AdvisorNet  # noqa: E402

#: ``latest.pt`` is the trainer's resume point — the LAST run, not the best
#: one. Evaluation reports what would ship, so it reads ``best.pt`` (the best
#: validation ``rel_err_rel`` of the current stage) whenever the trainer has
#: written one, and only falls back to the resume point for old run
#: directories that predate best-checkpoint selection.
LATEST_CHECKPOINT = ADVISOR_DIR / "runs" / "latest.pt"
BEST_CHECKPOINT = ADVISOR_DIR / "runs" / "best.pt"
REPORT_JSON = ADVISOR_DIR / "action_selection.json"


def default_checkpoint() -> Path:
    return BEST_CHECKPOINT if BEST_CHECKPOINT.is_file() else LATEST_CHECKPOINT


def spearman(a: np.ndarray, b: np.ndarray) -> float:
    """Rank correlation without a scipy dependency."""
    if a.size < 2:
        return float("nan")
    ra = np.argsort(np.argsort(a)).astype(np.float64)
    rb = np.argsort(np.argsort(b)).astype(np.float64)
    ra -= ra.mean()
    rb -= rb.mean()
    denom = float(np.sqrt((ra * ra).sum() * (rb * rb).sum()))
    return float((ra * rb).sum() / denom) if denom > 0.0 else float("nan")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", default=None)
    parser.add_argument("--csv", default=None)
    add_split_args(parser)
    parser.add_argument("--objective", default="rel_err")
    parser.add_argument("--budget-head", default="dof", choices=R.BUDGET_HEADS)
    parser.add_argument("--out", default=None)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    torch.set_num_threads(args.threads)

    checkpoint = Path(args.checkpoint) if args.checkpoint else default_checkpoint()
    if not checkpoint.is_file():
        raise SystemExit(f"no checkpoint at {checkpoint}; run scripts/advisor/train.py first")
    payload = torch.load(checkpoint, map_location="cpu", weights_only=False)
    net = AdvisorNet.from_config(payload["config"])
    net.load_state_dict(payload["model"])
    net.eval()

    data = load_from_args(args)
    choosers, cases, meta = build_choosers(net, data, seed=0, objective=args.objective,
                                           budget_head=args.budget_head)
    if not cases:
        raise SystemExit(
            f"no held-out case in split {data.split_mode} fold {data.fold} has "
            f"{R.MIN_ACTIONS}+ measured actions; nothing to score"
        )

    results: dict[str, Any] = R.score(cases, choosers, budget_head=args.budget_head)
    results["dof_to_target"] = R.dof_to_target(cases, choosers)
    results["cost_at_tolerance"] = R.cost_at_tolerance(
        cases, choosers, cost_head=args.budget_head)
    results["split_mode"] = data.split_mode
    results["fold"] = data.fold
    results["n_folds"] = data.n_folds
    results["held_out_groups"] = list(data.val_groups)
    results["family_lookup_hit_rate"] = meta["family_lookup_hit_rate"]()

    # Ranking quality of the enumerating chooser, kept because a model that
    # ranks the whole action set is worth more than one that only finds a good
    # top pick -- but regret, not rho, is what the product depends on.
    rhos: list[float] = []
    scores = {case.part: None for case in cases}
    with torch.no_grad():
        predicted = net(torch.from_numpy(data.val.x))["rel_err_rel"].numpy().reshape(-1)
    for case in cases:
        rows = np.asarray(case.rows, dtype=int)
        rho = spearman(predicted[rows].astype(np.float64), case.outcomes["rel_err"])
        if np.isfinite(rho):
            rhos.append(rho)
    results["mean_spearman"] = float(np.mean(rhos)) if rhos else float("nan")

    results["paired"] = {}
    for challenger in ("advisor_argmin", "advisor_policy"):
        for reference in ("default", "constant_config"):
            if reference not in choosers:
                continue
            left, right = R.paired_regrets(cases, choosers[challenger], choosers[reference],
                                           args.objective, budget_head=args.budget_head,
                                           quantile=0.5)
            results["paired"][f"{challenger}_vs_{reference}"] = R.sign_test(left, right)

    out = Path(args.out) if args.out else REPORT_JSON
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")

    print(f"split                 : {data.split_mode} fold {data.fold}/{data.n_folds}, "
          f"holding out {', '.join(data.val_groups)}")
    print(f"held-out cases scored : {results['n_cases']}")
    print(f"mean Spearman rho     : {results['mean_spearman']:.3f}"
          "   (advisor_argmin ordering vs actual, within case)")
    print(f"family_lookup coverage: {results['family_lookup_hit_rate']:.0%} of cases had their "
          "family in train")

    for label in ("unconstrained", "q0.5", "q0.25"):
        block = results["levels"].get(label, {})
        head_block = block.get("heads", {}).get(args.objective)
        if not head_block:
            continue
        print(f"\n{args.objective} regret at budget {label} "
              f"({args.budget_head} axis, n={block['n_cases']} cases)")
        print(f"{'chooser':>16} | {'mean':>8} {'median':>8} | {'x worse':>8}")
        print("-" * 50)
        for name, stats in sorted(head_block.items(), key=lambda kv: kv[1]["mean_regret"]):
            print(f"{name:>16} | {stats['mean_regret']:>8.4f} "
                  f"{stats['median_regret']:>8.4f} | "
                  f"{R.decades_to_factor(stats['mean_regret']):>8.2f}")

    print("\npaired sign tests (budget q0.5)")
    for key, test in sorted(results["paired"].items()):
        print(f"  {key:>38}: {test['wins']}W-{test['losses']}L-{test['ties']}T "
              f"p={test['p_value']:.4f}")

    print("\nactive DOF to reach a relative-error target "
          "(reach rate first: spending less while reaching the target less often is not a win)")
    for target, rows in results["dof_to_target"].items():
        usable = {k: v for k, v in rows.items() if v["attempted"]}
        if not usable:
            continue
        print(f"  {target}:")
        ranked = sorted(usable.items(),
                        key=lambda kv: (-kv[1]["reach_rate"], kv[1]["median_log10_dof"]))
        for name, stats in ranked:
            median = stats["median_log10_dof"]
            spend = f"{10 ** median:>9.0f} DOF" if median == median else "  censored"
            print(f"    {name:>16}: reached {stats['reached']}/{stats['attempted']} "
                  f"({stats['reach_rate']:>4.0%})  median {spend}")

    print("\ncheapest mesh meeting a relative-error tolerance "
          "(violation rate first: a cheap mesh that misses the tolerance is not a saving)")
    for target, rows in results["cost_at_tolerance"].items():
        usable = {k: v for k, v in rows.items() if v["attempted"]}
        if not usable:
            continue
        print(f"  {target}  ({next(iter(usable.values()))['attempted']} reachable cases):")
        ranked = sorted(usable.items(),
                        key=lambda kv: (kv[1]["violation_rate"], kv[1]["mean_cost_regret"]))
        for name, stats in ranked:
            regret = stats["mean_cost_regret"]
            spend = (f"{R.decades_to_factor(regret):>5.2f}x cheapest"
                     if regret == regret else "   no satisfying pick")
            print(f"    {name:>20}: violated {stats['violation_rate']:>5.1%}  "
                  f"satisfied {stats['satisfied']:>3}/{stats['attempted']}  {spend}")

    print("\nRegret in log10 units; 0 = chose the best feasible action the campaign ran.")
    print("Budget levels are quantiles of each case's own candidate cost distribution.")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
