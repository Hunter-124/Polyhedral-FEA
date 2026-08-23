#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Export the advisor network to ONNX and verify graph/torch parity (C4-C6).

Default mode exports ``bench/advisor/runs/best.pt`` to
``bench/advisor/model.onnx``, rewrites ``normalization.json`` and
``clamps.json`` from the same dataset pass and checks the ``ood.json`` beside
them, so the four files a C++ model directory needs can never drift apart. The
graph carries the nine C6 outputs first, then the three
``ACTIVATION_OUTPUT_NAMES`` trunk taps, and a tapped export drops an
``activation_layout.json`` sidecar beside the graph describing the same network
statically, so a consumer can draw what it is about to run.

``--tiny-fixture`` builds a deterministic miniature network on synthetic rows
and writes the C++ unit-test fixture directory ``tests/fixtures/advisor_tiny/``
containing ``model.onnx``, ``normalization.json``, ``clamps.json``,
``ood.json`` and ``parity.json``. The fixture is constructed — not hoped
for — so that it exercises the nominal, hard-clamp and failure-veto paths. It
is exported WITHOUT the taps on purpose: it is the fixture that proves a model
directory predating this feature still loads and recommends.

``--explain-fixture`` writes the same artifacts for the same network into
``tests/fixtures/advisor_explain/``, exported WITH the taps, plus
``activation_layout.json``; its ``parity.json`` cases additionally carry the
float64 ``trunk_input`` / ``trunk_fc1`` / ``trunk_fc2`` tensors so the C++ can
check its tap reads against PyTorch rather than against itself.

``--schema-from-checkpoint`` takes the input-column schema from the checkpoint
instead of the dataset CSV. It exists only to re-export a shipped checkpoint
whose corpus predates the current ``dataset.py`` (see ``run_export``); the
default path still refuses that case.

Usage
-----
    python scripts/advisor/export_onnx.py
    python scripts/advisor/export_onnx.py --schema-from-checkpoint
    python scripts/advisor/export_onnx.py --tiny-fixture
    python scripts/advisor/export_onnx.py --explain-fixture
