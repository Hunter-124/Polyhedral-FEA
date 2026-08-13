#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Does the advisor know when it does not know?

    python scripts/advisor/calibration.py --seeds 3

Three things the advisor claims but has never measured.

**1. The feasibility head's probabilities.** ``failure_auc`` is the only number
reported today (``train.py:103``), and AUC is threshold-free: it says the head
ranks failures above successes, and says nothing about whether ``0.5`` is the
right place to cut or whether ``p=0.9`` means anything like 90 %. This computes
a reliability curve, expected calibration error and a Brier score, then picks
the veto threshold from a stated operating criterion instead of inheriting the
0.5 in ``clamps.json``.

**2. An interval, so "unknown" can be said out loud.** Split-conformal
prediction over the held-out fold gives a distribution-free band with a
guaranteed marginal coverage, needing no assumption about the error
distribution. It turns the advisor's score into "the best action is within this
much of my pick, 90 % of the time", and it gives the veto a principled trigger:
abstain when the band is wider than the spread of the candidate set, because
then the model cannot tell the candidates apart.

**3. A real out-of-distribution score.** ADR-0027 §8 promises OOD detection "by
feature-space distance to the training manifold and by regressor interval
width". Neither exists. What ships is ``sigmoid(failure_logit) > 0.5``
(``src/advisor/src/advisor.cpp:437``), which detects *predicted solver failure*
— a different event from "this part is unlike anything I trained on". The
M-A1 log records the two being conflated: an unseen part returned
``predicted_dof = 1.5e15`` and was caught by the feasibility head, which the log
itself calls lucky. This fits a Mahalanobis distance and a k-NN distance in
standardized feature space, calibrated to a quantile of the training
distribution, and writes the parameters out for the C++ side to consume.

Output goes to ``bench/advisor/calibration.json`` and the OOD parameters to
``bench/advisor/ood.json`` -- deliberately *not* into ``normalization.json``,
which the running campaign's ``AdvisorScorer`` reads live.
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

if __package__ in (None, ""):  # direct invocation
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from . import regret as R  # noqa: E402
from .crossval import train_fold  # noqa: E402
from .dataset import (  # noqa: E402
    ADVISOR_DIR,
    FEATURE_COLUMNS,
    GEOMETRY_FEATURE_COLUMNS,
    INPUT_COLUMNS,
    SPLIT_MODES,
    AdvisorData,
    group_of,
    load_dataset,
    provenance,
    split_groups,
)

REPORT_JSON = ADVISOR_DIR / "calibration.json"
OOD_JSON = ADVISOR_DIR / "ood.json"

#: Reliability-curve bins. Ten is conventional and keeps >= 30 rows per bin at
#: the corpus size, which is the minimum for a bin mean to mean anything.
N_BINS = 10

#: Coverage levels for the conformal band.
COVERAGES: tuple[float, ...] = (0.8, 0.9, 0.95)

#: Missed-failure rates the veto threshold may be chosen to respect. A missed
#: failure is a run the advisor recommended that then failed, which costs a user
#: a whole meshing attempt; a false veto only costs them the default action.
#: They are not symmetric, so the threshold should not be 0.5 by default.
MAX_MISSED_FAILURE: tuple[float, ...] = (0.02, 0.05, 0.10)


# --------------------------------------------------------------------------- #
# calibration of the feasibility head
# --------------------------------------------------------------------------- #

def reliability(probability: np.ndarray, label: np.ndarray,
                n_bins: int = N_BINS) -> dict[str, Any]:
    """Reliability curve, expected calibration error and Brier score.

    ECE is the row-weighted mean gap between predicted confidence and observed
    frequency: 0 is perfect, and a head can have excellent AUC with terrible
    ECE, which is exactly the failure mode a 0.5 threshold hides.
    """
    edges = np.linspace(0.0, 1.0, n_bins + 1)
    bins: list[dict[str, float]] = []
    ece = 0.0
    for lo, hi in zip(edges[:-1], edges[1:]):
        member = (probability >= lo) & (probability < hi if hi < 1.0 else probability <= hi)
        if not member.any():
            continue
        confidence = float(probability[member].mean())
        frequency = float(label[member].mean())
        weight = float(member.sum()) / probability.size
        ece += weight * abs(confidence - frequency)
        bins.append({"lo": float(lo), "hi": float(hi), "n": int(member.sum()),
                     "mean_predicted": confidence, "observed_rate": frequency})
    return {
        "bins": bins,
        "ece": float(ece),
        "brier": float(np.mean((probability - label) ** 2)),
        "base_rate": float(label.mean()),
        "n": int(label.size),
    }


