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

This module owns the deployment export and the CLI; the graph writer, the
parity check and the fixture builders are ``export_graph``, ``export_parity``
and ``export_fixtures``.

Usage
-----
    python scripts/advisor/export_onnx.py
    python scripts/advisor/export_onnx.py --schema-from-checkpoint
    python scripts/advisor/export_onnx.py --tiny-fixture
    python scripts/advisor/export_onnx.py --explain-fixture
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np
import torch

if __package__ in (None, ""):  # direct `python scripts/advisor/export_onnx.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .calibration import OOD_FEATURE_COLUMNS, OOD_JSON
from .dataset import (
    CLAMPS_JSON,
    NORMALIZATION_JSON,
    AdvisorData,
    add_split_args,
    load_from_args,
    write_json,
)
from .export_fixtures import run_explain_fixture, run_tiny_fixture
from .export_graph import OPSET, export_graph
from .export_parity import PARITY_TOLERANCE, sample_raw_batch, verify_parity
from .model import ACTIVATION_OUTPUT_NAMES, AdvisorNet
from .paths import ADVISOR_DIR

MODEL_ONNX = ADVISOR_DIR / "model.onnx"
#: The shipped graph must be the shipped model: ``best.pt`` (best validation
#: ``rel_err_rel`` of the current stage), not the trainer's ``latest.pt``
#: resume point, which is simply the last run.
LATEST_CHECKPOINT = ADVISOR_DIR / "runs" / "latest.pt"
BEST_CHECKPOINT = ADVISOR_DIR / "runs" / "best.pt"


def default_checkpoint() -> Path:
    return BEST_CHECKPOINT if BEST_CHECKPOINT.is_file() else LATEST_CHECKPOINT


#: Keys in ``clamps.json`` that the C++ reads *after* inference. They affect no
#: tensor, so they may be retuned and re-exported without retraining; everything
#: else in that file describes the graph contract and may not.
DEPLOYMENT_CLAMP_KEYS = ("veto_threshold", "gate_threshold")



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
