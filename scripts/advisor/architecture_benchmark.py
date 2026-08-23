#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Paired family-held-out promotion gate for the proposed branch advisor.

The production graph remains the shared trunk unless this experiment wins the
pre-registered gate: lower rel_err_rel MAE at one-sided paired sign-test p<0.05,
no regression of the absolute accuracy heads, and lower MAE on at least one
portable cost head. A tie leaves production unchanged.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np
import torch
from torch import Tensor, nn

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import ACCURACY_HEADS, COST_HEADS, REGRESSION_HEADS, load_dataset
from .model import AdvisorNet
from .train import (BatchView, PolicyObjective, SplitTensors, compute_loss,
                    evaluate)

BRANCH_WIDTHS = {"accuracy": 12, "geometry": 12, "cost": 16, "feasibility": 8}


class BranchAdvisorNet(nn.Module):
    def __init__(self, config: dict[str, Any]) -> None:
        super().__init__()
        self.shared = AdvisorNet(config)
        hidden = int(config.get("hidden", 96))
        self.branches = nn.ModuleDict(
            {name: nn.Sequential(nn.Linear(hidden, width), nn.GELU())
             for name, width in BRANCH_WIDTHS.items()}
        )
        groups = {
            **{head: "accuracy" for head in ("rel_err", "rel_err_rel")},
            **{head: "geometry" for head in ("geo_chamfer", "geo_p99")},
            **{head: "cost" for head in COST_HEADS},
        }
        self.groups = groups
        self.regression_heads = nn.ModuleDict(
            {head: nn.Linear(BRANCH_WIDTHS[group], 1) for head, group in groups.items()}
        )
        self.failure_head = nn.Linear(BRANCH_WIDTHS["feasibility"], 1)

    def forward(self, value: Tensor) -> dict[str, Tensor]:
        hidden = self.shared.trunk(value)[2]
        branch = {name: layer(hidden) for name, layer in self.branches.items()}
        outputs = {
            head: layer(branch[self.groups[head]])
            for head, layer in self.regression_heads.items()
        }
        outputs["failure_logit"] = self.failure_head(branch["feasibility"])
        outputs["policy"] = self.shared.policy_head(hidden)
        return outputs


def weights() -> dict[str, float]:
    out = {head: 0.0 for head in REGRESSION_HEADS}
    out.update({"rel_err": 0.25, "rel_err_rel": 1.0,
                "geo_chamfer": 0.5, "geo_p99": 0.5,
                "solve_flops": 0.5, "solve_bytes": 0.25, "mesh_work": 0.25,
                "failure": 1.0, "policy": 0.3})
    return out


def train_model(model: nn.Module, data: Any, seed: int, epochs: int,
                batch_size: int, device: torch.device) -> dict[str, float]:
    torch.manual_seed(seed)
    model.to(device)
    train = SplitTensors(data.train, device)
    val = SplitTensors(data.val, device)
    objective = PolicyObjective(data.mesher_choices)
    optimizer = torch.optim.AdamW(model.parameters(), lr=3e-3, weight_decay=1e-4)
    run_weights = weights()
    generator = torch.Generator().manual_seed(seed)
    for _ in range(epochs):
        model.train()
        order = torch.randperm(train.n_rows, generator=generator).to(device)
        for start in range(0, train.n_rows, batch_size):
            batch = train.batch(order[start:start + batch_size])
            optimizer.zero_grad(set_to_none=True)
            loss, _ = compute_loss(model(batch.x), batch, run_weights, objective, 1.0)
            loss.backward()
            optimizer.step()
    return evaluate(model, val, run_weights, objective, 1.0)