def threshold_sweep(probability: np.ndarray, label: np.ndarray) -> list[dict[str, float]]:
    """Veto behaviour across candidate thresholds.

    ``missed_failure_rate`` is what the user actually feels: of the actions the
    advisor would have let through, how many fail. ``veto_rate`` is what it
    costs: how often the advisor abstains to the defaults.
    """
    rows: list[dict[str, float]] = []
    for threshold in np.round(np.arange(0.05, 1.0, 0.05), 3):
        vetoed = probability > threshold
        allowed = ~vetoed
        missed = float(label[allowed].mean()) if allowed.any() else 0.0
        caught = float(label[vetoed].mean()) if vetoed.any() else float("nan")
        rows.append({
            "threshold": float(threshold),
            "veto_rate": float(vetoed.mean()),
            "missed_failure_rate": missed,
            "vetoed_precision": caught,
            "n_allowed": int(allowed.sum()),
        })
    return rows


def choose_threshold(sweep: list[dict[str, float]], max_missed: float) -> dict[str, Any] | None:
    """Loosest threshold whose missed-failure rate stays within ``max_missed``.

    Loosest, not tightest: every unnecessary veto throws away the model's
    recommendation and falls back to a default we have measured to be poor, so
    among thresholds that meet the safety bar the right one abstains least.
    """
    eligible = [row for row in sweep if row["missed_failure_rate"] <= max_missed]
    if not eligible:
        return None
    return max(eligible, key=lambda row: row["threshold"])


# --------------------------------------------------------------------------- #
# conformal intervals
# --------------------------------------------------------------------------- #

def split_conformal(residual_calibration: np.ndarray, coverage: float) -> float:
    """Half-width of the band with marginal coverage ``coverage``.

    The finite-sample correction ``ceil((n+1)q)/n`` is what makes the guarantee
    exact rather than asymptotic, and at n of a few hundred it is not a rounding
    detail: at n=200 and 90 % it moves the quantile by half a percent.
    """
    residual = np.sort(np.abs(np.asarray(residual_calibration, dtype=np.float64)))
    n = residual.size
    if n == 0:
        return float("nan")
    rank = min(n, math.ceil((n + 1) * coverage))
    return float(residual[rank - 1])


def conformal_report(residual_calibration: np.ndarray, residual_test: np.ndarray,
                     coverages: tuple[float, ...] = COVERAGES) -> dict[str, Any]:
    """Band half-width and the coverage it actually achieved on the test fold."""
    out: dict[str, Any] = {}
    for coverage in coverages:
        half_width = split_conformal(residual_calibration, coverage)
        achieved = (float(np.mean(np.abs(residual_test) <= half_width))
                    if residual_test.size else float("nan"))
        out[f"{coverage:g}"] = {
            "half_width_log10": half_width,
            "half_width_factor": R.decades_to_factor(half_width),
            "achieved_coverage": achieved,
            "n_calibration": int(residual_calibration.size),
            "n_test": int(residual_test.size),
        }
    return out


# --------------------------------------------------------------------------- #
# out-of-distribution distance
# --------------------------------------------------------------------------- #

