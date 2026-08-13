#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Export the advisor network to ONNX and verify graph/torch parity (C4-C6).

Default mode exports ``bench/advisor/runs/latest.pt`` to
``bench/advisor/model.onnx`` and rewrites ``normalization.json`` and
``clamps.json`` from the same dataset pass, so the three files a C++ model
directory needs can never drift apart.

``--tiny-fixture`` builds a deterministic miniature network on synthetic rows
and writes the C++ unit-test fixture directory ``tests/fixtures/advisor_tiny/``
containing ``model.onnx``, ``normalization.json``, ``clamps.json`` and
``parity.json``. The fixture is constructed — not hoped for — so that it
exercises the nominal, hard-clamp and failure-veto paths.

Usage
-----
    python scripts/advisor/export_onnx.py
    python scripts/advisor/export_onnx.py --tiny-fixture
"""

from __future__ import annotations

import argparse
import copy
import csv
import json
import math
import sys
import tempfile
import warnings
from pathlib import Path
from typing import Any

import numpy as np
import torch
from torch import Tensor, nn

if __package__ in (None, ""):  # direct `python scripts/advisor/export_onnx.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import (
    ADVISOR_DIR,
    CASE_COLUMNS,
    CLAMPS_JSON,
    CONTINUOUS_ACTION_DIMS,
    FEATURE_COLUMNS,
    NORMALIZATION_JSON,
    OUTPUT_NAMES,
    REGRESSION_HEADS,
    ROOT,
    AdvisorData,
    action_group_slices,
    add_split_args,
    clamp_table,
    load_from_args,
    standardize_matrix,
    standardize_row,
    write_json,
)
from .model import AdvisorNet

MODEL_ONNX = ADVISOR_DIR / "model.onnx"
LATEST_CHECKPOINT = ADVISOR_DIR / "runs" / "latest.pt"
FIXTURE_DIR = ROOT / "tests" / "fixtures" / "advisor_tiny"

OPSET = 17
# Relative bound on |onnx_f32 - torch_f64| / max(1, |torch_f64|).
#
# Justified by float32 numerics, not chosen to make the check pass. float32 eps
# is 1.19e-07 and the widest reduction in the trunk is 64 wide, so a single
# layer already admits ~sqrt(64)*eps ~ 9.5e-07 of accumulation error before the
# heads add more. Measured on the trained model: float32 PyTorch is 2.148e-06
# from its own float64 result and ONNX Runtime is 1.597e-06 from it. Anything
# tighter than ~2e-06 would be demanding agreement below the noise floor of the
# computation itself; 1e-05 sits an order of magnitude above the observed noise
# and still catches every real export defect (a wrong op, a wrong weight, or a
# permuted column order moves outputs by orders of magnitude, not by 1e-06).
PARITY_TOLERANCE = 1e-5


class ExportWrapper(nn.Module):
    """Adapts ``AdvisorNet`` to the flat tuple signature ONNX needs."""

    def __init__(self, net: AdvisorNet) -> None:
        super().__init__()
        self.net = net

    def forward(self, features: Tensor) -> tuple[Tensor, ...]:
        return self.net.forward_tuple(features)


# --------------------------------------------------------------------------- #
# export + verification
# --------------------------------------------------------------------------- #

def export_graph(net: AdvisorNet, path: Path) -> None:
    """Write the C6 graph: one ``features`` input, eight named outputs."""
    net.eval()
    path.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros(2, len(net.input_columns), dtype=torch.float32)
    dynamic_axes: dict[str, dict[int, str]] = {"features": {0: "batch"}}
    for name in net.output_names:
        dynamic_axes[name] = {0: "batch"}
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        torch.onnx.export(
            ExportWrapper(net),
            (dummy,),
            str(path),
            input_names=["features"],
            output_names=list(net.output_names),
            dynamic_axes=dynamic_axes,
            opset_version=OPSET,
            do_constant_folding=True,
            dynamo=False,
        )


def _run_onnx(path: Path, batch: np.ndarray) -> tuple[dict[str, np.ndarray], str]:
    """Execute the exported graph, preferring onnxruntime over onnx.reference."""
    try:
        import onnxruntime  # type: ignore
    except ImportError:
        pass
    else:
        options = onnxruntime.SessionOptions()
        options.intra_op_num_threads = 1
        session = onnxruntime.InferenceSession(str(path), options,
                                               providers=["CPUExecutionProvider"])
        names = [output.name for output in session.get_outputs()]
        values = session.run(names, {"features": batch})
        return dict(zip(names, values)), f"onnxruntime {onnxruntime.__version__}"

    import onnx
    from onnx.reference import ReferenceEvaluator

    model = onnx.load(str(path))
    onnx.checker.check_model(model, full_check=True)
    evaluator = ReferenceEvaluator(model)
    names = [output.name for output in model.graph.output]
    values = evaluator.run(names, {"features": batch})
    return dict(zip(names, values)), f"onnx.reference {onnx.__version__}"


def verify_parity(net: AdvisorNet, path: Path, raw_batch: np.ndarray,
                  normalization: dict[str, Any]) -> tuple[float, float, str]:
    """Compare the exported graph against a float64 PyTorch reference.

    ``raw_batch`` is *un-standardized*; it is standardized here with
    ``normalization`` -- which must be the statistics that ship next to the
    graph -- so the check exercises the artifact triple (graph, normalization,
    clamps) as a unit rather than a pre-standardized matrix of unknown origin.

    The reference is the net evaluated in **float64**, not in float32. Scoring
    ONNX against float32 PyTorch asks two float32 implementations to agree more
    closely than either agrees with the exact answer, which is unsatisfiable:
    measured on this model, float32 PyTorch sits 2.148e-06 (relative) from its
    own float64 result while ONNX Runtime sits 1.597e-06 from it -- ORT is the
    *more* accurate of the two. Comparing both to float64 keeps the check
    sensitive to a real export defect (wrong op, wrong weights, wrong column
    order, all of which move outputs by orders of magnitude) while tolerating
    GEMM accumulation order, which is not a defect.

    Returns ``(worst_relative, worst_absolute, runtime)``; raises if the graph
    does not honour the C6 output contract.
    """
    batch = standardize_matrix(raw_batch, normalization)
    outputs, runtime = _run_onnx(path, batch)
    missing = [name for name in net.output_names if name not in outputs]
    if missing:
        raise SystemExit(f"exported graph is missing outputs {missing} (C6 violation)")
    net.eval()
    reference_net = copy.deepcopy(net).double()
    with torch.no_grad():
        reference = reference_net.forward_tuple(torch.from_numpy(batch.astype(np.float64)))
    worst_rel = 0.0
    worst_abs = 0.0
    for name, tensor in zip(net.output_names, reference):
        truth = tensor.numpy().astype(np.float64)
        delta = np.abs(np.asarray(outputs[name], dtype=np.float64) - truth)
        if delta.size == 0:
            continue
        worst_abs = max(worst_abs, float(delta.max()))
        worst_rel = max(worst_rel, float((delta / np.maximum(1.0, np.abs(truth))).max()))
    return worst_rel, worst_abs, runtime


SEED_BATCH = 9781


def sample_raw_batch(data: AdvisorData | None, n_inputs: int, rows: int = 16) -> np.ndarray:
    """A deterministic pseudo-random *raw* batch for the parity check.

    The split matrices are already standardized, so they are mapped back to raw
    units with the statistics that produced them; ``verify_parity`` then
    re-standardizes with whichever statistics are being exported.
    """
    generator = np.random.default_rng(SEED_BATCH)
    pool = np.concatenate([data.train.x, data.val.x], axis=0) if data is not None else None
    if pool is not None and pool.shape[0]:
        mean = np.asarray(data.normalization["mean"], dtype=np.float64)
        std = np.asarray(data.normalization["std"], dtype=np.float64)
        index = generator.integers(0, pool.shape[0], size=min(rows, pool.shape[0]))
        raw = pool[index].astype(np.float64) * std[None, :] + mean[None, :]
        # Jitter, in raw units, so the check is not just a replay of a training row.
        return raw + generator.normal(0.0, 0.05, size=raw.shape) * std[None, :]
    return generator.normal(0.0, 1.0, size=(rows, n_inputs))


# --------------------------------------------------------------------------- #
# default export
# --------------------------------------------------------------------------- #

def load_trained(checkpoint: Path, data: AdvisorData) -> tuple[AdvisorNet, dict[str, Any]]:
    """Load the checkpoint's net; returns it with the full saved payload."""
    if not checkpoint.is_file():
        raise SystemExit(
            f"no trained checkpoint at {checkpoint}\n"
            f"train first: python scripts/advisor/train.py --runs 1"
        )
    payload = torch.load(checkpoint, map_location="cpu", weights_only=False)
    config = payload.get("config")
    if not config:
        raise SystemExit(f"checkpoint {checkpoint} has no model config")
    expected = data.model_config()
    if list(config["input_columns"]) != expected["input_columns"] or \
       list(config["action_dims"]) != expected["action_dims"]:
        raise SystemExit(
            "checkpoint schema does not match the current dataset "
            f"(D={len(config['input_columns'])}/{len(expected['input_columns'])}, "
            f"A={len(config['action_dims'])}/{len(expected['action_dims'])}); retrain."
        )
    net = AdvisorNet(config)
    net.load_state_dict(payload["model"])
    net.eval()
    return net, payload


