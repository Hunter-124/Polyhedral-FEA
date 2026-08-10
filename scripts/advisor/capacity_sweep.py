#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Does the ``rel_err`` head need more capacity, or a different target?

Trains the accuracy head alone at a range of widths and depths on the exact
part-hash split ``train.py`` uses, and reports train and validation MAE for
each, so the capacity question is answered by measurement rather than opinion.

It fits two targets at every capacity:

* **absolute** -- ``log10(rel_err)`` as trained today.
* **centred**  -- the same value minus that case's median over the actions
  actually run. Ranking the actions *within* a case is what an advisor needs;
  that ranking does not require knowing the case's absolute error level, which
  is set by how good its reference truth happens to be.

    python scripts/advisor/capacity_sweep.py
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import torch
from torch import nn

if __package__ in (None, ""):  # direct `python scripts/advisor/capacity_sweep.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import centre_by_case, load_dataset  # noqa: E402

SEED = 1234
#: (hidden width, hidden layer count)
GEOMETRIES: list[tuple[int, int]] = [
    (32, 2), (64, 2), (128, 2), (256, 2), (512, 2), (256, 3), (256, 4), (512, 4)
]


def build(n_inputs: int, hidden: int, layers: int) -> nn.Module:
    blocks: list[nn.Module] = [nn.Linear(n_inputs, hidden), nn.GELU()]
    for _ in range(layers - 1):
        blocks += [nn.Linear(hidden, hidden), nn.GELU()]
    blocks.append(nn.Linear(hidden, 1))
    return nn.Sequential(*blocks)


def fit(x_tr, y_tr, x_va, y_va, hidden, layers, epochs, lr) -> tuple[float, float, int]:
    torch.manual_seed(SEED)
    net = build(x_tr.shape[1], hidden, layers)
    opt = torch.optim.AdamW(net.parameters(), lr=lr, weight_decay=1e-4)
    loss_fn = nn.HuberLoss(delta=1.0)
    xt, yt = torch.from_numpy(x_tr), torch.from_numpy(y_tr).unsqueeze(1)
    xv, yv = torch.from_numpy(x_va), torch.from_numpy(y_va).unsqueeze(1)
    for _ in range(epochs):
        net.train()
        opt.zero_grad()
        loss_fn(net(xt), yt).backward()
        opt.step()
    net.eval()
    with torch.no_grad():
        tr = float(torch.mean(torch.abs(net(xt) - yt)))
        va = float(torch.mean(torch.abs(net(xv) - yv)))
    return tr, va, sum(p.numel() for p in net.parameters())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--epochs", type=int, default=1500)
    parser.add_argument("--lr", type=float, default=3e-3)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    torch.set_num_threads(args.threads)

    data = load_dataset()
    head = "rel_err"
    tr_mask = data.train.masks[head]
    va_mask = data.val.masks[head]
    x_tr, x_va = data.train.x[tr_mask], data.val.x[va_mask]
    y_tr = data.train.targets[head][tr_mask]
    y_va = data.val.targets[head][va_mask]

    tr_parts = [p for p, m in zip(data.train.parts, tr_mask) if m]
    va_parts = [p for p, m in zip(data.val.parts, va_mask) if m]
    yc_tr = centre_by_case(data.train.targets[head], tr_mask, data.train.parts)[tr_mask]
    yc_va = centre_by_case(data.val.targets[head], va_mask, data.val.parts)[va_mask]

    print(f"train {x_tr.shape[0]} rows / {len(set(tr_parts))} cases   "
          f"val {x_va.shape[0]} rows / {len(set(va_parts))} cases")
    print(f"absolute target : train sd={y_tr.std():.3f}  val sd={y_va.std():.3f}")
    print(f"centred  target : train sd={yc_tr.std():.3f}  val sd={yc_va.std():.3f}")
    print()
    print(f"{'width':>6} {'depth':>6} {'params':>9} | "
          f"{'abs train':>10} {'abs val':>9} | {'ctr train':>10} {'ctr val':>9}")
    print("-" * 74)
    for hidden, layers in GEOMETRIES:
        a_tr, a_va, n = fit(x_tr, y_tr, x_va, y_va, hidden, layers, args.epochs, args.lr)
        c_tr, c_va, _ = fit(x_tr, yc_tr, x_va, yc_va, hidden, layers, args.epochs, args.lr)
        print(f"{hidden:>6} {layers:>6} {n:>9} | "
              f"{a_tr:>10.4f} {a_va:>9.4f} | {c_tr:>10.4f} {c_va:>9.4f}")

    print()
    print("A capacity-limited head improves on BOTH columns as width grows.")
    print("An unlearnable-target head drives train down and val up.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