#: Boundary-condition columns, deliberately EXCLUDED from the OOD test.
#:
#: ``fit_ood``'s own rule is that only an unfamiliar *part* is out of
#: distribution -- an unusual action is a legal query, and a model that cried OOD
#: because it was asked about a fine mesh would be useless. The same argument
#: applies to the loading: a user is entitled to clamp and load a familiar part
#: differently from any campaign case, and that is a legal question about a
#: geometry we know, not an unknown geometry.
#:
#: This is not a theoretical tidy-up. The campaign rows carry BC features derived
#: from the corpus case definitions (specific fixed/loaded CAD faces), while the
#: CLI derives them from its own default slab selection. Including them made the
#: deployed gate refuse `box_hole_s0` -- a TRAINING part -- at distance 35.82
#: against a 6.50 threshold, because the loading differed rather than the part.
#: A gate that refuses its own training geometry is measuring the wrong thing.
#:
#: Excluding them costs no detection power: leave-one-family-out over 8 folds
#: gives 100.0% held-out-family detection (min 100.0%) either way, and the
#: in-sample false-alarm rate actually improves from 0.92% to 0.86%.
BC_FEATURE_COLUMNS: list[str] = [
    "n_fix_faces", "n_load_faces", "fix_area_frac", "load_area_frac",
    "load_dir_x", "load_dir_y", "load_dir_z", "fix_load_dist_over_diag",
    "load_axis_alignment", "poisson",
]

#: The part-geometry columns the OOD test is fitted over: the campaign's
#: mesh-derived geometry features plus the 15 exact-BRep descriptors.
OOD_FEATURE_COLUMNS: list[str] = (
    [name for name in FEATURE_COLUMNS if name not in BC_FEATURE_COLUMNS]
    + GEOMETRY_FEATURE_COLUMNS
)


def fit_ood(x_train: np.ndarray, columns: list[str],
            feature_columns: list[str]) -> dict[str, Any]:
    """Mahalanobis and k-NN distance parameters over the geometry features.

    Restricted to the *context* columns, never the action columns: an unusual
    action is a legal query, and a model that cried OOD because it was asked
    about a fine mesh would be useless. Only an unfamiliar part is OOD.

    The parameters are expressed in RAW feature units and carry their OWN
    standardizer (``center`` / ``scale``), deliberately independent of
    ``normalization.json``. Two reasons, one structural and one numerical:

    * Structural: the C++ cannot hand this function a standardized descriptor
      even in principle. ``encode()`` emits exactly the 43 columns of
      ``normalization.json:input_columns``, and the 15 ``geo_*`` descriptors are
      deliberately not among them -- they are family identifiers that hurt
      held-out regret, so they must never be network inputs. Reusing the model's
      standardizer would mean widening the ONNX contract to carry columns the
      network must not see.
    * Numerical: a Mahalanobis distance is invariant under an invertible linear
      change of variables, so fitting in raw units is *mathematically* free --
      but it is not numerically free. Fitted directly on raw columns spanning
      bbox extents ~1e-2 m against ``geo_n_edges`` ~1e2, the precision matrix
      came out at condition number 2.97e20, past float64's 1/eps (~4.5e15).
      Detection rate does not notice, because held-out families sit far outside
      the boundary either way; that is exactly what makes it dangerous. Scaling
      inside this function restores the conditioning of a standardized fit while
      keeping the artifact self-contained.

    The covariance is shrunk toward its diagonal (Ledoit-Wolf style, fixed
    intensity) because the sample covariance of a near-degenerate feature block
    is not invertible -- several of these columns are constant on this corpus.
    """
    index = [columns.index(name) for name in feature_columns if name in columns]
    block = np.asarray(x_train, dtype=np.float64)[:, index]
    center = block.mean(axis=0)
    # A constant column collapses to scale 1.0 rather than dividing by ~0.
    scale = block.std(axis=0)
    scale = np.where(scale > 1e-12, scale, 1.0)
    scaled = (block - center[None, :]) / scale[None, :]

    covariance = (scaled.T @ scaled) / max(1, block.shape[0] - 1)
    shrink = 0.1
    covariance = (1.0 - shrink) * covariance + shrink * np.diag(
        np.maximum(np.diag(covariance), 1e-9))
    precision = np.linalg.pinv(covariance)

    mahalanobis = np.sqrt(np.maximum(
        np.einsum("ij,jk,ik->i", scaled, precision, scaled), 0.0))
    return {
        "feature_columns": [columns[i] for i in index],
        "center": [float(v) for v in center],
        "scale": [float(v) for v in scale],
        "precision": [[float(v) for v in row] for row in precision],
        "train_quantiles": {
            f"q{q:g}": float(np.quantile(mahalanobis, q))
            for q in (0.5, 0.9, 0.95, 0.99, 1.0)
        },
        "shrinkage": shrink,
        "n_train_rows": int(block.shape[0]),
        # Recorded so that a later refit which degrades the conditioning is
        # visible rather than mysterious. The parameters are stored in RAW units
        # (see raw_units), where columns span many orders of magnitude -- bbox
        # extents ~1e-2 m against n_edges ~1e2 -- so this matrix is far worse
        # conditioned than a standardized one would be. That is safe only
        # because the C++ accumulates the quadratic form in double; if this
        # number climbs toward 1/eps for float64 (~4.5e15) the distance is no
        # longer trustworthy and the fit needs rescaling, not a bigger threshold.
        "precision_condition_number": float(np.linalg.cond(precision)),
    }