#: Freshly recomputed statistics come from the same float64 code path as the
#: ones the checkpoint stored, so an honest match is bit-exact; the tolerance
#: only absorbs json/pickle round-tripping, never a genuine dataset change.
STATS_TOLERANCE = 1e-12


def _numeric_drift(name: str, saved: Any, fresh: Any) -> str | None:
    saved_array = np.asarray(saved, dtype=np.float64)
    fresh_array = np.asarray(fresh, dtype=np.float64)
    if saved_array.shape != fresh_array.shape:
        return f"{name}: length {saved_array.size} in checkpoint vs {fresh_array.size} fresh"
    if not saved_array.size:
        return None
    delta = np.abs(saved_array - fresh_array)
    scale = np.maximum(1.0, np.abs(saved_array))
    worst = int(np.argmax(delta / scale))
    if delta[worst] / scale[worst] <= STATS_TOLERANCE:
        return None
    return (f"{name}[{worst}]: checkpoint {float(saved_array[worst]):.12g} vs "
            f"fresh {float(fresh_array[worst]):.12g}")


def normalization_drift(saved: dict[str, Any], fresh: dict[str, Any]) -> list[str]:
    """Human-readable differences between two ``normalization.json`` payloads."""
    drift: list[str] = []
    for key in ("input_columns", "passthrough_columns", "order_choices",
                "mesher_choices", "output_names", "action_dims", "regression_heads"):
        if saved.get(key) != fresh.get(key):
            drift.append(f"{key}: checkpoint {saved.get(key)!r} vs fresh {fresh.get(key)!r}")
    for key in ("mean", "std", "impute"):
        difference = _numeric_drift(key, saved.get(key, []), fresh.get(key, []))
        if difference:
            drift.append(difference)
    return drift


