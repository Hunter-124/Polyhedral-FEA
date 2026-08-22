#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Accumulating outlier pruning for the advisor training loop.

After every training run the worst 5 % of training-split residuals per accuracy
head (``rel_err``, ``geo_chamfer``, ``geo_p99``) are unioned and appended to a
persistent ledger, ``bench/advisor/runs/pruned_rows.json``. Subsequent runs
train without those rows, so pruning accumulates instead of restarting.

That accumulation compounds -- each run drops 5 % of what is *left* -- so the
ledger is capped at ``MAX_LEDGER_FRACTION`` of the original training split.
Once the cap is reached pruning stops entirely; without it a 30-run schedule
would leave roughly a fifth of the corpus and the val curves would be measuring
the pruner rather than the model.

Failure rows (``failure == 1``) are never pruned: they are the only supervision
the failure head has, and a large accuracy residual is exactly what a failure
looks like.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

import numpy as np
import torch

if __package__ in (None, ""):  # direct `python scripts/advisor/prune.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import ACCURACY_HEADS, ADVISOR_DIR, Split

RUNS_DIR = ADVISOR_DIR / "runs"
PRUNED_JSON = RUNS_DIR / "pruned_rows.json"
DEFAULT_FRACTION = 0.05
#: Hard ceiling on the accumulated ledger, as a fraction of the *original*
#: training split (i.e. before any pruning). See the module docstring.
MAX_LEDGER_FRACTION = 0.25


def load_pruned(path: Path | None = None) -> set[str]:
    """Read the accumulated pruning ledger; missing file means nothing pruned."""
    ledger = Path(path) if path is not None else PRUNED_JSON
    if not ledger.is_file():
        return set()
    with ledger.open("r", encoding="utf-8") as stream:
        payload = json.load(stream)
    keys = payload.get("keys", []) if isinstance(payload, dict) else payload
    return {str(key) for key in keys}


def save_pruned(keys: set[str], runs: list[dict[str, Any]], path: Path | None = None) -> None:
    ledger = Path(path) if path is not None else PRUNED_JSON
    ledger.parent.mkdir(parents=True, exist_ok=True)
    payload = {"total": len(keys), "keys": sorted(keys), "runs": runs}
    with ledger.open("w", encoding="utf-8") as stream:
        json.dump(payload, stream, indent=2)
        stream.write("\n")


def load_ledger(path: Path | None = None) -> tuple[set[str], list[dict[str, Any]]]:
    """Ledger keys plus the per-run audit trail."""
    ledger = Path(path) if path is not None else PRUNED_JSON
    if not ledger.is_file():
        return set(), []
    with ledger.open("r", encoding="utf-8") as stream:
        payload = json.load(stream)
    if not isinstance(payload, dict):
        return {str(key) for key in payload}, []
    return {str(key) for key in payload.get("keys", [])}, list(payload.get("runs", []))


def keep_mask(split: Split, pruned: set[str]) -> np.ndarray:
    """Boolean row filter that drops already-pruned rows from ``split``."""
    if not pruned:
        return np.ones(split.n_rows, dtype=bool)
    return np.asarray([key not in pruned for key in split.keys], dtype=bool)


@torch.no_grad()
def head_residuals(model: torch.nn.Module, split: Split) -> dict[str, np.ndarray]:
    """Absolute per-row residual of every accuracy head (NaN where masked)."""
    residuals: dict[str, np.ndarray] = {
        head: np.full(split.n_rows, np.nan, dtype=np.float64) for head in ACCURACY_HEADS
    }
    if split.n_rows == 0:
        return residuals
    model.eval()
    device = next(model.parameters()).device
    outputs = model(torch.from_numpy(split.x).to(device))
    for head in ACCURACY_HEADS:
        mask = split.masks[head]
        if not mask.any():
            continue
        prediction = outputs[head].squeeze(1).detach().cpu().numpy().astype(np.float64)
        target = split.targets[head].astype(np.float64)
        residual = np.abs(prediction - target)
        residuals[head] = np.where(mask, residual, np.nan)
    return residuals


