#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Closed-form refit of the untrained ``dof`` / ``mesh_ms`` / ``solve_ms`` heads.

The portable-cost cycle (``docs/advisor/0012``) trains those three heads at
loss weight 0.0 in both stages, so the shipped graph emits them from their
initialisation: on the held-out family the ``dof`` head is about three decades
low, and ``--advisor-max-dof`` never binds. This tool rewrites only those three
heads of an exported ``model.onnx``:

* each head's 96->1 Gemm is refitted on the graph's own ``trunk_fc2`` tap over
  the TRAIN split (the split the trunk itself was trained on, ``--split`` /
  ``--fold`` as for ``train.py``), with the ridge strength chosen by
  leave-family-group-out inner CV on those same rows;
* ``dof`` and ``solve_ms`` use a Huber ridge (IRLS, ``losses.HUBER_DELTA``, the
  training loss); ``mesh_ms`` a squared ridge -- the variants that won the
  21-family leave-one-family-out comparison in ``docs/advisor/0014``;
* ``mesh_ms`` and ``solve_ms`` get a ``Clip`` to their training-label range.
  ``dof`` is deliberately left unclipped: it feeds the ``max_dof`` budget
  filter, and a ceiling at the largest training mesh would let a larger mesh
  pass any budget above it.

Every other output, trunk tap and node is left untouched, and the tool asserts
that they stay bit-identical, so accuracy ranking, the feasibility gate, the
veto and the efficiency objective (``solve_flops`` / ``solve_bytes`` /
``mesh_work``) decide exactly as before. The edit is idempotent: running it on
an already-refitted graph overwrites the same weights and clip bounds.

``export_onnx.py`` rewrites ``model.onnx`` from a checkpoint and therefore
drops this edit; re-run this tool after any export until a retrain gives the
three heads a nonzero loss weight.

Usage
-----
    python scripts/advisor/refit_cost_heads.py
    python scripts/advisor/refit_cost_heads.py --csv path/to/dataset.csv \
        --model bench/advisor/model.onnx --output /tmp/model.onnx
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np

if __package__ in (None, ""):  # direct `python scripts/advisor/refit_cost_heads.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from . import dataset as D
from .losses import HUBER_DELTA
from .paths import ADVISOR_DIR
from .splits import family_of, split_groups, split_mask, fold_groups

MODEL_ONNX = ADVISOR_DIR / "model.onnx"
#: head -> (fit, clip to the training-label range)
REFIT_HEADS: dict[str, tuple[str, bool]] = {
    "dof": ("huber_ridge", False),
    "mesh_ms": ("ridge", True),
    "solve_ms": ("huber_ridge", True),
}
TAP = "trunk_fc2"
RIDGE_LAMBDAS = np.logspace(-4, 3, 15)
HUBER_LAMBDAS = np.logspace(-4, 2, 7)
INNER_FOLDS = 5
HUBER_ITERS = 12


# --------------------------------------------------------------------------- #
# Fits. Ridge with an unpenalised intercept; lambda is scaled by the row count.
# --------------------------------------------------------------------------- #

def inner_group_folds(groups: np.ndarray, k: int = INNER_FOLDS) -> list[np.ndarray]:
    """Validation masks of a k-fold over whole groups, dealt round-robin."""
    uniq = sorted(set(groups.tolist()))
    k = min(k, len(uniq))
    return [np.isin(groups, [g for i, g in enumerate(uniq) if i % k == j]) for j in range(k)]


def _ridge_path(F: np.ndarray, t: np.ndarray, lambdas) -> tuple[list[np.ndarray], np.ndarray, float]:
    mu = F.mean(0)
    tm = t.mean()
    Fc = F - mu
    e, V = np.linalg.eigh(Fc.T @ Fc)
    vc = V.T @ (Fc.T @ (t - tm))
    n = F.shape[0]
    return [V @ (vc / (e + lam * n)) for lam in lambdas], mu, float(tm)


def fit_ridge(F: np.ndarray, y: np.ndarray, groups: np.ndarray) -> dict[str, Any]:
    se = np.zeros(len(RIDGE_LAMBDAS))
    for va in inner_group_folds(groups):
        ws, mu, tm = _ridge_path(F[~va], y[~va], RIDGE_LAMBDAS)
        for j, w in enumerate(ws):
            se[j] += float(np.sum((F[va] @ w + (tm - mu @ w) - y[va]) ** 2))
    lam = float(RIDGE_LAMBDAS[int(np.argmin(se))])
    (w,), mu, tm = _ridge_path(F, y, [lam])
    return {"w": w, "b": float(tm - mu @ w), "lambda": lam}