def run_export(args: argparse.Namespace) -> int:
    checkpoint = Path(args.checkpoint) if args.checkpoint else LATEST_CHECKPOINT
    data = load_from_args(args)
    net, payload = load_trained(checkpoint, data)

    # The graph was trained under the checkpoint's statistics, so those are the
    # only ones it is valid to ship. Recomputing them here (dataset.csv grows
    # between training and export in the normal batch workflow) would leave C++
    # standardizing with numbers the network has never seen.
    normalization = payload.get("normalization")
    clamps = payload.get("clamps")
    if not isinstance(normalization, dict) or not isinstance(clamps, dict):
        raise SystemExit(
            f"checkpoint {checkpoint} carries no normalization/clamps payload; "
            f"it predates the export contract. Retrain: "
            f"python scripts/advisor/train.py --runs 1"
        )
    drift = normalization_drift(normalization, data.normalization)
    if clamps != data.clamps:
        drift.append("clamps.json payload differs from the checkpoint's")
    if drift:
        detail = "\n".join(f"  {line}" for line in drift)
        raise SystemExit(
            f"checkpoint {checkpoint} was trained under different normalization "
            f"statistics than {data.csv_path} produces now:\n{detail}\n"
            f"Exporting either one would mismatch the graph. Retrain on the "
            f"current table: python scripts/advisor/train.py --runs 1"
        )

    output = Path(args.output) if args.output else MODEL_ONNX
    export_graph(net, output)
    write_json(NORMALIZATION_JSON, normalization)
    write_json(CLAMPS_JSON, clamps)

    batch = sample_raw_batch(data, len(net.input_columns))
    worst_rel, worst_abs, runtime = verify_parity(net, output, batch, normalization)

    print(f"wrote {output}")
    print(f"wrote {NORMALIZATION_JSON}")
    print(f"wrote {CLAMPS_JSON}")
    print(f"opset={OPSET} D={len(net.input_columns)} A={len(net.action_dims)} "
          f"params={net.n_parameters()}")
    print(f"normalization from {checkpoint} (run {payload.get('run', '?')})")
    print(f"verified with {runtime} on a {batch.shape[0]}x{batch.shape[1]} batch")
    print(f"max parity error = {worst_rel:.3e} relative "
          f"({worst_abs:.3e} absolute), tolerance {PARITY_TOLERANCE:.0e} relative")
    if not (worst_rel <= PARITY_TOLERANCE):
        raise SystemExit(
            f"ONNX/torch parity failure: {worst_rel:.3e} > {PARITY_TOLERANCE:.0e} relative")
    return 0