"""

from __future__ import annotations

import argparse
import copy
import csv
import dataclasses
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

from .calibration import OOD_FEATURE_COLUMNS, OOD_JSON, fit_ood, ood_scores
from .dataset import (
    ADVISOR_DIR,
    CASE_COLUMNS,
    CLAMPS_JSON,
    CONTINUOUS_ACTION_DIMS,
    FEATURE_COLUMNS,
    GEOMETRY_FEATURE_COLUMNS,
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
from .model import ACTIVATION_OUTPUT_NAMES, AdvisorNet

MODEL_ONNX = ADVISOR_DIR / "model.onnx"
#: The shipped graph must be the shipped model: ``best.pt`` (best validation
#: ``rel_err_rel`` of the current stage), not the trainer's ``latest.pt``
#: resume point, which is simply the last run.
LATEST_CHECKPOINT = ADVISOR_DIR / "runs" / "latest.pt"
BEST_CHECKPOINT = ADVISOR_DIR / "runs" / "best.pt"


def default_checkpoint() -> Path:
    return BEST_CHECKPOINT if BEST_CHECKPOINT.is_file() else LATEST_CHECKPOINT


FIXTURE_DIR = ROOT / "tests" / "fixtures" / "advisor_tiny"

#: Keys in ``clamps.json`` that the C++ reads *after* inference. They affect no
#: tensor, so they may be retuned and re-exported without retraining; everything
#: else in that file describes the graph contract and may not.
DEPLOYMENT_CLAMP_KEYS = ("veto_threshold", "gate_threshold")

OPSET = 17
# Relative bound on |onnx_f32 - torch_f64| / max(1, |torch_f64|).
#
# Justified by float32 numerics, not chosen to make the check pass. float32 eps
# is 1.19e-07 and the widest reduction on the head path is the 96-wide trunk,
# so a single layer already admits ~sqrt(96)*eps ~ 1.17e-06 of accumulation
# error before the heads add more. Re-measured on the shipped model over the
# batch `sample_raw_batch` produces with no dataset (raw N(0,1), which reaches
# |z| ~ 1.7e02 after standardization and is therefore the harsher of the two
# batches this script uses): float32 PyTorch is 3.891e-06 (relative) from its
# own float64 result while ONNX Runtime is 3.078e-06 from it -- ORT is the more
# accurate of the two -- and the taps, one layer shallower, are 2.923e-06 and
# 2.864e-06. Over 16 real dataset rows the same figures are 2.215e-06 /
# 1.744e-06 and 8.56e-07 / 9.78e-07. Anything tighter than ~5e-06 would be
# demanding agreement below the noise floor of the computation itself; 1e-05
# clears the observed noise while still catching every real export defect (a
# wrong op, a wrong weight, or a permuted column order moves outputs by orders
# of magnitude, not by 1e-06).
PARITY_TOLERANCE = 1e-5


class ExportWrapper(nn.Module):
    """Adapts ``AdvisorNet`` to the flat tuple signature ONNX needs.

    ``taps`` selects the graph shape: ``forward_tuple_explain``, whose outputs
    are the twelve contract names followed by the three trunk taps, or the bare
    ``forward_tuple`` contract. Both shapes must remain exportable: the C++
    reports ``has_activations() == false`` and still recommends against an
    untapped model directory, and ``tests/fixtures/advisor_tiny/`` is the
    fixture that pins that path.
    """

    def __init__(self, net: AdvisorNet, taps: bool) -> None:
        super().__init__()
        self.net = net
        self.taps = taps

    def forward(self, features: Tensor) -> tuple[Tensor, ...]:
        if self.taps:
            return self.net.forward_tuple_explain(features)
        return self.net.forward_tuple(features)


#: Sidecar schema id. Bump the version if a consumer could misread the payload.
ACTIVATION_LAYOUT_SCHEMA = "polymesh.advisor.activation_layout/1"

#: Sidecar file name. Lives beside ``model.onnx`` in the same model directory,
#: because it describes that exact graph's weights and would be a lie next to
#: any other one.
ACTIVATION_LAYOUT_NAME = "activation_layout.json"


def activation_layout_path(model_path: Path) -> Path:
    return model_path.with_name(ACTIVATION_LAYOUT_NAME)


# --------------------------------------------------------------------------- #
# export + verification
# --------------------------------------------------------------------------- #

def graph_output_names(net: AdvisorNet, taps: bool = True) -> list[str]:
    """Graph output order: the twelve contract names, then the activation taps.

    The contract names stay first and keep their indices because the C++ loader
    validates every output ``i`` by name before accepting the graph.
    """
    return list(net.output_names) + (list(ACTIVATION_OUTPUT_NAMES) if taps else [])


def export_graph(net: AdvisorNet, path: Path, taps: bool = True) -> Path | None:
    """Write the contract graph -- one ``features`` input, twelve named outputs --
    with the three trunk taps appended, plus its ``activation_layout.json`` sidecar.

    Naming the taps as graph outputs costs the production ``Advisor::Impl::run`
    nothing. They are not new arithmetic: ``forward_tuple_explain`` shares one
    trunk evaluation with the heads (see ``AdvisorNet.heads``), so the taps are
    tensors the graph already had to materialise on the way to the twelve
    contract outputs. ORT computes and returns only the outputs the caller names
    in ``Run``, and the C++ names exactly the twelve; the extra graph outputs
    merely stop those three intermediates from being fusible or reusable buffers.

    ``taps=False`` writes the pre-activation graph shape and no sidecar, which
    is what a model directory looked like before this feature and what the C++
    back-compat path must keep loading.

    Returns the sidecar path, or ``None`` when untapped.
    """
    net.eval()
    path.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros(2, len(net.input_columns), dtype=torch.float32)
    output_names = graph_output_names(net, taps)
    dynamic_axes: dict[str, dict[int, str]] = {"features": {0: "batch"}}
    for name in output_names:
        dynamic_axes[name] = {0: "batch"}
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        torch.onnx.export(
            ExportWrapper(net, taps),
            (dummy,),
            str(path),
            input_names=["features"],
            output_names=output_names,
            dynamic_axes=dynamic_axes,
            opset_version=OPSET,
            do_constant_folding=True,
            dynamo=False,
        )
    if not taps:
        return None

    layout_path = activation_layout_path(path)
    # Written by the same call that writes the graph, never by a separate
    # command: a layout describing a different set of weights than the .onnx
    # beside it would draw a network nobody ran.
    layout = {
        "schema": ACTIVATION_LAYOUT_SCHEMA,
        "hidden": int(net.hidden),
        "activation_outputs": list(ACTIVATION_OUTPUT_NAMES),
    }
    layout.update(net.network_layout())
    write_json(layout_path, layout)
    return layout_path


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


@dataclasses.dataclass(frozen=True)
class ParityResult:
    """Worst deviation from the float64 reference, per output group.

    The C6 outputs and the activation taps are reported apart because they are
    two different claims. The contract numbers say the shipped predictions are
    right; the tap numbers say the drawn network is the one that produced them.
    A regression in either must be visible on its own, not averaged away by the
    other.
    """

    contract_rel: float = 0.0
    contract_abs: float = 0.0
    tap_rel: float = 0.0
    tap_abs: float = 0.0
    taps: bool = True     # False for a deliberately untapped graph
    runtime: str = ""

    @property
    def worst_rel(self) -> float:
        return max(self.contract_rel, self.tap_rel)

    @property
    def worst_abs(self) -> float:
        return max(self.contract_abs, self.tap_abs)

    def worst_of(self, other: "ParityResult") -> "ParityResult":
        """Element-wise worst of two runs of the same graph."""
        return ParityResult(
            contract_rel=max(self.contract_rel, other.contract_rel),
            contract_abs=max(self.contract_abs, other.contract_abs),
            tap_rel=max(self.tap_rel, other.tap_rel),
            tap_abs=max(self.tap_abs, other.tap_abs),
            taps=self.taps and other.taps,
            runtime=self.runtime or other.runtime,
        )

    def report(self) -> list[str]:
        """The parity summary lines, taps reported as their own claim."""
        lines = [f"max parity error, contract = {self.contract_rel:.3e} relative "
                 f"({self.contract_abs:.3e} absolute)"]
        if self.taps:
            lines.append(f"max parity error, taps     = {self.tap_rel:.3e} relative "
                         f"({self.tap_abs:.3e} absolute)")
        else:
            lines.append("activation taps            = not exported (untapped graph)")
        lines.append(f"tolerance {PARITY_TOLERANCE:.0e} relative, every group")
        return lines


def float64_reference(net: AdvisorNet, batch: np.ndarray) -> tuple[Tensor, ...]:
    """The net's own float64 evaluation of a *standardized* batch: the nine C6
    outputs, then the three trunk taps.

    Every parity claim in this script is scored against this, and the explain
    fixture records it verbatim, so the fixture and the check can never be
    quoting different references.
    """
    net.eval()
    with torch.no_grad():
        return copy.deepcopy(net).double().forward_tuple_explain(
            torch.from_numpy(batch.astype(np.float64)))


def verify_parity(net: AdvisorNet, path: Path, raw_batch: np.ndarray,
                  normalization: dict[str, Any], taps: bool = True) -> ParityResult:
    """Compare the exported graph against a float64 PyTorch reference.

    ``raw_batch`` is *un-standardized*; it is standardized here with
    ``normalization`` -- which must be the statistics that ship next to the
    graph -- so the check exercises the artifact triple (graph, normalization,
    clamps) as a unit rather than a pre-standardized matrix of unknown origin.

    The reference is the net evaluated in **float64**, not in float32. Scoring
    ONNX against float32 PyTorch asks two float32 implementations to agree more
    closely than either agrees with the exact answer, which is unsatisfiable:
    measured on this model, float32 PyTorch sits 2.215e-06 (relative) from its
    own float64 result while ONNX Runtime sits 1.744e-06 from it -- ORT is the
    *more* accurate of the two, and see ``PARITY_TOLERANCE`` for the same
    measurement on the harsher synthetic batch. Comparing both to float64 keeps
    the check sensitive to a real export defect (wrong op, wrong weights, wrong
    column order, all of which move outputs by orders of magnitude) while
    tolerating GEMM accumulation order, which is not a defect.

    The three activation taps are held to the same reference and the same
    tolerance, against ``AdvisorNet.trunk``'s float64 tensors. An unchecked tap
    would be worse than no tap: the cinema surface would draw confident
    per-neuron values with nothing asserting they are the network's own.

    ``taps`` must match how the graph was exported; a graph that should carry
    the taps and does not is a hard failure, exactly as a missing C6 output is.
    """
    batch = standardize_matrix(raw_batch, normalization)
    outputs, runtime = _run_onnx(path, batch)
    names = graph_output_names(net, taps)
    contract = len(net.output_names)
    missing = [name for name in names if name not in outputs]
    if missing:
        raise SystemExit(
            f"exported graph is missing outputs {missing}; it must carry the "
            f"{contract} C6 names"
            + (f" and the taps {list(ACTIVATION_OUTPUT_NAMES)}" if taps else ""))
    if not taps:
        # An untapped export must be untapped on purpose, not by a stale
        # wrapper: a tap that leaked in would silently change the graph shape
        # the back-compat fixture exists to pin.
        leaked = [name for name in ACTIVATION_OUTPUT_NAMES if name in outputs]
        if leaked:
            raise SystemExit(f"untapped export unexpectedly carries {leaked}")
    reference = float64_reference(net, batch)
    contract_rel = contract_abs = tap_rel = tap_abs = 0.0
    # `names` is shorter than `reference` for an untapped graph, so the zip
    # drops the tap tensors that were never exported.
    for index, (name, tensor) in enumerate(zip(names, reference)):
        truth = tensor.numpy().astype(np.float64)
        delta = np.abs(np.asarray(outputs[name], dtype=np.float64) - truth)
        if delta.size == 0:
            continue
        absolute = float(delta.max())
        relative = float((delta / np.maximum(1.0, np.abs(truth))).max())
        if index < contract:
            contract_abs = max(contract_abs, absolute)
            contract_rel = max(contract_rel, relative)
        else:
            tap_abs = max(tap_abs, absolute)
            tap_rel = max(tap_rel, relative)
    return ParityResult(contract_rel=contract_rel, contract_abs=contract_abs,
                        tap_rel=tap_rel, tap_abs=tap_abs, taps=taps, runtime=runtime)


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

def load_trained(checkpoint: Path,
                 data: AdvisorData | None) -> tuple[AdvisorNet, dict[str, Any]]:
    """Load the checkpoint's net; returns it with the full saved payload.

    ``data`` is the table the graph will be checked against. ``None`` means
    ``--schema-from-checkpoint``: take the schema from the checkpoint and skip
    the comparison below, which is the only way to re-export a shipped model
    whose corpus predates the current ``dataset.py``.
    """
    if not checkpoint.is_file():
        raise SystemExit(
            f"no trained checkpoint at {checkpoint}\n"
            f"train first: python scripts/advisor/train.py --runs 1"
        )
    payload = torch.load(checkpoint, map_location="cpu", weights_only=False)
    config = payload.get("config")
    if not config:
        raise SystemExit(f"checkpoint {checkpoint} has no model config")
    if data is not None:
        expected = data.model_config()
        if list(config["input_columns"]) != expected["input_columns"] or \
           list(config["action_dims"]) != expected["action_dims"]:
            raise SystemExit(
                "checkpoint schema does not match the current dataset "
                f"(D={len(config['input_columns'])}/{len(expected['input_columns'])}, "
                f"A={len(config['action_dims'])}/{len(expected['action_dims'])}); "
                f"retrain, or pass --schema-from-checkpoint to re-export the "
                f"checkpoint under its own schema."
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


def verify_ood(path: Path, net: AdvisorNet) -> dict[str, Any]:
    """Check the OOD block that ships beside the graph; never refit it.

    ``ood.json`` is fitted on the *corpus*, not on the graph, and carries its
    own standardizer over the geometry descriptors (see ``calibration.fit_ood``),
    so a change to the network's input schema cannot invalidate it. Refitting it
    here on today's table would silently move the veto boundary of a network
    trained on a different one -- a retrain decision, not an export decision.
    So this verifies instead: present, the right width, and the columns the C++
    descriptor builder emits. The C++ loader refuses a model directory without a
    usable OOD block, so an export that left one broken would ship a directory
    that cannot be loaded at all.
    """
    if not path.is_file():
        raise SystemExit(
            f"no OOD block at {path}; the C++ loader requires one. "
            f"Fit it: python scripts/advisor/calibration.py")
    params = json.loads(path.read_text(encoding="utf-8"))
    columns = params.get("feature_columns")
    if columns != list(OOD_FEATURE_COLUMNS):
        raise SystemExit(
            f"{path} was fitted over {len(columns or [])} columns, but the C++ "
            f"descriptor builder emits {len(OOD_FEATURE_COLUMNS)}; refit it: "
            f"python scripts/advisor/calibration.py")
    width = len(columns)
    for key in ("center", "scale"):
        if len(params.get(key, [])) != width:
            raise SystemExit(f"{path}: '{key}' is not {width} wide")
    if len(params.get("precision", [])) != width:
        raise SystemExit(f"{path}: 'precision' is not {width}x{width}")
    return params


def run_export(args: argparse.Namespace) -> int:
    checkpoint = Path(args.checkpoint) if args.checkpoint else default_checkpoint()

    # `--schema-from-checkpoint` takes the input-column schema from the
    # checkpoint and skips the dataset comparison entirely. It exists for one
    # measured situation: re-exporting a SHIPPED checkpoint whose dataset
    # generation predates the current `dataset.py`. HEAD's dataset.py declares
    # 62 INPUT_COLUMNS, while `runs/best.pt` and the shipped
    # bench/advisor/{model.onnx,normalization.json,clamps.json} are all the
    # 47-column schema, so the default path (correctly) refuses to export any of
    # them. That skew is a known, separate defect -- the shipped model needs a
    # retrain on the 62-column table -- and this flag does not fix it and must
    # not be used to paper over it.
    #
    # What makes the flag safe is that the re-export is provably the same
    # network: all 24 of `best.pt`'s state_dict tensors are bit-equal to the
    # shipped graph's initializers, and the nine contract outputs of the
    # re-exported graph are bit-identical to the previous graph's on the same
    # batch. Everything else is still verified below: parity against float64
    # PyTorch, the nine-name C6 contract, the three taps, and the
    # normalization/clamps/OOD triple, all taken from or checked against the
    # checkpoint's own schema so the four files cannot drift apart.
    #
    # The dataset is not read at all on this path, which is deliberate:
    # `bench/advisor/dataset.csv` is gitignored, so a command that needed it
    # could not regenerate a shipped artifact from a fresh clone.
    data = None if args.schema_from_checkpoint else load_from_args(args)
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
    if list(normalization.get("input_columns", [])) != list(net.input_columns):
        raise SystemExit(
            f"checkpoint {checkpoint} stores {len(normalization.get('input_columns', []))} "
            f"normalization columns for a {len(net.input_columns)}-input network; "
            f"the payload is inconsistent with its own graph")

    # `clamps.json` mixes two kinds of key. Vocabularies, boxes, defaults and
    # the candidate grid describe what the graph was trained to encode and
    # decode, so a change there really does invalidate the checkpoint. The two
    # thresholds do not: they are read by the C++ chooser *after* inference and
    # touch neither the graph nor the encoding, so retuning them must not force
    # a retrain. Conflating the two would mean a one-line operating-point
    # change could only ship behind hours of training, which is how the gate
    # ended up left at an unconsidered value in the first place.
    graph_affecting = {k: v for k, v in clamps.items() if k not in DEPLOYMENT_CLAMP_KEYS}

    # The operating point comes from the current table when there is one, and
    # from the checkpoint when there is not. Either way it is appended last, so
    # the file's key order is a property of this exporter rather than of which
    # path wrote it -- a re-export must not churn the artifact by reordering it.
    operating_point = clamps if data is None else data.clamps
    if data is None:
        drift: list[str] = []
    else:
        drift = normalization_drift(normalization, data.normalization)
        current_graph_affecting = {
            k: v for k, v in data.clamps.items() if k not in DEPLOYMENT_CLAMP_KEYS
        }
        if graph_affecting != current_graph_affecting:
            drift.append("clamps.json graph-affecting payload differs from the checkpoint's")

    clamps = dict(graph_affecting)
    for key in DEPLOYMENT_CLAMP_KEYS:
        if key not in operating_point:
            raise SystemExit(
                f"no '{key}' in {'the checkpoint' if data is None else 'dataset.py output'}; "
                f"the C++ requires it and refuses a clamps.json that omits it"
            )
        clamps[key] = operating_point[key]
    if drift:
        detail = "\n".join(f"  {line}" for line in drift)
        raise SystemExit(
            f"checkpoint {checkpoint} was trained under different normalization "
            f"statistics than {data.csv_path} produces now:\n{detail}\n"
            f"Exporting either one would mismatch the graph. Retrain on the "
            f"current table: python scripts/advisor/train.py --runs 1"
        )

    output = Path(args.output) if args.output else MODEL_ONNX
    layout_path = export_graph(net, output)
    write_json(NORMALIZATION_JSON, normalization)
    write_json(CLAMPS_JSON, clamps)
    ood = verify_ood(OOD_JSON, net)

    batch = sample_raw_batch(data, len(net.input_columns))
    parity = verify_parity(net, output, batch, normalization)

    source = ("the checkpoint's own schema (--schema-from-checkpoint)" if data is None
              else f"the current table {data.csv_path}")
    print(f"wrote {output}")
    print(f"wrote {layout_path}")
    print(f"wrote {NORMALIZATION_JSON}")
    print(f"wrote {CLAMPS_JSON}")
    print(f"checked {OOD_JSON} ({len(ood['feature_columns'])} columns, "
          f"{ood.get('n_train_rows', '?')} rows; fitted separately, not rewritten)")
    print(f"opset={OPSET} D={len(net.input_columns)} A={len(net.action_dims)} "
          f"hidden={net.hidden} params={net.n_parameters()}")
    print(f"outputs = {len(net.output_names)} contract + "
          f"{len(ACTIVATION_OUTPUT_NAMES)} taps {ACTIVATION_OUTPUT_NAMES}")
    print(f"schema from {source}")
    print(f"normalization from {checkpoint} (run {payload.get('run', '?')})")
    print(f"verified with {parity.runtime} on a {batch.shape[0]}x{batch.shape[1]} batch")
    for line in parity.report():
        print(line)
    if not (parity.worst_rel <= PARITY_TOLERANCE):
        raise SystemExit(
            f"ONNX/torch parity failure: {parity.worst_rel:.3e} > "
            f"{PARITY_TOLERANCE:.0e} relative")
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
EXPLAIN_FIXTURE_DIR = ROOT / "tests" / "fixtures" / "advisor_explain"


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


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", default=None, help="checkpoint to export (default runs/best.pt, else runs/latest.pt)")
    parser.add_argument("--output", default=None, help="output .onnx path (default bench/advisor/model.onnx)")
    parser.add_argument("--csv", default=None, help="dataset CSV override")
    add_split_args(parser)
    parser.add_argument("--schema-from-checkpoint", action="store_true",
                        help="take the input-column schema from the checkpoint instead of "
                             "the dataset CSV, to re-export a shipped model whose corpus "
                             "predates the current dataset.py")
    parser.add_argument("--tiny-fixture", action="store_true",
                        help="build the deterministic tests/fixtures/advisor_tiny/ "
                             "directory (no activation taps)")
    parser.add_argument("--explain-fixture", action="store_true",
                        help="build the deterministic tests/fixtures/advisor_explain/ "
                             "directory (activation taps + layout sidecar)")
    parser.add_argument("--fixture-dir", default=None, help="fixture output directory override")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    torch.set_num_threads(1)
    args = parse_args(argv)
    if args.tiny_fixture and args.explain_fixture:
        raise SystemExit("--tiny-fixture and --explain-fixture write different "
                         "directories; run them one at a time")
    if args.tiny_fixture:
        return run_tiny_fixture(args)
    if args.explain_fixture:
        return run_explain_fixture(args)
    return run_export(args)


if __name__ == "__main__":
    raise SystemExit(main())