def _huber(r: np.ndarray) -> np.ndarray:
    a = np.abs(r)
    return np.where(a <= HUBER_DELTA, 0.5 * r * r, HUBER_DELTA * (a - 0.5 * HUBER_DELTA))


def _huber_ridge(F: np.ndarray, t: np.ndarray, lam: float) -> tuple[np.ndarray, float]:
    """IRLS Huber ridge, unpenalised intercept."""
    n, k = F.shape
    wts = np.ones(n)
    w = np.zeros(k)
    b = float(np.median(t))
    for _ in range(HUBER_ITERS):
        sw = wts.sum()
        mu = (wts @ F) / sw
        tm = float(wts @ t) / sw
        Fc = F - mu
        G = (Fc * wts[:, None]).T @ Fc + lam * n * np.eye(k)
        w = np.linalg.solve(G, (Fc * wts[:, None]).T @ (t - tm))
        b = tm - mu @ w
        a = np.abs(F @ w + b - t)
        new = np.where(a <= HUBER_DELTA, 1.0, HUBER_DELTA / np.maximum(a, 1e-12))
        if np.max(np.abs(new - wts)) < 1e-4:
            break
        wts = new
    return w, float(b)


def fit_huber_ridge(F: np.ndarray, y: np.ndarray, groups: np.ndarray) -> dict[str, Any]:
    loss = np.zeros(len(HUBER_LAMBDAS))
    for va in inner_group_folds(groups):
        for j, lam in enumerate(HUBER_LAMBDAS):
            w, b = _huber_ridge(F[~va], y[~va], lam)
            loss[j] += float(_huber(F[va] @ w + b - y[va]).sum())
    lam = float(HUBER_LAMBDAS[int(np.argmin(loss))])
    w, b = _huber_ridge(F, y, lam)
    return {"w": w, "b": b, "lambda": lam}


# --------------------------------------------------------------------------- #
# Data and graph
# --------------------------------------------------------------------------- #

def load_rows(csv_path: Path, normalization: dict[str, Any]) -> dict[str, Any]:
    """Raw rows standardised with the GRAPH's normalization.json (not a
    re-derived one), with the same label masks ``load_dataset`` builds."""
    rows = D._reject_non_advisor_rows(D.read_rows(csv_path), csv_path)
    mesher_choices = sorted({str(r.get("mesher", "") or "").strip() for r in rows
                             if str(r.get("mesher", "") or "").strip()})
    if mesher_choices != list(normalization["mesher_choices"]):
        raise SystemExit(f"{csv_path} mesher vocabulary {mesher_choices} does not match "
                         f"normalization.json {normalization['mesher_choices']}")
    if list(normalization["input_columns"]) != list(D.INPUT_COLUMNS):
        raise SystemExit("normalization.json input_columns differ from dataset.INPUT_COLUMNS")
    targets, presence = D._raw_targets(rows)
    ok = np.asarray([D._failure_flag(r) for r in rows], dtype=np.float32) == 0.0
    parts = [str(r.get("part", "") or "") for r in rows]
    x = D.standardize_matrix(D._raw_matrix(rows, mesher_choices), normalization)
    # Cost heads are measured off the mesh and the clock, so (like load_dataset)
    # they need a present label on a non-failed row, not a trusted reference.
    masks = {h: presence[h] & ok for h in REFIT_HEADS}
    return {"x": x, "parts": parts, "targets": targets, "masks": masks,
            "families": np.asarray([family_of(p) for p in parts])}


def run_graph(path: Path, x: np.ndarray) -> dict[str, np.ndarray]:
    import onnxruntime as ort

    session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    names = [o.name for o in session.get_outputs()]
    return dict(zip(names, session.run(names, {"features": x.astype(np.float32)})))


def edit_graph(model_path: Path, fits: dict[str, dict[str, Any]], out_path: Path) -> None:
    import onnx
    from onnx import helper, numpy_helper

    model = onnx.load(str(model_path))
    graph = model.graph
    inits = {t.name: t for t in graph.initializer}

    def set_init(name: str, value) -> None:
        tensor = inits[name]
        tensor.CopyFrom(numpy_helper.from_array(
            np.asarray(value, dtype=np.float32).reshape(tuple(tensor.dims)), name))

    for head, fit in fits.items():
        set_init(f"net.regression_heads.{head}.weight", fit["w"])
        set_init(f"net.regression_heads.{head}.bias", [fit["b"]])
        if fit["clip"] is None:
            continue
        lo_name, hi_name, node_name = (f"refit_{head}_lo", f"refit_{head}_hi",
                                       f"refit_{head}_clip")
        lo, hi = fit["clip"]
        if lo_name in inits:  # already refitted: move the bounds only
            set_init(lo_name, lo)
            set_init(hi_name, hi)
            continue
        raw = f"{head}_unclipped"
        for node in graph.node:
            for i, name in enumerate(node.output):
                if name == head:
                    node.output[i] = raw
        graph.initializer.extend([
            numpy_helper.from_array(np.asarray(lo, np.float32), lo_name),
            numpy_helper.from_array(np.asarray(hi, np.float32), hi_name)])
        graph.node.append(helper.make_node("Clip", [raw, lo_name, hi_name], [head],
                                           name=node_name))
    onnx.checker.check_model(model)
    onnx.save(model, str(out_path))