# --------------------------------------------------------------------------- #
# tiny fixture
# --------------------------------------------------------------------------- #

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


def force_head_values(net: AdvisorNet, standardized: np.ndarray) -> None:
    """Solve the final layer so the fixture cases hit the required outputs.

    The trunk is untouched; only the head rows are re-fitted. With
    ``n_cases << hidden`` the least-squares system is under-determined, so the
    fit is exact to float32 rounding.
    """
    net.eval()
    with torch.no_grad():
        _, _, h2 = net.trunk(torch.from_numpy(standardized.astype(np.float32)))
    design = np.concatenate(
        [h2.numpy().astype(np.float64), np.ones((h2.shape[0], 1))], axis=1)
    rank = int(np.linalg.matrix_rank(design))
    if rank < design.shape[0]:
        raise SystemExit(
            f"tiny fixture trunk activations are rank {rank} for {design.shape[0]} cases; "
            f"cannot force distinct head outputs")

    def fit(target: list[float]) -> tuple[np.ndarray, float]:
        solution, *_ = np.linalg.lstsq(design, np.asarray(target, dtype=np.float64), rcond=None)
        return solution[:-1], float(solution[-1])

    with torch.no_grad():
        for head in REGRESSION_HEADS:
            weight, bias = fit(TINY_FORCED[head])
            net.regression_heads[head].weight.copy_(torch.from_numpy(weight).view(1, -1).float())
            net.regression_heads[head].bias.fill_(bias)
        weight, bias = fit(TINY_FORCED["failure_logit"])
        net.failure_head.weight.copy_(torch.from_numpy(weight).view(1, -1).float())
        net.failure_head.bias.fill_(bias)
        groups = action_group_slices(net.config["mesher_choices"])
        start = groups["continuous"].start
        for offset, dim in enumerate(CONTINUOUS_ACTION_DIMS):
            weight, bias = fit(TINY_FORCED[f"policy_{dim}"])
            net.policy_head.weight[start + offset].copy_(torch.from_numpy(weight).float())
            net.policy_head.bias[start + offset] = bias


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


