#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Staged multi-objective training for the learned mesh advisor (C7).

Each invocation performs ``--runs N`` *training runs*. A run is a warm-started
optimisation pass over the train split that ends by appending one record to
``bench/advisor/runs/history.jsonl`` and writing
``runs/<NNN>/{metrics.json,checkpoint.pt,activations.json}`` plus refreshing
``runs/latest.pt``.

Objectives are kept linearly separate: one masked Huber loss per regression
head in log10 space, a BCE failure head, a behaviour-cloning policy head, and
an explicit guardrail barrier — never one collapsed scalar.

    Stage A : rel_err, geo_chamfer, geo_p99, failure, policy
    Stage B : additionally ramps dof / mesh_ms / solve_ms from 0 to their
              target weight linearly over ``ramp_runs`` runs

The A -> B transition fires when the validation ``rel_err_mae`` has stopped
improving: best of the last 10 runs vs best of the 10 before is < 2 %.

Usage
-----
    python scripts/advisor/train.py --runs 3
    python scripts/advisor/train.py --baseline
    python scripts/advisor/train.py --self-test
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
import torch
import torch.nn.functional as F

if __package__ in (None, ""):  # direct `python scripts/advisor/train.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import (
    ADVISOR_DIR,
    CLAMPS_JSON,
    COST_HEADS,
    NORMALIZATION_JSON,
    REGRESSION_HEADS,
    AdvisorData,
    Split,
    action_group_slices,
    continuous_box_halfwidths,
    load_dataset,
    write_json,
)
from .model import AdvisorNet
from .prune import keep_mask, load_pruned, prune_run

RUNS_DIR = ADVISOR_DIR / "runs"
HISTORY_JSONL = RUNS_DIR / "history.jsonl"
LATEST_CHECKPOINT = RUNS_DIR / "latest.pt"
WEIGHTS_JSON = ADVISOR_DIR / "weights.json"
BASELINE_JSON = ADVISOR_DIR / "baseline_metrics.json"

SEED = 1234
HUBER_DELTA = 1.0

# `rel_err_rel` carries the weight the absolute head used to: measured, the
# absolute level of rel_err does not generalize across parts (val MAE ~1.0 for
# both this net and LightGBM, at every capacity from 2.5k to 811k parameters),
# while the per-case-centred version reaches ~0.30. The absolute head is kept
# at a low weight because it is still what reports a human-readable predicted
# error; the centred head is what ranks actions.
STAGE_A_WEIGHTS: dict[str, float] = {
    "rel_err": 0.25,
    "rel_err_rel": 1.0,
    "geo_chamfer": 0.5,
    "geo_p99": 0.5,
    "dof": 0.0,
    "mesh_ms": 0.0,
    "solve_ms": 0.0,
    "failure": 1.0,
    "policy": 0.3,
}
STAGE_B_TARGETS: dict[str, float] = {"dof": 0.25, "mesh_ms": 0.25, "solve_ms": 0.5}

DEFAULT_WEIGHTS: dict[str, Any] = {
    "stage_a": dict(STAGE_A_WEIGHTS),
    "stage_b_targets": dict(STAGE_B_TARGETS),
    "beta": 1.0,
    "ramp_runs": 5,
    "plateau_window": 10,
    "plateau_rel_improvement": 0.02,
}

METRIC_KEYS = [f"{head}_mae" for head in REGRESSION_HEADS] + [
    "failure_bce", "failure_acc", "failure_auc", "policy_mse", "total_loss",
]


# --------------------------------------------------------------------------- #
# small json helpers
# --------------------------------------------------------------------------- #

