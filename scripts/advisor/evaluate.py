#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Does the advisor actually choose a better mesh than the default?

Every other metric in this project scores a *prediction*. This one scores a
*decision*, which is the only thing the advisor exists to make.

For each held-out case the campaign ran a known set of actions, so the best
achievable outcome for that case is known exactly. Regret is how much worse the
chosen action is than that best:

    regret(chooser) = value(chooser's action) - min over actions of value

measured in log10 units, so 0.30 of regret means "0.30 decades = 2x worse than
the best mesh this case could have had". Three choosers are compared:

* **advisor** -- the action minimizing the model's predicted `rel_err_rel`.
* **default** -- the clamp-box default action, i.e. what shipping without an
  advisor gets you. This is the bar to beat.
* **oracle**  -- the true best action; regret 0 by definition, shown to make the
  scale readable.

Ranking quality is reported alongside as Spearman rho between the predicted and
actual ordering of a case's actions, averaged over cases.

    python scripts/advisor/evaluate.py
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

if __package__ in (None, ""):  # direct `python scripts/advisor/evaluate.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import ADVISOR_DIR, load_dataset  # noqa: E402
from .model import AdvisorNet  # noqa: E402

LATEST_CHECKPOINT = ADVISOR_DIR / "runs" / "latest.pt"
REPORT_JSON = ADVISOR_DIR / "action_selection.json"

#: Outcome columns a chooser can be scored on, and whether lower is better.
SCORED_HEADS = ["rel_err", "geo_p99", "solve_ms"]


def spearman(a: np.ndarray, b: np.ndarray) -> float:
    """Rank correlation without a scipy dependency."""
    if a.size < 3:
        return float("nan")

    def ranks(v: np.ndarray) -> np.ndarray:
        order = np.argsort(v, kind="stable")
        out = np.empty_like(order, dtype=np.float64)
        out[order] = np.arange(v.size, dtype=np.float64)
        return out

    ra, rb = ranks(a), ranks(b)
    ra -= ra.mean()
    rb -= rb.mean()
    denom = float(np.sqrt((ra * ra).sum() * (rb * rb).sum()))
    return float((ra * rb).sum() / denom) if denom > 0.0 else float("nan")


def default_row(indices: list[int], x: np.ndarray, columns: list[str],
                clamps: dict[str, Any], normalization: dict[str, Any]) -> int:
    """The case's row closest to the clamp-box default action.

    The default action is not guaranteed to be one of the grid points the
    campaign ran, so the nearest one in standardized action space is used and
    the distance is reported by the caller.
    """
    defaults = clamps["defaults"]
    mean = np.asarray(normalization["mean"], dtype=np.float64)
    std = np.asarray(normalization["std"], dtype=np.float64)
    wanted = {
        "h_rel": float(defaults["h_rel"]),
        "eta_target": float(defaults["eta_target"]),
        "adapt_passes": float(defaults["adapt_passes"]),
        "p_elevate": 1.0 if defaults["p_elevate"] else 0.0,
        "order_idx": float(normalization["order_choices"].index(defaults["order"])),
        "mesher_idx": float(normalization["mesher_choices"].index(defaults["mesher"])),
    }
    cols = {name: i for i, name in enumerate(columns)}
    best, best_d = indices[0], float("inf")
    for i in indices:
        d = 0.0
        for name, target in wanted.items():
            c = cols.get(name)
            if c is None:
                continue
            d += (float(x[i, c]) - (target - mean[c]) / std[c]) ** 2
        if d < best_d:
            best, best_d = i, d
    return best


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", default=None)
    parser.add_argument("--out", default=None)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    torch.set_num_threads(args.threads)

    checkpoint = Path(args.checkpoint) if args.checkpoint else LATEST_CHECKPOINT
    if not checkpoint.is_file():
        raise SystemExit(f"no checkpoint at {checkpoint}; run scripts/advisor/train.py first")
    payload = torch.load(checkpoint, map_location="cpu", weights_only=False)
    net = AdvisorNet.from_config(payload["config"])
    net.load_state_dict(payload["model"])
    net.eval()

    data = load_dataset()
    split = data.val
    with torch.no_grad():
        predicted = net(torch.from_numpy(split.x))
    score = predicted["rel_err_rel"].numpy().reshape(-1)

    by_case: dict[str, list[int]] = defaultdict(list)
    for i, part in enumerate(split.parts):
        by_case[part].append(i)

    results: dict[str, Any] = {"cases": [], "heads": {}}
    per_head: dict[str, dict[str, list[float]]] = {
        h: {"advisor": [], "default": [], "oracle": []} for h in SCORED_HEADS
    }
    rhos: list[float] = []

    for part, rows in sorted(by_case.items()):
        usable = [i for i in rows if split.masks["rel_err"][i]]
        if len(usable) < 3:
            continue
        truth = split.targets["rel_err"][usable].astype(np.float64)
        pred = score[usable].astype(np.float64)
        rho = spearman(pred, truth)
        if np.isfinite(rho):
            rhos.append(rho)

        chosen = usable[int(np.argmin(pred))]
        oracle = usable[int(np.argmin(truth))]
        fallback = default_row(usable, split.x, data.input_columns, data.clamps,
                               data.normalization)

        entry: dict[str, Any] = {
            "part": part, "n_actions": len(usable), "spearman": rho,
            "advisor_cfg": split.cfg_ids[chosen], "default_cfg": split.cfg_ids[fallback],
            "oracle_cfg": split.cfg_ids[oracle],
        }
        for head in SCORED_HEADS:
            mask = split.masks[head]
            if not (mask[chosen] and mask[fallback] and mask[oracle]):
                continue
            values = split.targets[head]
            best = float(min(values[i] for i in usable if mask[i]))
            for name, index in (("advisor", chosen), ("default", fallback), ("oracle", oracle)):
                per_head[head][name].append(float(values[index]) - best)
            entry[f"{head}_regret_advisor"] = float(values[chosen]) - best
            entry[f"{head}_regret_default"] = float(values[fallback]) - best
        results["cases"].append(entry)

    results["mean_spearman"] = float(np.mean(rhos)) if rhos else float("nan")
    results["n_cases"] = len(results["cases"])
    for head, choosers in per_head.items():
        if not choosers["advisor"]:
            continue
        results["heads"][head] = {
            name: {"mean_regret": float(np.mean(v)), "median_regret": float(np.median(v))}
            for name, v in choosers.items()
        }
        adv = float(np.mean(choosers["advisor"]))
        dfl = float(np.mean(choosers["default"]))
        results["heads"][head]["improvement_vs_default"] = dfl - adv
        results["heads"][head]["beats_default"] = bool(adv < dfl)

    out = Path(args.out) if args.out else REPORT_JSON
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")

    print(f"held-out cases scored : {results['n_cases']}")
    print(f"mean Spearman rho     : {results['mean_spearman']:.3f}"
          "   (predicted vs actual action ordering, within case)")
    print()
    print(f"{'outcome':>10} | {'advisor':>9} {'default':>9} {'oracle':>7} | {'gain':>8}")
    print("-" * 54)
    for head, block in results["heads"].items():
        print(f"{head:>10} | {block['advisor']['mean_regret']:>9.4f} "
              f"{block['default']['mean_regret']:>9.4f} "
              f"{block['oracle']['mean_regret']:>7.4f} | "
              f"{block['improvement_vs_default']:>+8.4f}")
    print()
    print("Mean regret in log10 units; 0 = chose the best action the campaign ran.")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