def run_tiny_fixture(args: argparse.Namespace) -> int:
    directory = Path(args.fixture_dir) if args.fixture_dir else FIXTURE_DIR
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
    cases = tiny_raw_cases(normalization)
    # Raw (pre-standardization) matrix; NaN marks a column the case omits, which
    # is exactly the impute path both standardize_row and the C++ loader take.
    raw_matrix = np.asarray(
        [[float(case["features"].get(column, np.nan))
          for column in normalization["input_columns"]]
         for case in cases], dtype=np.float64)
    standardized = np.stack([standardize_row(case["features"], normalization) for case in cases])
    force_head_values(net, standardized)

    net.eval()
    with torch.no_grad():
        outputs = net.forward_tuple(torch.from_numpy(standardized.astype(np.float32)))
    tensors = dict(zip(net.output_names, outputs))
    for index, case in enumerate(cases):
        case["standardized_input"] = [float(value) for value in standardized[index]]
        case["outputs"] = {
            name: (float(tensors[name][index, 0]) if name != "policy"
                   else [float(value) for value in tensors["policy"][index]])
            for name in net.output_names
        }
    check_fixture_guarantees(cases, clamps)

    model_path = directory / "model.onnx"
    export_graph(net, model_path)
    write_json(directory / "normalization.json", normalization)
    write_json(directory / "clamps.json", clamps)
    parity = {
        "generator": "scripts/advisor/export_onnx.py --tiny-fixture",
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
        "cases": cases,
    }
    write_json(directory / "parity.json", parity)

    worst_rel, worst_abs, runtime = verify_parity(net, model_path, raw_matrix, normalization)
    extra_rel, extra_abs, _ = verify_parity(
        net, model_path, sample_raw_batch(data, len(net.input_columns)), normalization)
    worst_rel = max(worst_rel, extra_rel)
    worst_abs = max(worst_abs, extra_abs)

    print(f"wrote {model_path}")
    print(f"wrote {directory / 'normalization.json'}")
    print(f"wrote {directory / 'clamps.json'}")
    print(f"wrote {directory / 'parity.json'}")
    print(f"opset={OPSET} D={len(net.input_columns)} A={len(net.action_dims)} "
          f"hidden={config['hidden']} params={net.n_parameters()}")
    for case in cases:
        policy = case["outputs"]["policy"]
        print(f"  case {case['name']:<18} h_rel={policy[0]:+.6f} "
              f"failure_logit={case['outputs']['failure_logit']:+.4f} "
              f"rel_err={case['outputs']['rel_err']:+.4f}")
    print(f"verified with {runtime}")
    print(f"max parity error = {worst_rel:.3e} relative "
          f"({worst_abs:.3e} absolute), tolerance {PARITY_TOLERANCE:.0e} relative")
    if not (worst_rel <= PARITY_TOLERANCE):
        raise SystemExit(
            f"ONNX/torch parity failure: {worst_rel:.3e} > {PARITY_TOLERANCE:.0e} relative")
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", default=None, help="checkpoint to export (default runs/latest.pt)")
    parser.add_argument("--output", default=None, help="output .onnx path (default bench/advisor/model.onnx)")
    parser.add_argument("--csv", default=None, help="dataset CSV override")
    add_split_args(parser)
    parser.add_argument("--tiny-fixture", action="store_true",
                        help="build the deterministic tests/fixtures/advisor_tiny/ directory")
    parser.add_argument("--fixture-dir", default=None, help="fixture output directory override")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    torch.set_num_threads(1)
    args = parse_args(argv)
    if args.tiny_fixture:
        return run_tiny_fixture(args)
    return run_export(args)


if __name__ == "__main__":
    raise SystemExit(main())