def jsonable(value: Any) -> Any:
    """Recursively replace non-finite floats with ``None`` (valid JSON)."""
    if isinstance(value, dict):
        return {key: jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [jsonable(item) for item in value]
    if isinstance(value, (np.floating, float)):
        number = float(value)
        return number if math.isfinite(number) else None
    if isinstance(value, (np.integer,)):
        return int(value)
    if isinstance(value, (np.bool_,)):
        return bool(value)
    return value


def dump_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        json.dump(jsonable(payload), stream, indent=2, allow_nan=False)
        stream.write("\n")


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# --------------------------------------------------------------------------- #
# weights.json
# --------------------------------------------------------------------------- #

def load_weights_config(path: Path | None = None) -> dict[str, Any]:
    """Read ``weights.json``, creating it with the Stage A defaults on first run."""
    target = Path(path) if path is not None else WEIGHTS_JSON
    if not target.is_file():
        write_json(target, DEFAULT_WEIGHTS)
        return json.loads(json.dumps(DEFAULT_WEIGHTS))
    with target.open("r", encoding="utf-8") as stream:
        payload = json.load(stream)
    config = json.loads(json.dumps(DEFAULT_WEIGHTS))
    config["stage_a"].update(payload.get("stage_a", {}))
    config["stage_b_targets"].update(payload.get("stage_b_targets", {}))
    for key in ("beta", "ramp_runs", "plateau_window", "plateau_rel_improvement"):
        if key in payload:
            config[key] = payload[key]
    return config


# --------------------------------------------------------------------------- #
# history / staging
# --------------------------------------------------------------------------- #

def read_history(path: Path | None = None) -> list[dict[str, Any]]:
    target = Path(path) if path is not None else HISTORY_JSONL
    if not target.is_file():
        return []
    records: list[dict[str, Any]] = []
    with target.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


def append_history(record: dict[str, Any], path: Path | None = None) -> None:
    target = Path(path) if path is not None else HISTORY_JSONL
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("a", encoding="utf-8") as stream:
        json.dump(jsonable(record), stream, allow_nan=False)
        stream.write("\n")


def val_rel_err_series(records: list[dict[str, Any]]) -> list[float]:
    """Validation ``rel_err_mae`` per run, skipping runs where it is undefined."""
    series: list[float] = []
    for record in records:
        value = (record.get("val") or {}).get("rel_err_mae")
        if isinstance(value, (int, float)) and math.isfinite(float(value)):
            series.append(float(value))
    return series


def plateau_reached(series: list[float], window: int = 10,
                    rel_improvement: float = 0.02) -> bool:
    """True when the metric has stopped improving.

    Compares the best of the last ``window`` runs against the best of the
    ``window`` runs before them. Fewer than ``2 * window`` usable runs of
    history is never a plateau.
    """
    if window <= 0 or len(series) < 2 * window:
        return False
    recent = series[-window:]
    previous = series[-2 * window:-window]
    best_recent = min(recent)
    best_previous = min(previous)
    if not math.isfinite(best_recent) or not math.isfinite(best_previous):
        return False
    if best_previous <= 0.0:
        return False
    relative = (best_previous - best_recent) / best_previous
    return relative < rel_improvement


def stage_for_run(records: list[dict[str, Any]], config: dict[str, Any]) -> tuple[str, int | None, bool]:
    """Stage of the *next* run.

    Returns ``(stage, stage_b_entry_run, transition)`` where ``transition`` is
    True only on the run that flips A -> B.
    """
    entry: int | None = None
    for record in records:
        if str(record.get("stage", "A")).upper() == "B":
            entry = int(record.get("run", len(records)))
            break
    next_run = len(records) + 1
    if entry is not None:
        return "B", entry, False
    if plateau_reached(val_rel_err_series(records),
                       int(config.get("plateau_window", 10)),
                       float(config.get("plateau_rel_improvement", 0.02))):
        return "B", next_run, True
    return "A", None, False


def head_weights_for(stage: str, run: int, entry_run: int | None,
                     config: dict[str, Any]) -> dict[str, float]:
    """Per-head loss weights, with the Stage B cost-head ramp applied."""
    weights = {key: float(value) for key, value in config["stage_a"].items()}
    if stage != "B":
        return weights
    ramp_runs = max(1, int(config.get("ramp_runs", 5)))
    start = int(entry_run if entry_run is not None else run)
    progress = min(1.0, max(0.0, (run - start + 1) / ramp_runs))
    for head in COST_HEADS:
        weights[head] = progress * float(config["stage_b_targets"].get(head, 0.0))
    return weights


# --------------------------------------------------------------------------- #
# tensors
# --------------------------------------------------------------------------- #

class SplitTensors:
    """Torch view of a :class:`~advisor.dataset.Split`."""

    def __init__(self, split: Split) -> None:
        self.split = split
        self.x = torch.from_numpy(np.ascontiguousarray(split.x, dtype=np.float32))
        self.targets = {
            head: torch.from_numpy(np.ascontiguousarray(values, dtype=np.float32))
            for head, values in split.targets.items()
        }
        self.masks = {
            head: torch.from_numpy(np.ascontiguousarray(values, dtype=bool))
            for head, values in split.masks.items()
        }
        self.failure = torch.from_numpy(np.ascontiguousarray(split.failure, dtype=np.float32))
        self.policy_target = torch.from_numpy(
            np.ascontiguousarray(split.policy_target, dtype=np.float32))
        self.policy_mask = torch.from_numpy(
            np.ascontiguousarray(split.policy_mask, dtype=bool))

    @property
    def n_rows(self) -> int:
        return int(self.x.shape[0])

    def batch(self, index: torch.Tensor) -> "BatchView":
        return BatchView(self, index)


class BatchView:
    """A row subset of :class:`SplitTensors` (no copies of the split itself)."""

    def __init__(self, source: SplitTensors, index: torch.Tensor) -> None:
        self.x = source.x[index]
        self.targets = {head: values[index] for head, values in source.targets.items()}
        self.masks = {head: values[index] for head, values in source.masks.items()}
        self.failure = source.failure[index]
        self.policy_target = source.policy_target[index]
        self.policy_mask = source.policy_mask[index]

    @property
    def n_rows(self) -> int:
        return int(self.x.shape[0])


# --------------------------------------------------------------------------- #
# losses
# --------------------------------------------------------------------------- #

def masked_huber(prediction: torch.Tensor, target: torch.Tensor,
                 mask: torch.Tensor, delta: float = HUBER_DELTA) -> torch.Tensor:
    """Mean Huber over the valid rows; exact zero when nothing is valid."""
    count = int(mask.sum())
    if count == 0:
        return prediction.sum() * 0.0
    return F.huber_loss(prediction[mask], target[mask], delta=delta, reduction="sum") / count


def masked_bce(logit: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    if logit.numel() == 0:
        return logit.sum() * 0.0
    return F.binary_cross_entropy_with_logits(logit, target, reduction="mean")


class PolicyObjective:
    """Behaviour-cloning loss + guardrail barrier over the C4 action vector."""

    def __init__(self, mesher_choices: list[str]) -> None:
        self.groups = action_group_slices(mesher_choices)
        self.halfwidths = torch.from_numpy(continuous_box_halfwidths())

    def loss(self, policy: torch.Tensor, target: torch.Tensor,
             mask: torch.Tensor) -> torch.Tensor:
        if policy.numel() == 0:
            return policy.sum() * 0.0
        total = policy.sum() * 0.0
        continuous = self.groups["continuous"]
        total = total + masked_huber(policy[:, continuous].reshape(-1),
                                     target[:, continuous].reshape(-1),
                                     mask[:, continuous].reshape(-1))
        p_slice = self.groups["p_elevate"]
        p_mask = mask[:, p_slice].reshape(-1)
        if bool(p_mask.any()):
            total = total + masked_bce(policy[:, p_slice].reshape(-1)[p_mask],
                                       target[:, p_slice].reshape(-1)[p_mask])
        for name in ("order", "mesher"):
            group = self.groups[name]
            rows = mask[:, group].all(dim=1)
            if bool(rows.any()):
                logits = policy[rows][:, group]
                classes = target[rows][:, group].argmax(dim=1)
                total = total + F.cross_entropy(logits, classes, reduction="mean")
        return total

    def penalty(self, policy: torch.Tensor) -> torch.Tensor:
        """``mean_rows sum_dims relu(|value| - halfwidth)`` over continuous dims."""
        if policy.numel() == 0:
            return policy.sum() * 0.0
        values = policy[:, self.groups["continuous"]]
        excess = torch.relu(values.abs() - self.halfwidths.to(values.dtype))
        return excess.sum(dim=1).mean()

    @torch.no_grad()
    def mse(self, policy: torch.Tensor, target: torch.Tensor,
            mask: torch.Tensor) -> float:
        """Masked MSE with categorical dims compared as probabilities."""
        if policy.numel() == 0 or not bool(mask.any()):
            return math.nan
        predicted = policy.clone()
        p_slice = self.groups["p_elevate"]
        predicted[:, p_slice] = torch.sigmoid(policy[:, p_slice])
        for name in ("order", "mesher"):
            group = self.groups[name]
            predicted[:, group] = torch.softmax(policy[:, group], dim=1)
        squared = (predicted - target) ** 2
        return float(squared[mask].mean())


def compute_loss(outputs: dict[str, torch.Tensor], batch: BatchView,
                 weights: dict[str, float], policy_objective: PolicyObjective,
                 beta: float) -> tuple[torch.Tensor, dict[str, float]]:
    """Weighted sum of the separate per-objective losses plus the barrier."""
    total = outputs["failure_logit"].sum() * 0.0
    parts: dict[str, float] = {}
    for head in REGRESSION_HEADS:
        weight = float(weights.get(head, 0.0))
        term = masked_huber(outputs[head].squeeze(1), batch.targets[head], batch.masks[head])
        parts[head] = float(term.detach())
        if weight != 0.0:
            total = total + weight * term
    failure_term = masked_bce(outputs["failure_logit"].squeeze(1), batch.failure)
    parts["failure"] = float(failure_term.detach())
    total = total + float(weights.get("failure", 0.0)) * failure_term
    policy_term = policy_objective.loss(outputs["policy"], batch.policy_target, batch.policy_mask)
    parts["policy"] = float(policy_term.detach())
    total = total + float(weights.get("policy", 0.0)) * policy_term
    barrier = policy_objective.penalty(outputs["policy"])
    parts["penalty"] = float(barrier.detach())
    total = total + float(beta) * barrier
    return total, parts


# --------------------------------------------------------------------------- #
# metrics
# --------------------------------------------------------------------------- #

def _auc(scores: np.ndarray, labels: np.ndarray) -> float:
    """Mann-Whitney ROC AUC; NaN when only one class is present."""
    positive = labels > 0.5
    n_pos = int(positive.sum())
    n_neg = int(labels.size - n_pos)
    if n_pos == 0 or n_neg == 0:
        return math.nan
    order = np.argsort(scores, kind="mergesort")
    ranks = np.empty(scores.size, dtype=np.float64)
    sorted_scores = scores[order]
    i = 0
    while i < sorted_scores.size:
        j = i
        while j + 1 < sorted_scores.size and sorted_scores[j + 1] == sorted_scores[i]:
            j += 1
        average = 0.5 * (i + j) + 1.0
        ranks[order[i:j + 1]] = average
        i = j + 1
    rank_sum = float(ranks[positive].sum())
    return (rank_sum - n_pos * (n_pos + 1) / 2.0) / (n_pos * n_neg)


@torch.no_grad()
def evaluate(model: AdvisorNet, tensors: SplitTensors, weights: dict[str, float],
             policy_objective: PolicyObjective, beta: float) -> dict[str, float]:
    """The 11 C7 metric keys for one split."""
    metrics: dict[str, float] = {key: math.nan for key in METRIC_KEYS}
    if tensors.n_rows == 0:
        return metrics
    model.eval()
    outputs = model(tensors.x)
    for head in REGRESSION_HEADS:
        mask = tensors.masks[head]
        if bool(mask.any()):
            residual = (outputs[head].squeeze(1)[mask] - tensors.targets[head][mask]).abs()
            metrics[f"{head}_mae"] = float(residual.mean())
    logit = outputs["failure_logit"].squeeze(1)
    metrics["failure_bce"] = float(masked_bce(logit, tensors.failure))
    probability = torch.sigmoid(logit).numpy()
    labels = tensors.failure.numpy()
    metrics["failure_acc"] = float(((probability >= 0.5).astype(np.float64) == labels).mean())
    metrics["failure_auc"] = _auc(probability.astype(np.float64), labels.astype(np.float64))
    metrics["policy_mse"] = policy_objective.mse(
        outputs["policy"], tensors.policy_target, tensors.policy_mask)
    batch = BatchView(tensors, torch.arange(tensors.n_rows))
    total, _ = compute_loss(outputs, batch, weights, policy_objective, beta)
    metrics["total_loss"] = float(total)
    return metrics


# --------------------------------------------------------------------------- #
# checkpoints
# --------------------------------------------------------------------------- #

def save_checkpoint(path: Path, model: AdvisorNet, optimizer: torch.optim.Optimizer,
                    run: int, data: AdvisorData) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    torch.save({
        "config": model.config,
        "model": model.state_dict(),
        "optimizer": optimizer.state_dict(),
        "run": run,
        "normalization": data.normalization,
        "clamps": data.clamps,
    }, path)


def build_model(data: AdvisorData, warm_start: Path | None) -> tuple[AdvisorNet, dict[str, Any] | None, str]:
    """Warm-start from ``latest.pt`` when its schema still matches the data."""
    config = data.model_config()
    torch.manual_seed(SEED)
    model = AdvisorNet(config)
    if warm_start is None or not warm_start.is_file():
        return model, None, "fresh"
    payload = torch.load(warm_start, map_location="cpu", weights_only=False)
    saved = payload.get("config", {})
    compatible = (
        list(saved.get("input_columns", [])) == config["input_columns"]
        and list(saved.get("action_dims", [])) == config["action_dims"]
        and list(saved.get("mesher_choices", [])) == config["mesher_choices"]
        and list(saved.get("order_choices", [])) == config["order_choices"]
    )
    if not compatible:
        return model, None, "schema-changed"
    model.load_state_dict(payload["model"])
    return model, payload.get("optimizer"), "warm"


# --------------------------------------------------------------------------- #
# a single training run
# --------------------------------------------------------------------------- #

def activation_record(model: AdvisorNet, data: AdvisorData, split: Split,
                      row: int, run: int) -> dict[str, Any]:
    x = torch.from_numpy(np.ascontiguousarray(split.x[row:row + 1], dtype=np.float32))
    payload = model.activations(x)
    return {
        "run": run,
        "input_case": {
            "part": split.parts[row],
            "cfg_id": split.cfg_ids[row],
            "split": split.name,
            "columns": list(data.input_columns),
            "values": [float(value) for value in split.x[row]],
        },
        "layers": payload["layers"],
        "edges": payload["edges"],
    }


def train_one_run(model: AdvisorNet, optimizer: torch.optim.Optimizer,
                  train: SplitTensors, val: SplitTensors,
                  weights: dict[str, float], policy_objective: PolicyObjective,
                  beta: float, run: int, epochs: int,
                  batch_size: int) -> tuple[list[dict[str, Any]], float]:
    """Run ``epochs`` deterministic passes; return the per-epoch metric series."""
    generator = torch.Generator()
    generator.manual_seed(SEED + run)
    series: list[dict[str, Any]] = []
    last_penalty = 0.0
    for epoch in range(1, epochs + 1):
        model.train()
        if train.n_rows:
            order = torch.randperm(train.n_rows, generator=generator)
            epoch_loss = 0.0
            epoch_penalty = 0.0
            n_batches = 0
            for start in range(0, train.n_rows, batch_size):
                index = order[start:start + batch_size]
                batch = train.batch(index)
                optimizer.zero_grad(set_to_none=True)
                outputs = model(batch.x)
                loss, parts = compute_loss(outputs, batch, weights, policy_objective, beta)
                loss.backward()
                optimizer.step()
                epoch_loss += float(loss.detach())
                epoch_penalty += parts["penalty"]
                n_batches += 1
            last_penalty = epoch_penalty / max(1, n_batches)
        else:
            epoch_loss, n_batches = 0.0, 1
        series.append({
            "epoch": epoch,
            "train_loss": epoch_loss / max(1, n_batches),
            "penalty": last_penalty,
            "train": evaluate(model, train, weights, policy_objective, beta),
            "val": evaluate(model, val, weights, policy_objective, beta),
        })
    return series, last_penalty


def run_training(args: argparse.Namespace) -> int:
    torch.set_num_threads(max(1, int(args.threads)))
    torch.manual_seed(SEED)

    data = load_dataset(args.csv, args.val_fraction)
    config = load_weights_config(args.weights)
    policy_objective = PolicyObjective(data.mesher_choices)

    # Keep the shared C4/C5 artifacts in lockstep with whatever we just trained on.
    write_json(NORMALIZATION_JSON, data.normalization)
    write_json(CLAMPS_JSON, data.clamps)

    print(f"dataset      : {data.csv_path} ({data.n_rows} rows)")
    print(f"inputs D     : {len(data.input_columns)}   actions A: {len(data.action_dims)}")
    print(f"split        : train={data.train.n_rows} val={data.val.n_rows} "
          f"(val parts: {sorted(set(data.val.parts))})")

    for _ in range(int(args.runs)):
        history = read_history()
        run = len(history) + 1
        stage, entry_run, transition = stage_for_run(history, config)
        weights = head_weights_for(stage, run, entry_run, config)
        beta = float(config.get("beta", 1.0))

        pruned = load_pruned()
        active = data.train.select(keep_mask(data.train, pruned))
        train_tensors = SplitTensors(active)
        val_tensors = SplitTensors(data.val)

        model, optimizer_state, warm = build_model(data, LATEST_CHECKPOINT)
        optimizer = torch.optim.Adam(model.parameters(), lr=float(args.lr),
                                     weight_decay=float(args.weight_decay))
        if optimizer_state is not None:
            try:
                optimizer.load_state_dict(optimizer_state)
            except ValueError:
                pass

        started = time.perf_counter()
        series, penalty = train_one_run(
            model, optimizer, train_tensors, val_tensors, weights,
            policy_objective, beta, run, int(args.epochs), int(args.batch_size),
        )
        seconds = time.perf_counter() - started

        train_metrics = series[-1]["train"] if series else {key: math.nan for key in METRIC_KEYS}
        val_metrics = series[-1]["val"] if series else {key: math.nan for key in METRIC_KEYS}

        pruning = prune_run(model, active, run, float(args.prune_fraction))

        run_dir = RUNS_DIR / f"{run:03d}"
        run_dir.mkdir(parents=True, exist_ok=True)

        record = {
            "run": run,
            "stage": stage,
            "stage_transition": transition,
            "seconds": round(seconds, 4),
            "train_rows": train_tensors.n_rows,
            "val_rows": val_tensors.n_rows,
            "pruned_rows": pruning["pruned_rows"],
            "pruned_total": pruning["pruned_total"],
            "prune_ceiling": pruning["prune_ceiling"],
            "prune_cap_reached": pruning["prune_cap_reached"],
            "head_weights": weights,
            "penalty": penalty,
            "train": train_metrics,
            "val": val_metrics,
            "timestamp": utc_now(),
        }
        metrics_payload = dict(record)
        metrics_payload.update({
            "beta": beta,
            "warm_start": warm,
            "n_epochs": len(series),
            "epochs": series,
            "prune_per_head": pruning["per_head"],
            "input_columns": list(data.input_columns),
            "action_dims": list(data.action_dims),
            "n_parameters": model.n_parameters(),
        })
        dump_json(run_dir / "metrics.json", metrics_payload)

        source = data.val if data.val.n_rows else active
        if source.n_rows:
            row = min(max(0, int(args.activation_row)), source.n_rows - 1)
            dump_json(run_dir / "activations.json",
                      activation_record(model, data, source, row, run))

        save_checkpoint(run_dir / "checkpoint.pt", model, optimizer, run, data)
        save_checkpoint(LATEST_CHECKPOINT, model, optimizer, run, data)
        append_history(record)

        val_rel = val_metrics.get("rel_err_mae")
        val_text = f"{val_rel:.4f}" if isinstance(val_rel, float) and math.isfinite(val_rel) else "n/a"
        print(f"run {run:03d} stage={stage}{' *A->B*' if transition else ''} "
              f"warm={warm} rows={train_tensors.n_rows} "
              f"val_rel_err_mae={val_text} penalty={penalty:.5f} "
              f"pruned={pruning['pruned_rows']}(+{pruning['pruned_total']} total) "
              f"{seconds:.2f}s")
    return 0


# --------------------------------------------------------------------------- #
# LightGBM baseline
# --------------------------------------------------------------------------- #

def run_baseline(args: argparse.Namespace) -> int:
    try:
        from lightgbm import LGBMRegressor
    except ImportError as error:  # pragma: no cover - environment guard
        raise SystemExit(
            f"lightgbm is required for --baseline ({error}).\n"
            f"install it with: python -m pip install lightgbm"
        ) from error

    data = load_dataset(args.csv, args.val_fraction)
    params = {"n_estimators": 300, "learning_rate": 0.05, "num_leaves": 31,
              "min_child_samples": 5, "n_jobs": 4, "random_state": SEED, "verbose": -1}

    targets: dict[str, Any] = {}
    for head in REGRESSION_HEADS:
        train_mask = data.train.masks[head]
        val_mask = data.val.masks[head]
        n_train = int(train_mask.sum())
        n_val = int(val_mask.sum())
        if n_train < 2 or n_val < 1:
            targets[head] = {"val_mae": None, "val_rmse": None, "n_train": n_train,
                             "n_val": n_val, "skipped": "insufficient masked rows"}
            continue
        regressor = LGBMRegressor(**params)
        regressor.fit(data.train.x[train_mask], data.train.targets[head][train_mask])
        prediction = regressor.predict(data.val.x[val_mask])
        truth = data.val.targets[head][val_mask].astype(np.float64)
        residual = prediction - truth
        targets[head] = {
            "val_mae": float(np.abs(residual).mean()),
            "val_rmse": float(np.sqrt((residual ** 2).mean())),
            "n_train": n_train,
            "n_val": n_val,
        }

    payload = {
        "model": "lightgbm.LGBMRegressor",
        "targets": targets,
        "n_train": data.train.n_rows,
        "n_val": data.val.n_rows,
        "params": params,
        "input_columns": list(data.input_columns),
        "timestamp": utc_now(),
    }
    dump_json(BASELINE_JSON, payload)
    print(f"wrote {BASELINE_JSON}")
    for head, metrics in targets.items():
        mae = metrics.get("val_mae")
        print(f"  {head:<12} val_mae={mae if mae is None else round(mae, 5)} "
              f"n_train={metrics['n_train']} n_val={metrics['n_val']}")
    return 0


# --------------------------------------------------------------------------- #
# self-test
# --------------------------------------------------------------------------- #

def _synthetic_history(series: list[float]) -> list[dict[str, Any]]:
    return [
        {"run": index + 1, "stage": "A", "stage_transition": False,
         "val": {"rel_err_mae": value}}
        for index, value in enumerate(series)
    ]


def _first_trigger(series: list[float], window: int, threshold: float) -> int | None:
    for length in range(1, len(series) + 1):
        if plateau_reached(series[:length], window, threshold):
            return length
    return None


def run_self_test(args: argparse.Namespace) -> int:
    """Validate the Stage A->B plateau rule without touching real artifacts."""
    window = 10
    threshold = 0.02
    failures: list[str] = []

    def check(condition: bool, message: str) -> None:
        status = "ok  " if condition else "FAIL"
        print(f"  [{status}] {message}")
        if not condition:
            failures.append(message)

    # A series that improves fast for 20 runs then flattens completely.
    # best(last 10) vs best(prev 10) only stops improving once the previous
    # window has also reached the floor, i.e. at 30 records.
    plateauing = [max(0.10, 1.0 - 0.045 * (i + 1)) for i in range(40)]
    improving = [1.0 * (0.9 ** (i + 1)) for i in range(40)]

    print("self-test: plateau trigger")
    check(not plateau_reached(plateauing[:19], window, threshold),
          "no trigger with 19 runs of history (needs 2*window)")
    first = _first_trigger(plateauing, window, threshold)
    check(first == 30, f"plateauing series first triggers at 30 records (got {first})")
    check(plateau_reached(plateauing[:30], window, threshold),
          "plateauing series triggers at exactly 30 records")
    check(not plateau_reached(plateauing[:29], window, threshold),
          "plateauing series does not trigger at 29 records")
    check(_first_trigger(improving, window, threshold) is None,
          "still-improving series never triggers")

    print("self-test: stage bookkeeping (temp dir)")
    with tempfile.TemporaryDirectory(prefix="advisor-selftest-") as tmp:
        root = Path(tmp)
        weights_path = root / "weights.json"
        config = load_weights_config(weights_path)
        check(weights_path.is_file(), "weights.json created with Stage A defaults")
        check(config["stage_a"]["dof"] == 0.0 and config["stage_a"]["rel_err_rel"] == 1.0,
              "Stage A zeroes the cost heads and keeps rel_err at 1.0")

        history_path = root / "history.jsonl"
        for record in _synthetic_history(plateauing[:29]):
            append_history(record, history_path)
        stage, entry, transition = stage_for_run(read_history(history_path), config)
        check((stage, transition) == ("A", False), f"29 records -> stage A (got {stage})")

        append_history(_synthetic_history(plateauing[:30])[-1], history_path)
        records = read_history(history_path)
        stage, entry, transition = stage_for_run(records, config)
        check((stage, transition, entry) == ("B", True, 31),
              f"30 records -> stage B transition on run 31 (got {stage},{transition},{entry})")

        weights = head_weights_for(stage, 31, entry, config)
        expected_first = config["stage_b_targets"]["solve_ms"] / config["ramp_runs"]
        check(abs(weights["solve_ms"] - expected_first) < 1e-12,
              f"stage B ramp starts at 1/{config['ramp_runs']} of the target weight")
        check(abs(head_weights_for("B", 35, 31, config)["solve_ms"]
                  - config["stage_b_targets"]["solve_ms"]) < 1e-12,
              "stage B ramp reaches the full target weight after ramp_runs")
        check(head_weights_for("B", 99, 31, config)["solve_ms"]
              == config["stage_b_targets"]["solve_ms"],
              "stage B ramp saturates, never exceeds the target")

        # Once a B record exists, the stage sticks and never re-triggers.
        b_record = dict(_synthetic_history(plateauing[:31])[-1])
        b_record.update({"run": 31, "stage": "B", "stage_transition": True})
        append_history(b_record, history_path)
        stage, entry, transition = stage_for_run(read_history(history_path), config)
        check((stage, entry, transition) == ("B", 31, False),
              "stage B is sticky and only transitions once")

        improving_path = root / "improving.jsonl"
        for record in _synthetic_history(improving):
            append_history(record, improving_path)
        stage, _, transition = stage_for_run(read_history(improving_path), config)
        check((stage, transition) == ("A", False),
              "40 still-improving records stay in stage A")

    print("self-test: guardrail barrier")
    objective = PolicyObjective(["graded_tet", "hex"])
    policy = torch.zeros(1, 4 + 4 + 2)
    policy[0, 0] = 0.15  # h_rel inside [0, 0.2]
    check(float(objective.penalty(policy)) == 0.0, "in-box policy incurs zero penalty")
    policy[0, 0] = 0.5   # h_rel above the 0.2 half-width
    check(abs(float(objective.penalty(policy)) - 0.3) < 1e-6,
          "out-of-box policy penalty equals the excess (0.5 - 0.2)")
    policy[0, 0] = -0.5  # symmetric box, |value| is what matters
    check(abs(float(objective.penalty(policy)) - 0.3) < 1e-6,
          "penalty uses |value| so negative excursions are penalised too")

    if failures:
        print(f"\nself-test FAILED: {len(failures)} check(s)")
        for message in failures:
            print(f"  - {message}")
        return 1
    print("\nself-test passed")
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=1, help="number of training runs to perform")
    parser.add_argument("--epochs", type=int, default=60, help="optimisation epochs per run")
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--lr", type=float, default=3e-3)
    parser.add_argument("--weight-decay", type=float, default=1e-4,
                        help="Adam L2 regularisation; the net overfits small corpora without it")
    parser.add_argument("--threads", type=int, default=4, help="torch CPU threads")
    parser.add_argument("--csv", default=None, help="dataset CSV override")
    parser.add_argument("--val-fraction", type=float, default=0.2)
    parser.add_argument("--weights", type=Path, default=None, help="weights.json override")
    parser.add_argument("--prune-fraction", type=float, default=0.05)
    parser.add_argument("--activation-row", type=int, default=0,
                        help="row of the val split dumped to activations.json")
    parser.add_argument("--baseline", action="store_true", help="train the LightGBM baseline only")
    parser.add_argument("--self-test", action="store_true",
                        help="validate the staging rules on synthetic history and exit")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.self_test:
        return run_self_test(args)
    if args.baseline:
        return run_baseline(args)
    return run_training(args)


if __name__ == "__main__":
    raise SystemExit(main())
