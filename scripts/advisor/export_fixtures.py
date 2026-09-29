# SPDX-License-Identifier: BSD-3-Clause
"""Deterministic C++ test fixtures: ``tests/fixtures/advisor_tiny/`` (no taps) and
``tests/fixtures/advisor_explain/`` (taps + layout sidecar), built from a tiny
network on synthetic rows so every advisor code path is reached by construction."""
from __future__ import annotations

import argparse
import csv
import math
import tempfile
from pathlib import Path
from typing import Any

import numpy as np
import torch

from .calibration import OOD_FEATURE_COLUMNS, fit_ood, ood_scores
from .dataset import (
    CASE_COLUMNS,
    CONTINUOUS_ACTION_DIMS,
    FEATURE_COLUMNS,
    OUTPUT_NAMES,
    REGRESSION_HEADS,
    action_group_slices,
    clamp_table,
    load_from_args,
    standardize_row,
    write_json,
)
from .export_graph import OPSET, export_graph
from .export_parity import PARITY_TOLERANCE, float64_reference, sample_raw_batch, verify_parity
from .model import ACTIVATION_OUTPUT_NAMES, AdvisorNet
from .paths import REPO_ROOT

FIXTURE_DIR = REPO_ROOT / "tests" / "fixtures" / "advisor_tiny"

#: Fixed so the fixture is byte-reproducible across machines and reruns.
TINY_SEED = 20260810

TINY_MESHERS = ["graded_tet", "hybrid_zoo"]
TINY_PARTS = ["tiny_bar", "tiny_bracket", "tiny_plate", "tiny_shaft"]

#: fixture case name -> forced head values, in TINY_CASE_NAMES order below.
TINY_CASE_NAMES = ["nominal", "clamped_low_h_rel", "vetoed_failure", "imputed_defaults"]
TINY_FORCED: dict[str, list[float]] = {
    "rel_err": [-2.13, -1.42, -0.35, -1.90],
    # Per-case-centred, so it straddles zero by construction: negative means
    # this action is better than that case's median action.
    "rel_err_rel": [-0.42, 0.18, 0.95, -0.27],
    "geo_chamfer": [-3.01, -2.45, -1.10, -2.80],
    "geo_p99": [-2.40, -1.95, -0.80, -2.20],
    "dof": [4.20, 5.10, 3.40, 4.55],
    "mesh_ms": [2.10, 2.60, 1.80, 2.25],
    "solve_ms": [2.90, 3.50, 2.20, 3.05],
    "solve_flops": [6.20, 7.10, 5.80, 6.50],
    "solve_bytes": [7.00, 7.80, 6.50, 7.20],
    "mesh_work": [-2.00, -1.00, 0.50, -1.50],
    "failure_logit": [-6.00, -4.50, 6.50, -5.25],
    # policy continuous dims, physical units
    "policy_h_rel": [0.080, 0.0005, 0.050, 0.120],
    "policy_adapt_passes": [2.0, 1.0, 3.0, 0.0],
    "policy_eta_target": [0.050, 0.020, 0.100, 0.030],
}



