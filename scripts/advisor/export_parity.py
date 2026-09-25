# SPDX-License-Identifier: BSD-3-Clause
"""ONNX/torch parity: run the exported graph and compare it with the net's own
float64 evaluation of the same standardized batch."""
from __future__ import annotations

import copy
import dataclasses
from pathlib import Path
from typing import Any

import numpy as np
import torch
from torch import Tensor

from .dataset import AdvisorData, standardize_matrix
from .export_graph import graph_output_names
from .model import ACTIVATION_OUTPUT_NAMES, AdvisorNet

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

