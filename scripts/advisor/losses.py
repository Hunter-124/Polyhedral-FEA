# SPDX-License-Identifier: BSD-3-Clause
"""Torch views of the advisor splits, the C7 objectives and their metrics.

Shared by ``train.py`` (the production run loop), ``crossval.py`` and the
research benchmarks, so every consumer optimises and scores exactly the same
per-head losses.
"""

from __future__ import annotations

import math

import numpy as np
import torch
import torch.nn.functional as F

from .dataset import (
    REGRESSION_HEADS,
    Split,
    action_group_slices,
    continuous_box_halfwidths,
)
from .model import AdvisorNet

HUBER_DELTA = 1.0

METRIC_KEYS = [f"{head}_mae" for head in REGRESSION_HEADS] + [
    "failure_bce", "failure_acc", "failure_auc", "policy_mse", "total_loss",
]


# --------------------------------------------------------------------------- #
# tensors
# --------------------------------------------------------------------------- #

class SplitTensors:
    """Torch view of a :class:`~advisor.dataset.Split`."""

    def __init__(self, split: Split, device: torch.device | str = "cpu") -> None:
        self.split = split
        self.x = torch.from_numpy(
            np.ascontiguousarray(split.x, dtype=np.float32)).to(device)
        self.targets = {
            head: torch.from_numpy(
                np.ascontiguousarray(values, dtype=np.float32)).to(device)
            for head, values in split.targets.items()
        }
        self.masks = {
            head: torch.from_numpy(np.ascontiguousarray(values, dtype=bool)).to(device)
            for head, values in split.masks.items()
        }
        self.failure = torch.from_numpy(
            np.ascontiguousarray(split.failure, dtype=np.float32)).to(device)
        self.policy_target = torch.from_numpy(
            np.ascontiguousarray(split.policy_target, dtype=np.float32)).to(device)
        self.policy_mask = torch.from_numpy(
            np.ascontiguousarray(split.policy_mask, dtype=bool)).to(device)

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
        # No p_elevate branch: it was removed from the action vector because
        # `order >= 2` is the same actuator (apps/cli/commands_solve.cpp).
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
        excess = torch.relu(
            values.abs() - self.halfwidths.to(device=values.device, dtype=values.dtype))
        return excess.sum(dim=1).mean()

    @torch.no_grad()
    def mse(self, policy: torch.Tensor, target: torch.Tensor,
            mask: torch.Tensor) -> float:
        """Masked MSE with categorical dims compared as probabilities."""
        if policy.numel() == 0 or not bool(mask.any()):
            return math.nan
        predicted = policy.clone()
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
    """Every ``METRIC_KEYS`` entry for one split."""
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
    probability = torch.sigmoid(logit).cpu().numpy()
    labels = tensors.failure.cpu().numpy()
    metrics["failure_acc"] = float(((probability >= 0.5).astype(np.float64) == labels).mean())
    metrics["failure_auc"] = _auc(probability.astype(np.float64), labels.astype(np.float64))
    metrics["policy_mse"] = policy_objective.mse(
        outputs["policy"], tensors.policy_target, tensors.policy_mask)
    batch = BatchView(
        tensors, torch.arange(tensors.n_rows, device=tensors.x.device))
    total, _ = compute_loss(outputs, batch, weights, policy_objective, beta)
    metrics["total_loss"] = float(total)
    return metrics