def sign_test_paired(shared: list[float], branch: list[float]) -> dict[str, float | int]:
    wins = sum(b < s for s, b in zip(shared, branch) if math.isfinite(s) and math.isfinite(b))
    losses = sum(b > s for s, b in zip(shared, branch) if math.isfinite(s) and math.isfinite(b))
    n = wins + losses
    p = (sum(math.comb(n, k) for k in range(wins, n + 1)) / (2 ** n)) if n else 1.0
    return {"wins": wins, "losses": losses, "ties": len(shared) - n, "p_one_sided": p}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=Path("bench/advisor/dataset.csv"))
    parser.add_argument("--folds", type=int, default=12)
    parser.add_argument("--seeds", type=int, default=5)
    parser.add_argument("--epochs", type=int, default=12)
    parser.add_argument("--batch-size", type=int, default=2048)
    parser.add_argument("--out", type=Path,
                        default=Path("bench/advisor/evidence/architecture_benchmark.json"))
    args = parser.parse_args()
    if not torch.cuda.is_available():
        raise SystemExit("CUDA is required for the paired architecture gate")
    device = torch.device("cuda")
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.set_float32_matmul_precision("high")

    rows: list[dict[str, Any]] = []
    started = time.perf_counter()
    for fold in range(args.folds):
        data = load_dataset(args.csv, split="family", fold=fold, n_folds=args.folds)
        for seed_index in range(args.seeds):
            seed = 1234 + 1000 * fold + seed_index
            torch.manual_seed(seed)
            shared = AdvisorNet(data.model_config())
            shared_metrics = train_model(
                shared, data, seed, args.epochs, args.batch_size, device)
            del shared
            torch.cuda.empty_cache()
            torch.manual_seed(seed)
            branch = BranchAdvisorNet(data.model_config())
            branch_metrics = train_model(
                branch, data, seed, args.epochs, args.batch_size, device)
            del branch
            torch.cuda.empty_cache()
            rows.append({"fold": fold, "seed": seed,
                         "shared": shared_metrics, "branch": branch_metrics})
            print(f"fold {fold + 1}/{args.folds} seed {seed_index + 1}/{args.seeds}: "
                  f"rel shared={shared_metrics['rel_err_rel_mae']:.4g} "
                  f"branch={branch_metrics['rel_err_rel_mae']:.4g}")

    shared_accuracy = [row["shared"]["rel_err_rel_mae"] for row in rows]
    branch_accuracy = [row["branch"]["rel_err_rel_mae"] for row in rows]
    sign_test = sign_test_paired(shared_accuracy, branch_accuracy)
    def finite_mean(values: list[float]) -> float:
        finite = [value for value in values if math.isfinite(value)]
        return float(np.mean(finite)) if finite else math.nan

    mean_mae = {
        head: {
            "shared": finite_mean([row["shared"][f"{head}_mae"] for row in rows]),
            "branch": finite_mean([row["branch"][f"{head}_mae"] for row in rows]),
        }
        for head in REGRESSION_HEADS
    }
    accuracy_no_regression = all(
        mean_mae[head]["branch"] <= mean_mae[head]["shared"]
        for head in ACCURACY_HEADS
    )
    portable_cost_win = any(
        mean_mae[head]["branch"] < mean_mae[head]["shared"]
        for head in ("solve_flops", "solve_bytes", "mesh_work")
        if math.isfinite(mean_mae[head]["shared"])
    )
    promote = (sign_test["p_one_sided"] < 0.05 and accuracy_no_regression
               and portable_cost_win)
    document = {
        "schema": "polymesh.advisor.architecture-benchmark/1",
        "folds": args.folds,
        "seeds": args.seeds,
        "epochs": args.epochs,
        "branch_widths": BRANCH_WIDTHS,
        "sign_test_rel_err_rel": sign_test,
        "mean_mae": mean_mae,
        "accuracy_no_regression": accuracy_no_regression,
        "portable_cost_win": portable_cost_win,
        "promote_branch_architecture": promote,
        "decision": "promote" if promote else "keep-shared-trunk",
        "wall_seconds": time.perf_counter() - started,
        "rows": rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print("decision:", document["decision"])
    print("wrote", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