def synthetic_csv(path: Path, n_rows: int = 32) -> Path:
    """Write a deterministic synthetic advisor CSV covering every C2 column."""
    generator = np.random.default_rng(TINY_SEED)
    columns = (
        ["schema", "campaign", "cfg_id", "part", "tier"]
        + FEATURE_COLUMNS
        + CASE_COLUMNS
        + ["h", "h_rel", "mesher", "element_tendency", "skin_layers", "feature_refine",
           "bc_grading", "adapt_passes", "eta_target", "p_elevate", "adapt_leb_waves", "order"]
        + ["status", "mesh_ms", "solve_ms", "n_dof", "n_elems", "n_nodes",
           "accuracy_rel_err", "accuracy_trusted",
           "geo_fidelity_chamfer_mean", "geo_fidelity_dist_p95",
           "geo_fidelity_dist_p99", "geo_fidelity_dist_max",
           "geo_fidelity_available"]
    )
    rows: list[dict[str, Any]] = []
    for index in range(n_rows):
        part = TINY_PARTS[index % len(TINY_PARTS)]
        scale = 0.05 + 0.02 * (index % len(TINY_PARTS))
        h_rel = float(np.round(0.01 + 0.03 * generator.random(), 5))
        row: dict[str, Any] = {
            "schema": "advisor-row-v3",
            "campaign": "advisor-tiny-fixture",
            "cfg_id": f"c{index:04d}",
            "part": part,
            "tier": 0,
        }
        for offset, name in enumerate(FEATURE_COLUMNS):
            row[name] = float(np.round(scale * (1.0 + offset * 0.1)
                                       + 0.01 * generator.standard_normal(), 6))
        for name in CASE_COLUMNS:
            row[name] = float(np.round(0.3 + 0.1 * generator.random(), 6))
        row["case_n_fix_regions"] = 1
        row["case_n_load_regions"] = 1
        row["h"] = float(np.round(h_rel * scale, 8))
        row["h_rel"] = h_rel
        row["mesher"] = TINY_MESHERS[index % len(TINY_MESHERS)]
        row["element_tendency"] = float(np.round(generator.random(), 4))
        row["skin_layers"] = int(index % 3)
        row["feature_refine"] = bool(index % 2)
        row["bc_grading"] = float(np.round(0.5 * generator.random(), 4))
        row["adapt_passes"] = int(index % 4)
        row["eta_target"] = float(np.round(0.01 + 0.05 * generator.random(), 5))
        row["p_elevate"] = bool((index // 2) % 2)
        row["adapt_leb_waves"] = int(index % 2)
        row["order"] = 1 + (index % 2)
        row["status"] = "ok" if index % 16 else "over_budget"
        row["mesh_ms"] = float(np.round(20.0 + 200.0 * generator.random(), 4))
        row["solve_ms"] = float(np.round(100.0 + 2000.0 * generator.random(), 4))
        row["n_dof"] = int(5000 + 500 * index)
        row["n_elems"] = int(4000 + 400 * index)
        row["n_nodes"] = int(1800 + 180 * index)
        row["accuracy_rel_err"] = float(np.round(0.002 + 0.2 * generator.random(), 8))
        row["accuracy_trusted"] = row["status"] == "ok"
        # Only dist_p95 <= dist_p99 <= dist_max and chamfer_mean <= dist_max
        # hold on real meshes; chamfer_mean is NOT bounded by dist_p95, since
        # p95 collapses to ~0 while the mean is carried by the tail.
        chamfer = float(np.round(1e-4 + 5e-3 * generator.random(), 9))
        p95 = float(np.round(5e-5 * generator.random(), 9))
        p99 = p95 + float(np.round(5e-4 + 1e-2 * generator.random(), 9))
        row["geo_fidelity_chamfer_mean"] = chamfer
        row["geo_fidelity_dist_p95"] = p95
        row["geo_fidelity_dist_p99"] = p99
        row["geo_fidelity_dist_max"] = max(p99, chamfer) + float(
            np.round(1e-3 + 2e-2 * generator.random(), 9))
        row["geo_fidelity_available"] = True
        rows.append(row)

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    return path


def tiny_raw_cases(normalization: dict[str, Any]) -> list[dict[str, Any]]:
    """Four raw (pre-standardization) feature dicts, one per fixture case."""
    columns = normalization["input_columns"]
    generator = np.random.default_rng(TINY_SEED + 1)
    defaults = clamp_table(normalization["mesher_choices"])["defaults"]
    cases: list[dict[str, Any]] = []
    for index, name in enumerate(TINY_CASE_NAMES):
        raw: dict[str, Any] = {}
        for offset, column in enumerate(columns):
            if column == "order_idx":
                raw[column] = float(index % len(normalization["order_choices"]))
                continue
            if column == "mesher_idx":
                raw[column] = float(index % len(normalization["mesher_choices"]))
                continue
            raw[column] = float(np.round(
                0.05 * (1 + index) * (1.0 + 0.1 * offset) + 0.02 * generator.standard_normal(),
                6))
        # The C++ Advisor queries the policy head at the clamp-box DEFAULT
        # action, so a fixture case only exercises the recommend path if its
        # action columns already are those defaults. Otherwise the forced policy
        # values would belong to a row the C++ never evaluates.
        raw["h_rel"] = float(defaults["h_rel"])
        raw["eta_target"] = float(defaults["eta_target"])
        raw["adapt_passes"] = float(defaults["adapt_passes"])
        # No p_elevate: `order >= 2` is the p-elevation actuator, so the column
        # no longer exists in the input vector.
        raw["order_idx"] = float(normalization["order_choices"].index(defaults["order"]))
        raw["mesher_idx"] = float(normalization["mesher_choices"].index(defaults["mesher"]))
        if name == "imputed_defaults":
            # Exercise the C++ impute path: these columns are simply absent and
            # must be filled from normalization.json["impute"].
            for column in ("min_feature_h", "bc_grading", "kappa_max_h",
                           "case_traction_magnitude", "adapt_leb_waves"):
                raw.pop(column, None)
        cases.append({"name": name, "features": raw})
    return cases


def force_head_values(net: AdvisorNet, standardized: np.ndarray,
                      cases: list[dict[str, Any]], normalization: dict[str, Any],
                      clamps: dict[str, Any]) -> None:
    """Pin parity outputs, then pin a real candidate accuracy/DOF tradeoff.

    All heads first fit only the four exported parity rows, which keeps the
    cross-runtime system well-conditioned. The already-fitted accuracy and
    failure heads then identify two gate-passing candidate actions on the
    nominal case. Only the DOF head receives those two additional equations:
    the best-accuracy action is expensive and a lower-ranked action is cheap.
    This makes the C++ max_dof fixture non-vacuous without perturbing any other
    head's parity.
    """
    net.eval()
    with torch.no_grad():
        _, _, h2 = net.trunk(torch.from_numpy(standardized.astype(np.float32)))
    base_design = np.concatenate(
        [h2.numpy().astype(np.float64), np.ones((h2.shape[0], 1))], axis=1)
    if int(np.linalg.matrix_rank(base_design)) < base_design.shape[0]:
        raise SystemExit(
            f"tiny fixture trunk activations cannot force {base_design.shape[0]} outputs")

    def fit(matrix: np.ndarray, target: list[float]) -> tuple[np.ndarray, float]:
        solution, *_ = np.linalg.lstsq(
            matrix, np.asarray(target, dtype=np.float64), rcond=None)
        return solution[:-1], float(solution[-1])

    with torch.no_grad():
        for head in REGRESSION_HEADS:
            weight, bias = fit(base_design, TINY_FORCED[head])
            net.regression_heads[head].weight.copy_(
                torch.from_numpy(weight).view(1, -1).float())
            net.regression_heads[head].bias.fill_(bias)
        weight, bias = fit(base_design, TINY_FORCED["failure_logit"])
        net.failure_head.weight.copy_(torch.from_numpy(weight).view(1, -1).float())
        net.failure_head.bias.fill_(bias)
        groups = action_group_slices(net.config["mesher_choices"])
        start = groups["continuous"].start
        for offset, dim in enumerate(CONTINUOUS_ACTION_DIMS):
            weight, bias = fit(base_design, TINY_FORCED[f"policy_{dim}"])
            net.policy_head.weight[start + offset].copy_(torch.from_numpy(weight).float())
            net.policy_head.bias[start + offset] = bias

    candidate_inputs: list[np.ndarray] = []
    for action in clamps["candidate_grid"]["actions"]:
        raw = dict(cases[0]["features"])
        raw["h_rel"] = float(action["h_rel"])
        raw["adapt_passes"] = float(action["adapt_passes"])
        raw["eta_target"] = float(action["eta_target"])
        raw["order_idx"] = float(normalization["order_choices"].index(action["order"]))
        raw["mesher_idx"] = float(
            normalization["mesher_choices"].index(action["mesher"]))
        candidate_inputs.append(standardize_row(raw, normalization))
    candidate_matrix = np.stack(candidate_inputs).astype(np.float32)
    with torch.no_grad():
        _, _, candidate_h2 = net.trunk(torch.from_numpy(candidate_matrix))
        candidate_outputs = net.heads(candidate_h2)
    scores = candidate_outputs["rel_err_rel"].squeeze(1).numpy()
    risks = 1.0 / (
        1.0 + np.exp(-candidate_outputs["failure_logit"].squeeze(1).numpy()))
    survivors = np.flatnonzero(risks <= float(clamps["gate_threshold"]))
    if survivors.size < 2:
        raise SystemExit("tiny fixture has fewer than two gate-passing candidate actions")
    ranked = survivors[np.argsort(scores[survivors], kind="mergesort")]
    chosen = candidate_h2[ranked[:2]].numpy().astype(np.float64)
    dof_design = np.concatenate(
        [np.vstack([h2.numpy().astype(np.float64), chosen]),
         np.ones((base_design.shape[0] + 2, 1))],
        axis=1,
    )
    if int(np.linalg.matrix_rank(dof_design)) < dof_design.shape[0]:
        raise SystemExit("tiny fixture candidate tradeoff is rank deficient")
    weight, bias = fit(dof_design, TINY_FORCED["dof"] + [6.0, 3.0])
    with torch.no_grad():
        net.regression_heads["dof"].weight.copy_(
            torch.from_numpy(weight).view(1, -1).float())
        net.regression_heads["dof"].bias.fill_(bias)


def check_fixture_guarantees(cases: list[dict[str, Any]], clamps: dict[str, Any]) -> None:
    """Fail loudly if the fixture no longer exercises what the C++ test needs."""
    floor = float(clamps["h_rel"][0])
    veto = float(clamps["veto_threshold"])
    by_name = {case["name"]: case for case in cases}

    nominal = by_name["nominal"]["outputs"]
    lo, hi = clamps["h_rel"]
    if not (lo <= nominal["policy"][0] <= hi):
        raise SystemExit("fixture 'nominal' h_rel is outside the clamp box")
    if 1.0 / (1.0 + math.exp(-nominal["failure_logit"])) >= veto:
        raise SystemExit("fixture 'nominal' would be vetoed; it must not be")

    clamped = by_name["clamped_low_h_rel"]["outputs"]
    if clamped["policy"][0] >= floor:
        raise SystemExit(
            f"fixture 'clamped_low_h_rel' policy h_rel {clamped['policy'][0]} "
            f"is not below the clamp floor {floor}")
    if 1.0 / (1.0 + math.exp(-clamped["failure_logit"])) >= veto:
        raise SystemExit("fixture 'clamped_low_h_rel' must not be vetoed, or the clamp "
                         "path is never reached")

    vetoed = by_name["vetoed_failure"]["outputs"]
    if vetoed["failure_logit"] < 4.0:
        raise SystemExit(
            f"fixture 'vetoed_failure' logit {vetoed['failure_logit']} must be >= 4.0")


#: How far above the fixture's own worst in-distribution distance the fixture
#: threshold sits. Every fixture case must stay INSIDE it, or the four parity
#: cases would all be refused and the C++ test would only prove that a veto
#: which fires on everything fires.
TINY_OOD_MARGIN = 1.5


def tiny_ood(raw_matrix: np.ndarray, normalization: dict[str, Any]) -> dict[str, Any]:
    """OOD parameters for the tiny fixture, fitted exactly as the real ones are.

    Same :func:`advisor.calibration.fit_ood` the shipped ``bench/advisor/ood.json``
    comes from, on the fixture's own RAW rows, so the C++ loader is exercised
    against a real dense precision matrix and a real ``center``/``scale`` pair
    rather than an identity.

    Fitted over the intersection of ``OOD_FEATURE_COLUMNS`` with the fixture's own
    columns, deliberately and not by accident. The
    synthetic fixture cases are keyed by ``normalization.json:input_columns`` and
    carry no ``geo_*`` descriptors, so asking for them would make ``fit_ood``
    silently drop them -- the same quiet-subset failure this fixture exists to
    catch. The count is asserted below rather than trusted: a fixture that
    silently fitted 11 columns instead of 26 would still produce a plausible
    distance and would still pass every test.

    The operating point cannot be a training quantile here -- four rows have no
    99th percentile -- so it is placed above the worst fixture distance and
    asserted, which is what the C++ test needs: the four parity cases are in
    distribution, and a far row is not.
    """
    input_columns = list(normalization["input_columns"])
    expected = [name for name in OOD_FEATURE_COLUMNS if name in input_columns]
    # `raw_matrix` carries NaN wherever a synthetic case omits a column -- that is
    # deliberate, it is what exercises the impute path -- but a NaN propagates
    # through the covariance and makes the SVD diverge. Fill from the fixture's
    # own recorded impute values, i.e. the same medians the C++ would substitute.
    # This is sound HERE precisely because it is a synthetic fixture whose job is
    # to exercise the loader and the quadratic form; production never imputes an
    # OOD input, it refuses (see Advisor::Impl::mahalanobis).
    impute = np.asarray(normalization["impute"], dtype=np.float64)
    filled = np.asarray(raw_matrix, dtype=np.float64).copy()
    for column in range(filled.shape[1]):
        missing = ~np.isfinite(filled[:, column])
        filled[missing, column] = impute[column]
    params = fit_ood(filled, input_columns, expected)
    if params["feature_columns"] != expected:
        raise SystemExit(
            f"tiny fixture OOD fit resolved {len(params['feature_columns'])} columns, "
            f"expected {len(expected)}; a silently narrowed fit would still produce "
            f"plausible distances"
        )
    scores = ood_scores(params, filled, input_columns)
    threshold = float(scores.max()) * TINY_OOD_MARGIN
    if not (scores.max() < threshold):
        raise SystemExit("tiny fixture OOD threshold does not admit its own cases")
    params["operating_point"] = {
        "rule": "flag when mahalanobis distance exceeds the threshold",
        "threshold": threshold,
        "note": f"synthetic fixture operating point: {TINY_OOD_MARGIN}x the worst of the "
                f"{len(scores)} fixture cases (max {float(scores.max()):.6f}), so every "
                "parity case is in distribution and the C++ veto has a direction to be "
                "wrong in. The shipped bench/advisor/ood.json uses the validated "
                "training q0.99 instead.",
        "fixture_case_distances": [float(value) for value in scores],
    }
    return params


#: The activation fixture: the same tiny network, exported WITH the taps and
#: carrying the float64 tap tensors per case. Kept separate from
#: ``advisor_tiny`` on purpose. That directory is the evidence that a model
#: without taps still loads and recommends (``has_activations() == false``), so
#: tapping it in place would delete the back-compat fixture rather than add one.
EXPLAIN_FIXTURE_DIR = REPO_ROOT / "tests" / "fixtures" / "advisor_explain"


def build_fixture(args: argparse.Namespace, directory: Path, taps: bool,
                  flag: str) -> int:
    """Write one deterministic fixture directory.

    ``taps`` selects the graph shape and, with it, whether the parity cases
    carry the three float64 tap tensors. Everything else -- seed, synthetic
    table, forced heads, OOD fit -- is shared, so the two fixtures differ in
    exactly the thing under test and nothing else.
    """
    directory.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="advisor-tiny-") as tmp:
        csv_path = synthetic_csv(Path(tmp) / "tiny_dataset.csv")
        data = load_from_args(args, csv_path)

    config = data.model_config()
    config["hidden"] = 16
    config["emb_dim"] = 2
    net = AdvisorNet.from_config(config, seed=TINY_SEED)

    normalization = data.normalization
    clamps = data.clamps
    # The synthetic tradeoff points move the failure plane slightly. Keep the
    # fixture's candidate gate wide enough to retain multiple measured actions;
    # the residual veto remains 0.5 and still exercises the refusal arm.
    clamps["gate_threshold"] = 0.1
    cases = tiny_raw_cases(normalization)
    # Raw (pre-standardization) matrix; NaN marks a column the case omits, which
    # is exactly the impute path both standardize_row and the C++ loader take.
    raw_matrix = np.asarray(
        [[float(case["features"].get(column, np.nan))
          for column in normalization["input_columns"]]
         for case in cases], dtype=np.float64)
    standardized = np.stack([standardize_row(case["features"], normalization) for case in cases])
    force_head_values(net, standardized, cases, normalization, clamps)

    net.eval()
    with torch.no_grad():
        outputs = net.forward_tuple(torch.from_numpy(standardized.astype(np.float32)))
    tensors = dict(zip(net.output_names, outputs))
    # The taps are recorded in float64, from the same reference the parity check
    # scores the graph against, so the C++ has something better than the graph's
    # own float32 answer to compare its tap reads to.
    reference = float64_reference(net, standardized) if taps else ()
    for index, case in enumerate(cases):
        case["standardized_input"] = [float(value) for value in standardized[index]]
        case["outputs"] = {
            name: (float(tensors[name][index, 0]) if name != "policy"
                   else [float(value) for value in tensors["policy"][index]])
            for name in net.output_names
        }
        for offset, name in enumerate(ACTIVATION_OUTPUT_NAMES if taps else []):
            tap = reference[len(net.output_names) + offset]
            case[name] = [float(value) for value in tap[index]]
    check_fixture_guarantees(cases, clamps)

    model_path = directory / "model.onnx"
    layout_path = export_graph(net, model_path, taps)
    write_json(directory / "normalization.json", normalization)
    write_json(directory / "clamps.json", clamps)
    # The OOD block is required by the C++ loader, so the fixture must carry one
    # or every advisor test fails at construction. Fitted from the fixture's own
    # raw rows rather than copied from bench/advisor: a fixture that shared the
    # shipped parameters would stop being a self-contained unit.
    write_json(directory / "ood.json", tiny_ood(raw_matrix, normalization))
    parity = {
        "generator": f"scripts/advisor/export_onnx.py {flag}",
        "seed": TINY_SEED,
        "opset": OPSET,
        "tolerance": {
            "relative": PARITY_TOLERANCE,
            "formula": "abs(onnx - torch) / max(1, abs(torch)) <= relative",
            "note": "float32 GEMM differences scale with output magnitude; an "
                    "absolute bound of 1e-6 is not attainable for outputs above "
                    "about 8 in magnitude.",
        },
        "input_columns": list(normalization["input_columns"]),
        "action_dims": list(clamps["action_dims"]),
        "output_names": list(OUTPUT_NAMES),
    }
    if taps:
        parity["activation_outputs"] = list(ACTIVATION_OUTPUT_NAMES)
        parity["activation_reference"] = (
            "float64 torch AdvisorNet.trunk() on standardized_input; the graph is "
            "float32, so compare under tolerance.relative")
    parity["cases"] = cases
    write_json(directory / "parity.json", parity)

    parity_run = verify_parity(net, model_path, raw_matrix, normalization, taps)
    parity_run = parity_run.worst_of(verify_parity(
        net, model_path, sample_raw_batch(data, len(net.input_columns)),
        normalization, taps))

    print(f"wrote {model_path}")
    if layout_path is not None:
        print(f"wrote {layout_path}")
    print(f"wrote {directory / 'normalization.json'}")
    print(f"wrote {directory / 'clamps.json'}")
    print(f"wrote {directory / 'ood.json'}")
    print(f"wrote {directory / 'parity.json'}")
    print(f"opset={OPSET} D={len(net.input_columns)} A={len(net.action_dims)} "
          f"hidden={config['hidden']} params={net.n_parameters()}")
    print(f"outputs = {len(net.output_names)} contract"
          + (f" + {len(ACTIVATION_OUTPUT_NAMES)} taps {ACTIVATION_OUTPUT_NAMES}"
             if taps else " (no activation taps, by design)"))
    for case in cases:
        policy = case["outputs"]["policy"]
        print(f"  case {case['name']:<18} h_rel={policy[0]:+.6f} "
              f"failure_logit={case['outputs']['failure_logit']:+.4f} "
              f"rel_err={case['outputs']['rel_err']:+.4f}")
    print(f"verified with {parity_run.runtime}")
    for line in parity_run.report():
        print(line)
    total = sum(path.stat().st_size for path in sorted(directory.iterdir())
                if path.is_file())
    print(f"{directory} total {total} bytes")
    if not (parity_run.worst_rel <= PARITY_TOLERANCE):
        raise SystemExit(
            f"ONNX/torch parity failure: {parity_run.worst_rel:.3e} > "
            f"{PARITY_TOLERANCE:.0e} relative")
    return 0


def run_tiny_fixture(args: argparse.Namespace) -> int:
    return build_fixture(args, Path(args.fixture_dir) if args.fixture_dir else FIXTURE_DIR,
                         taps=False, flag="--tiny-fixture")


def run_explain_fixture(args: argparse.Namespace) -> int:
    return build_fixture(args,
                         Path(args.fixture_dir) if args.fixture_dir else EXPLAIN_FIXTURE_DIR,
                         taps=True, flag="--explain-fixture")