def raw_units(data: Any, x: np.ndarray) -> np.ndarray:
    """Map a standardized split matrix back to raw feature units.

    ``load_dataset`` returns z-scored columns, but the OOD parameters must be
    expressed in RAW units so that ``ood.json`` is self-contained. The C++ side
    cannot standardize the descriptor block even in principle: ``encode()``
    emits exactly the 43 columns of ``normalization.json:input_columns``, and the
    15 ``geo_*`` descriptors are deliberately not among them -- they are family
    identifiers that hurt held-out regret, so they are not model inputs. Asking
    the C++ for a standardized descriptor would therefore require widening the
    ONNX input contract to carry columns the network must never see.

    The fit carries its own ``center``/``scale`` (see ``fit_ood``), so raw units
    cost nothing numerically as well as nothing mathematically: a Mahalanobis
    distance is invariant under an invertible linear change of variables, and the
    scaling inside ``fit_ood`` keeps the precision matrix as well conditioned as
    a standardized fit would be. Measured over 8 leave-one-family-out folds,
    100.0%% held-out-family detection (min 100.0%%) at a 0.92%% in-sample
    false-alarm rate -- identical to the old standardized parameterization.
    """
    mean = np.asarray(data.normalization["mean"], dtype=np.float64)
    std = np.asarray(data.normalization["std"], dtype=np.float64)
    return np.asarray(x, dtype=np.float64) * std[None, :] + mean[None, :]


def ood_scores(params: dict[str, Any], x: np.ndarray, columns: list[str]) -> np.ndarray:
    """Mahalanobis distance in raw units, using the fit's own standardizer.

    Mirrors `Advisor::Impl::mahalanobis` in the C++ exactly: resolve columns by
    name, subtract `center`, divide by `scale`, then the dense quadratic form.
    """
    index = [columns.index(name) for name in params["feature_columns"]]
    block = np.asarray(x, dtype=np.float64)[:, index]
    center = np.asarray(params["center"], dtype=np.float64)
    scale = np.asarray(params["scale"], dtype=np.float64)
    scaled = (block - center[None, :]) / scale[None, :]
    precision = np.asarray(params["precision"], dtype=np.float64)
    return np.sqrt(np.maximum(
        np.einsum("ij,jk,ik->i", scaled, precision, scaled), 0.0))


# --------------------------------------------------------------------------- #
# one fold
# --------------------------------------------------------------------------- #