def prune_run(model: torch.nn.Module, split: Split, run: int,
              fraction: float = DEFAULT_FRACTION,
              path: Path | None = None,
              original_rows: int | None = None) -> dict[str, Any]:
    """Prune this run's worst residuals and persist the ledger.

    ``split`` must be the *active* training split (already filtered by the
    existing ledger), so a row can only be pruned once. The original split size
    is therefore ``len(split) + len(ledger)``; pass ``original_rows`` only if
    the caller filtered the split by something else as well.

    Pruning stops once the ledger reaches ``MAX_LEDGER_FRACTION`` of that
    original size. Below the ceiling the per-run rule is unchanged: the worst
    ``fraction`` of each accuracy head's eligible rows.

    Returns ``{"pruned_rows": n_this_run, "pruned_total": n_total,
    "prune_ceiling": n_max, "prune_cap_reached": bool, "per_head": {...},
    "keys": [...]}``.
    """
    keys, runs = load_ledger(path)
    before = len(keys)
    total_rows = int(original_rows) if original_rows is not None else len(split.keys) + before
    ceiling = int(np.floor(MAX_LEDGER_FRACTION * total_rows))
    allowance = max(0, ceiling - before)

    per_head: dict[str, int] = {head: 0 for head in ACCURACY_HEADS}
    by_head: dict[str, list[str]] = {head: [] for head in ACCURACY_HEADS}
    selected: set[str] = set()
    if allowance > 0:
        residuals = head_residuals(model, split)
        protected = split.failure > 0.0
        severity: dict[str, float] = {}
        for head in ACCURACY_HEADS:
            values = residuals[head]
            eligible = np.isfinite(values) & ~protected
            n_eligible = int(eligible.sum())
            count = int(np.floor(n_eligible * float(fraction)))
            if count <= 0:
                continue
            candidates = np.flatnonzero(eligible)
            # Deterministic worst-first order: residual descending, key ascending.
            order = sorted(candidates, key=lambda i: (-float(values[i]), split.keys[i]))
            for index in order[:count]:
                key = split.keys[index]
                by_head[head].append(key)
                severity[key] = max(severity.get(key, 0.0), float(values[index]))
            selected.update(by_head[head])

        selected -= keys
        if len(selected) > allowance:
            # The ceiling lands mid-run: keep the worst residuals, deterministically.
            ranked = sorted(selected, key=lambda key: (-severity[key], key))
            selected = set(ranked[:allowance])
        for head in ACCURACY_HEADS:
            per_head[head] = sum(1 for key in by_head[head] if key in selected)

    keys |= selected
    cap_reached = total_rows > 0 and len(keys) >= ceiling
    record = {"run": int(run), "pruned": len(selected), "per_head": per_head,
              "fraction": float(fraction), "ceiling": ceiling,
              "cap_reached": cap_reached}
    runs.append(record)
    save_pruned(keys, runs, path)
    return {
        "pruned_rows": len(selected),
        "pruned_total": len(keys),
        "pruned_before": before,
        "per_head": per_head,
        "original_rows": total_rows,
        "prune_ceiling": ceiling,
        "prune_cap_fraction": MAX_LEDGER_FRACTION,
        "prune_cap_reached": cap_reached,
        "keys": sorted(selected),
    }


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--show", action="store_true", help="print the current ledger summary")
    parser.add_argument("--reset", action="store_true", help="clear the accumulated ledger")
    parser.add_argument("--ledger", type=Path, default=None)
    args = parser.parse_args()

    ledger = args.ledger or PRUNED_JSON
    if args.reset:
        save_pruned(set(), [], ledger)
        print(f"cleared {ledger}")
        return 0
    keys, runs = load_ledger(ledger)
    print(f"ledger      : {ledger}")
    print(f"pruned total: {len(keys)}")
    print(f"runs logged : {len(runs)}")
    if args.show:
        for key in sorted(keys):
            print(f"  {key}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