def metrics(pred: np.ndarray, y: np.ndarray) -> dict[str, float]:
    r = pred - y
    return {"n": int(y.size), "mae": float(np.mean(np.abs(r))),
            "rmse": float(np.sqrt(np.mean(r * r))), "bias": float(np.mean(r))}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--csv", default=None, help="dataset CSV (default bench/advisor/dataset.csv)")
    parser.add_argument("--model", default=str(MODEL_ONNX), help="graph to refit")
    parser.add_argument("--output", default=None, help="output graph (default: --model, in place)")
    parser.add_argument("--report", default=None, help="optional JSON report path")
    D.add_split_args(parser)
    args = parser.parse_args(argv)

    model_path = Path(args.model)
    out_path = Path(args.output) if args.output else model_path
    csv_path = Path(args.csv) if args.csv else D.DATASET_CSV
    normalization = D.load_json(model_path.parent / "normalization.json")
    data = load_rows(csv_path, normalization)
    is_val = split_mask(data["parts"], args.split, args.fold, args.n_folds)
    groups = split_groups(data["parts"], args.split)
    held = fold_groups(groups, args.fold, len(groups) if args.n_folds is None else args.n_folds)

    before = run_graph(model_path, data["x"])
    H = before[TAP].astype(np.float64)
    fits: dict[str, dict[str, Any]] = {}
    for head, (kind, clip) in REFIT_HEADS.items():
        train = data["masks"][head] & ~is_val
        y = data["targets"][head][train].astype(np.float64)
        fitter = fit_huber_ridge if kind == "huber_ridge" else fit_ridge
        fit = fitter(H[train], y, data["families"][train])
        fit["kind"] = kind
        fit["train_rows"] = int(train.sum())
        fit["clip"] = (float(y.min()), float(y.max())) if clip else None
        fits[head] = fit

    edit_graph(model_path, fits, out_path)
    after = run_graph(out_path, data["x"])

    for name, value in before.items():
        if name not in REFIT_HEADS and not np.array_equal(value, after[name]):
            raise SystemExit(f"refit changed an output it must not touch: {name}")
    report: dict[str, Any] = {"model": out_path.name, "dataset_csv": csv_path.name,
                              "dataset_rows": int(len(data["parts"])),
                              "dataset_sha256": D.file_digest(csv_path),
                              "split": args.split, "fold": args.fold,
                              "held_out_groups": held, "heads": {}}
    for head, fit in fits.items():
        ref = H @ fit["w"] + fit["b"]
        if fit["clip"] is not None:
            ref = np.clip(ref, *fit["clip"])
        parity = float(np.max(np.abs(after[head].reshape(-1) - ref)))
        if parity > 1e-4:
            raise SystemExit(f"{head}: ONNX vs numpy refit parity {parity:.2e} > 1e-4")
        val = data["masks"][head] & is_val
        y = data["targets"][head][val].astype(np.float64)
        entry = {k: fit[k] for k in ("kind", "lambda", "train_rows", "clip")}
        entry["parity_max_abs"] = parity
        if val.any():
            entry["held_out_before"] = metrics(before[head].reshape(-1)[val].astype(np.float64), y)
            entry["held_out_after"] = metrics(after[head].reshape(-1)[val].astype(np.float64), y)
        report["heads"][head] = entry
        held_txt = ""
        if val.any():
            held_txt = (f"  held-out {','.join(held)} MAE "
                        f"{entry['held_out_before']['mae']:.3f} -> {entry['held_out_after']['mae']:.3f}")
        print(f"{head:9s} {fit['kind']:11s} lambda={fit['lambda']:.3g} rows={fit['train_rows']}"
              f" clip={fit['clip']} parity={parity:.1e}{held_txt}")
    print(f"other outputs bit-identical; wrote {out_path}")
    if args.report:
        D.write_json(Path(args.report), report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