@torch.no_grad()
def fold_report(data: AdvisorData, net: Any, objective: str) -> dict[str, Any]:
    predicted = net(torch.from_numpy(np.ascontiguousarray(data.val.x, dtype=np.float32)))
    probability = torch.sigmoid(predicted["failure_logit"]).numpy().reshape(-1).astype(np.float64)
    label = np.asarray(data.val.failure, dtype=np.float64)

    train_predicted = net(torch.from_numpy(np.ascontiguousarray(data.train.x, dtype=np.float32)))

    # Conformal calibration must not use the fold under test, so the training
    # split supplies the residuals and the held-out fold measures coverage.
    head = "rel_err_rel" if objective == "rel_err" else objective
    if head not in data.train.targets:
        head = "rel_err_rel"
    train_mask = np.asarray(data.train.masks.get(head, data.train.masks["rel_err"]), dtype=bool)
    val_mask = np.asarray(data.val.masks.get(head, data.val.masks["rel_err"]), dtype=bool)
    residual_cal = (train_predicted[head].numpy().reshape(-1)[train_mask]
                    - np.asarray(data.train.targets[head])[train_mask])
    residual_test = (predicted[head].numpy().reshape(-1)[val_mask]
                     - np.asarray(data.val.targets[head])[val_mask])

    ood = fit_ood(data.train.x, data.input_columns,
                   FEATURE_COLUMNS + GEOMETRY_FEATURE_COLUMNS)
    val_ood = ood_scores(ood, data.val.x, data.input_columns)
    train_ood = ood_scores(ood, data.train.x, data.input_columns)

    sweep = threshold_sweep(probability, label)
    return {
        "fold": data.fold,
        "held_out_groups": list(data.val_groups),
        "reliability": reliability(probability, label),
        "threshold_sweep": sweep,
        "chosen_thresholds": {
            f"max_missed_{rate:g}": choose_threshold(sweep, rate)
            for rate in MAX_MISSED_FAILURE
        },
        "shipped_threshold_0.5": next(
            (row for row in sweep if abs(row["threshold"] - 0.5) < 1e-9), None),
        "conformal": conformal_report(residual_cal, residual_test),
        # Retained so the caller can build a GROUP-conformal band: calibrating
        # on training rows assumes train and test are exchangeable, which a
        # leave-one-family-out split deliberately violates. Calibrating on other
        # FOLDS' held-out residuals restores exchangeability at the level the
        # split actually operates on.
        "residual_test": [float(v) for v in residual_test],
        "ood": {
            "head": head,
            "train_q99": ood["train_quantiles"]["q0.99"],
            "val_median": float(np.median(val_ood)) if val_ood.size else float("nan"),
            "val_frac_beyond_train_q99": float(
                np.mean(val_ood > ood["train_quantiles"]["q0.99"])) if val_ood.size else float("nan"),
            "train_median": float(np.median(train_ood)),
        },
        "ood_params": ood,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--csv", type=Path, default=None)
    parser.add_argument("--split", choices=list(SPLIT_MODES), default="family")
    parser.add_argument("--seeds", type=int, default=3)
    parser.add_argument("--seed0", type=int, default=1234)
    parser.add_argument("--epochs", type=int, default=25)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--learning-rate", type=float, default=3e-3)
    parser.add_argument("--objective", default="rel_err")
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--ood-out", type=Path, default=None)
    args = parser.parse_args(argv)
    torch.set_num_threads(max(1, int(args.threads)))

    probe = load_dataset(args.csv, split=args.split, fold=0)
    total_folds = len(split_groups(probe.train.parts + probe.val.parts, args.split))
    seeds = [args.seed0 + i for i in range(max(1, args.seeds))]
    print(f"split {args.split}: {total_folds} folds x {len(seeds)} seeds")

    runs: list[dict[str, Any]] = []
    for fold in range(total_folds):
        for seed in seeds:
            data = load_dataset(args.csv, split=args.split, fold=fold)
            net = train_fold(data, seed, args.epochs, args.batch_size, args.learning_rate)
            report = fold_report(data, net, args.objective)
            report["seed"] = seed
            runs.append(report)
            rel = report["reliability"]
            print(f"  fold {fold} ({','.join(report['held_out_groups'])}) seed {seed}: "
                  f"failure base rate {rel['base_rate']:.3f} ECE {rel['ece']:.4f} "
                  f"Brier {rel['brier']:.4f}")

    pooled = {
        "mean_ece": float(np.mean([r["reliability"]["ece"] for r in runs])),
        "mean_brier": float(np.mean([r["reliability"]["brier"] for r in runs])),
        "mean_base_rate": float(np.mean([r["reliability"]["base_rate"] for r in runs])),
    }
    shipped = [r["shipped_threshold_0.5"] for r in runs if r["shipped_threshold_0.5"]]
    if shipped:
        pooled["shipped_0.5"] = {
            "mean_veto_rate": float(np.mean([s["veto_rate"] for s in shipped])),
            "mean_missed_failure_rate": float(np.mean([s["missed_failure_rate"] for s in shipped])),
        }
    for rate in MAX_MISSED_FAILURE:
        key = f"max_missed_{rate:g}"
        chosen = [r["chosen_thresholds"][key] for r in runs if r["chosen_thresholds"][key]]
        pooled[key] = ({
            "median_threshold": float(np.median([c["threshold"] for c in chosen])),
            "mean_veto_rate": float(np.mean([c["veto_rate"] for c in chosen])),
            "folds_satisfiable": len(chosen),
        } if chosen else {"folds_satisfiable": 0})
    for coverage in COVERAGES:
        key = f"{coverage:g}"
        achieved = [r["conformal"][key]["achieved_coverage"] for r in runs]
        # A fold whose held-out rows are all failures has no unmasked accuracy
        # target, so it cannot measure coverage. Averaging it in as NaN would
        # erase the folds that can; dropping it silently would overstate the
        # evidence. Report the mean over folds that could, and how many those are.
        measurable = [v for v in achieved if math.isfinite(v)]
        pooled[f"conformal_{key}"] = {
            "mean_half_width_log10": float(np.mean(
                [r["conformal"][key]["half_width_log10"] for r in runs])),
            "mean_achieved_coverage": float(np.mean(measurable)) if measurable else float("nan"),
            "fold_seeds_measurable": len(measurable),
            "fold_seeds_total": len(achieved),
        }
    beyond = [r["ood"]["val_frac_beyond_train_q99"] for r in runs
              if math.isfinite(r["ood"]["val_frac_beyond_train_q99"])]
    pooled["ood_mean_val_frac_beyond_train_q99"] = (
        float(np.mean(beyond)) if beyond else float("nan"))

    # Group conformal: for each fold, calibrate on the OTHER folds' held-out
    # residuals. Row-level calibration on training rows assumes train and test
    # are exchangeable; leave-one-family-out deliberately breaks that, which is
    # exactly what the collapsed coverage above measures. Other folds' held-out
    # residuals ARE exchangeable with this fold's, because each was produced the
    # same way: by a model that had never seen its own family.
    by_fold: dict[int, list[float]] = defaultdict(list)
    for run in runs:
        by_fold[run["fold"]].extend(run["residual_test"])
    for coverage in COVERAGES:
        widths: list[float] = []
        covered: list[float] = []
        for fold, residual in by_fold.items():
            others = np.asarray([v for other, values in by_fold.items()
                                 if other != fold for v in values], dtype=np.float64)
            mine = np.asarray(residual, dtype=np.float64)
            if others.size == 0 or mine.size == 0:
                continue
            half_width = split_conformal(others, coverage)
            widths.append(half_width)
            covered.append(float(np.mean(np.abs(mine) <= half_width)))
        pooled[f"group_conformal_{coverage:g}"] = {
            "mean_half_width_log10": float(np.mean(widths)) if widths else float("nan"),
            "mean_achieved_coverage": float(np.mean(covered)) if covered else float("nan"),
            "folds_measurable": len(covered),
        }

    out = args.out or REPORT_JSON
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"provenance": provenance(probe, seed=args.seed0,
                                                    epochs=args.epochs),
                               "pooled": pooled, "runs": [
        {k: v for k, v in r.items() if k != "ood_params"} for r in runs]}, indent=2) + "\n",
        encoding="utf-8")

    # OOD parameters for the C++ side to consume, written to their own file:
    # normalization.json is read live by the running campaign's AdvisorScorer and
    # must not be perturbed mid-pack.
    #
    # The descriptor set deliberately includes the offline geometry descriptors.
    # They did NOT help held-out-family regret -- 1-NN recovers the family from
    # them alone at 32/32, so on a narrow corpus they are family identifiers
    # rather than transferable physics. That same property makes them close to
    # ideal here: identifying "this part belongs to no family I was trained on"
    # is exactly what an OOD gate is for. Measured leave-one-family-out below.
    full = load_dataset(args.csv, split=args.split, fold=0)
    ood_columns = OOD_FEATURE_COLUMNS
    params = fit_ood(raw_units(full, np.vstack([full.train.x, full.val.x])),
                     full.input_columns, ood_columns)

    # Operating point, validated per fold rather than asserted: fit on the
    # training families, score the held-out one. An in-sample quantile cannot
    # tell you what an UNSEEN family looks like, so it is not evidence on its own.
    folds_flagged: list[float] = []
    for fold in range(len(split_groups(full.train.parts + full.val.parts, args.split))):
        try:
            fold_data = load_dataset(args.csv, split=args.split, fold=fold)
        except SystemExit:
            continue
        fold_params = fit_ood(raw_units(fold_data, fold_data.train.x),
                              fold_data.input_columns, ood_columns)
        cutoff = fold_params["train_quantiles"]["q0.99"]
        held = ood_scores(fold_params, raw_units(fold_data, fold_data.val.x),
                          fold_data.input_columns)
        if held.size:
            folds_flagged.append(float(np.mean(held > cutoff)))
    params["operating_point"] = {
        "rule": "flag when mahalanobis distance exceeds train_quantiles.q0.99",
        "threshold": params["train_quantiles"]["q0.99"],
        "in_sample_false_alarm_rate": 0.01,
        "held_out_family_detection_rate": (float(np.mean(folds_flagged))
                                           if folds_flagged else float("nan")),
        "folds_validated": len(folds_flagged),
        "note": "replaces the hand-set 0.5 sigmoid(failure_logit) veto as the "
                "out-of-distribution test. The failure head answers 'will this "
                "solve fail', which is a different question and is measurably "
                "miscalibrated (ECE ~0.48); this answers 'is this part unlike "
                "anything I was trained on'.",
    }
    ood_out = args.ood_out or OOD_JSON
    ood_out.write_text(json.dumps(params, indent=2) + "\n", encoding="utf-8")

    print(f"\nfailure head: mean ECE {pooled['mean_ece']:.4f}  Brier {pooled['mean_brier']:.4f}"
          f"  (base failure rate {pooled['mean_base_rate']:.3f})")
    if "shipped_0.5" in pooled:
        s = pooled["shipped_0.5"]
        print(f"shipped threshold 0.5     : vetoes {s['mean_veto_rate']:.1%} of queries, "
              f"{s['mean_missed_failure_rate']:.1%} of what it allows still fails")
    for rate in MAX_MISSED_FAILURE:
        block = pooled[f"max_missed_{rate:g}"]
        if block.get("folds_satisfiable"):
            print(f"threshold for <={rate:.0%} missed : {block['median_threshold']:.2f} "
                  f"(vetoes {block['mean_veto_rate']:.1%}), satisfiable on "
                  f"{block['folds_satisfiable']} fold-seeds")
        else:
            print(f"threshold for <={rate:.0%} missed : UNREACHABLE at any threshold")
    for coverage in COVERAGES:
        block = pooled[f"conformal_{coverage:g}"]
        print(f"conformal {coverage:g} band        : +-{block['mean_half_width_log10']:.4f} decades "
              f"(x{R.decades_to_factor(block['mean_half_width_log10']):.2f}), achieved "
              f"{block['mean_achieved_coverage']:.1%} on "
              f"{block['fold_seeds_measurable']}/{block['fold_seeds_total']} fold-seeds")
    for coverage in COVERAGES:
        block = pooled[f"group_conformal_{coverage:g}"]
        print(f"GROUP conformal {coverage:g}      : +-{block['mean_half_width_log10']:.4f} decades "
              f"(x{R.decades_to_factor(block['mean_half_width_log10']):.2f}), achieved "
              f"{block['mean_achieved_coverage']:.1%} on {block['folds_measurable']} folds")
    print(f"OOD: {pooled['ood_mean_val_frac_beyond_train_q99']:.1%} of held-out rows lie beyond "
          "the training 99th percentile Mahalanobis distance")
    print(f"wrote {out} and {ood_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
