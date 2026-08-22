#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Load ``bench/advisor/dataset.csv`` into standardized advisor tensors.

Implements contracts C2 (columns), C4 (``clamps.json`` / ``action_dims``) and
C5 (``normalization.json``).

Design notes
------------
* Only ``csv`` + ``numpy`` are used; the table is small and this keeps the
  loader free of a pandas dependency.
* Every regression head carries its own validity mask. A row contributes to a
  head only when that head's *raw* target is present and finite, and only when
  the row is not a failure. Failure rows are always kept for the failure head.
* The train/validation split is by geometry GROUP, never by row and never by
  the full ``part`` string. The corpus is parametric -- ``box_hole_s0_c0`` and
  ``box_hole_s0_c1`` are the same CAD solid under a different load case -- so a
  per-part split puts an identical geometry on both sides. See :func:`group_of`.
* ``order_idx`` / ``mesher_idx`` are *passthrough* columns (mean 0, std 1) so
  the plain ``(x - mean) / std`` loop in the C++ inference module stays valid
  for all D columns; the embedding lookup happens inside the graph.
"""

from __future__ import annotations

import csv
import hashlib
import json
import math
import os
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
from cost_labels import portable_cost_label

ROOT = Path(__file__).resolve().parents[2]
ADVISOR_DIR = ROOT / "bench" / "advisor"
DATASET_CSV = ADVISOR_DIR / "dataset.csv"
NORMALIZATION_JSON = ADVISOR_DIR / "normalization.json"
CLAMPS_JSON = ADVISOR_DIR / "clamps.json"

#: Only rows emitted by the advisor-aware testlab path carry the full C2
#: feature/action vector. Legacy campaign rows leave all 26 features and most
#: of the action columns empty, so training on them silently learns from
#: imputed constants; ``load_dataset`` rejects them outright.
ADVISOR_ROW_SCHEMA = "advisor-row-v3"

# --- C2 input columns -------------------------------------------------------

FEATURE_COLUMNS: list[str] = [
    "bbox_dx", "bbox_dy", "bbox_dz", "diag", "volume", "surface_area",
    "sa_over_v23", "n_faces", "n_sharp_edges", "sharp_edge_len_total",
    "curved_frac", "kappa_max_h", "kappa_mean_h", "thin_min_over_diag",
    "thin_p10_over_diag", "min_feature_h", "n_fix_faces", "n_load_faces",
    "fix_area_frac", "load_area_frac", "load_dir_x", "load_dir_y", "load_dir_z",
    "fix_load_dist_over_diag", "load_axis_alignment", "poisson",
    # Proximity / crease / singularity block, appended for the portable-cost
    # retrain in the order the contract fixes and `pipeline::CaseFeatures`
    # declares. Appended, never inserted: the C++ side reads inputs by name but
    # the ONNX graph is positional, so an insertion silently reindexes a shipped
    # model. The first ten are also emitted per PART by
    # `geometry_features.py`; `_load_geometry_features` drops those duplicates
    # because the per-ROW value comes from the shipped C++ extractor with the
    # case's own inputs, and two columns of the same name would leave one of
    # them permanently NaN.
    "geo_n_inner_loops", "geo_hole_spacing_min_rel", "geo_hole_spacing_p10_rel",
    "geo_feat_pair_dist_min_rel", "geo_feat_pair_dist_p10_rel",
    "geo_feat_pair_dist_mean_rel", "geo_dihedral_p10", "geo_dihedral_p50",
    "geo_dihedral_p90", "geo_singular_lambda_min",
    "load_to_feature_dist_min_rel", "fix_to_feature_dist_min_rel",
    "case_load_multiaxiality",
]
CASE_COLUMNS: list[str] = [
    "case_poisson", "case_n_fix_regions", "case_n_load_regions", "case_load_dir_x",
    "case_load_dir_y", "case_load_dir_z", "case_traction_magnitude",
]

#: Real per-part geometric descriptors, computed offline from the STEP files by
#: ``scripts/advisor/geometry_features.py`` and joined by geometry name. They
#: exist because the campaign's own geometry columns are largely dead: ten of
#: the original 44 inputs are constant across all 3,456 rows, and ``curved_frac``
#: is 1.0 in every single one because its formula saturates
#: (``apps/testlab/main.cpp:1709``). A model cannot prefer ``graded_tet`` on a
#: curved part when every part reports identical curvature, which is the most
#: likely reason matched-cost judgement measured worse than random.
#:
#: Empty when ``bench/advisor/geometry_features.csv`` is absent, so the loader
#: still works without the OCP binding.
GEOMETRY_FEATURES_CSV = ADVISOR_DIR / "geometry_features.csv"


def _load_geometry_features() -> tuple[list[str], dict[str, dict[str, float]]]:
    """Read the offline descriptor table, keyed by geometry name.

    Joined on ``<family>_s<n>`` rather than the full part id, because these are
    properties of the CAD solid and every load case of one solid shares them.

    Set ``ADVISOR_NO_GEOMETRY_FEATURES=1`` to ablate them. The with/without
    comparison is the experiment that says whether they help, so it has to be
    runnable from a flag rather than by moving the file out of the way.
    """
    if os.environ.get("ADVISOR_NO_GEOMETRY_FEATURES"):
        return [], {}
    if not GEOMETRY_FEATURES_CSV.is_file():
        return [], {}
    with GEOMETRY_FEATURES_CSV.open("r", newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        return [], {}
    # A name carried by both this table and the per-row feature vector is taken
    # from the ROW: `pipeline::extract_case_features` measured it on the same
    # BRep with the case's own inputs, while this table can only be a per-part
    # average. Keeping both would put the name twice in ``INPUT_COLUMNS``, and
    # since positions are resolved with ``list.index`` the second copy would
    # stay NaN forever and train as an imputed constant.
    names = [name for name in rows[0]
             if name != "part" and name not in FEATURE_COLUMNS]

    def cell(value: Any) -> float:
        try:
            number = float(str(value).strip())
        except (TypeError, ValueError):
            return math.nan
        return number if math.isfinite(number) else math.nan

    table = {
        str(row["part"]): {name: cell(row.get(name)) for name in names}
        for row in rows
    }
    return names, table


GEOMETRY_FEATURE_COLUMNS, GEOMETRY_FEATURE_TABLE = _load_geometry_features()
CONTINUOUS_ACTION_COLUMNS: list[str] = [
    # ``p_elevate`` is deliberately absent. It is not merely unvaried in the
    # corpus, it is redundant: ``apps/cli/main.cpp:805`` computes
    # ``p_elevate = decision.p_elevate || decision.order >= 2``, so it actuates
    # exactly what ``order >= 2`` already actuates. Advertising it as a separate
    # policy dimension claimed a control the engine does not have.
    "h_rel", "eta_target", "adapt_passes", "element_tendency",
    "skin_layers", "feature_refine", "bc_grading", "adapt_leb_waves",
]
CATEGORICAL_INDEX_COLUMNS: list[str] = ["order_idx", "mesher_idx"]

#: Scale-law inputs, derived per row rather than read from the CSV.
#:
#: Element count obeys ``n ~ volume / h^3`` and the cost heads are log10
#: targets, so the relationship the cost heads need is LINEAR in these four
#: and in nothing the raw columns offer: ``h`` itself is not an input at all
#: (only the dimensionless ``h_rel``), and ``volume`` spans several decades
#: across the corpus, which standardisation compresses into a spike.
#:
#: Measured on the clean regenerated dataset with a family-held-out split, the
#: net without these features predicted DOF to a validation MAE of 0.70 in
#: log10 -- a factor of five -- while a LightGBM baseline on the same split and
#: the same columns reached 0.059, because trees can recover a ratio by
#: splitting where a standardised MLP cannot.
DERIVED_FEATURE_COLUMNS: list[str] = [
    "log10_volume", "log10_diag", "log10_h", "log10_cells",
]


def derived_features(volume: float, diag: float, h_rel: float) -> dict[str, float]:
    """The four scale-law inputs. Non-positive or missing inputs give NaN,
    which the loader imputes like any other absent column."""

    def lg(value: float) -> float:
        return math.log10(value) if value > 0.0 and math.isfinite(value) else math.nan

    log_v = lg(volume)
    log_d = lg(diag)
    log_h = log_d + lg(h_rel)  # h = h_rel * diag, and the CSV carries no h_rel-free h
    return {
        "log10_volume": log_v,
        "log10_diag": log_d,
        "log10_h": log_h,
        "log10_cells": log_v - 3.0 * log_h,
    }


INPUT_COLUMNS: list[str] = (
    FEATURE_COLUMNS + GEOMETRY_FEATURE_COLUMNS + CASE_COLUMNS
    + CONTINUOUS_ACTION_COLUMNS + DERIVED_FEATURE_COLUMNS + CATEGORICAL_INDEX_COLUMNS
)
PASSTHROUGH_COLUMNS: list[str] = list(CATEGORICAL_INDEX_COLUMNS)

# --- heads ------------------------------------------------------------------

REGRESSION_HEADS: list[str] = [
    "rel_err", "rel_err_rel", "geo_chamfer", "geo_p99", "dof", "mesh_ms",
    "solve_ms", "solve_flops", "solve_bytes", "mesh_work",
]
ACCURACY_HEADS: list[str] = ["rel_err", "rel_err_rel", "geo_chamfer", "geo_p99"]
COST_HEADS: list[str] = [
    "dof", "mesh_ms", "solve_ms", "solve_flops", "solve_bytes", "mesh_work",
]
OUTPUT_NAMES: list[str] = REGRESSION_HEADS + ["failure_logit", "policy"]
HEAD_NAMES: list[str] = REGRESSION_HEADS + ["failure"]

#: Heads whose target is the raw value minus that case's median over the
#: actions actually run. The absolute level of `rel_err` is set largely by how
#: good a case's reference truth is -- an offset a held-out part never shows
#: the model, which is why the absolute head does not generalize. Choosing a
#: mesh only ever needs the ORDERING of actions within one case, and that
#: ordering survives the centring.
CENTRED_HEADS: dict[str, str] = {"rel_err_rel": "rel_err"}

#: head -> (source csv column, floor applied before log10)
TARGET_SOURCES: dict[str, tuple[str, float]] = {
    "rel_err": ("accuracy_rel_err", 1e-12),
    "geo_chamfer": ("geo_fidelity_chamfer_mean", 1e-12),
    # p95 collapses to ~0 on any conforming mesh (boundary nodes sit on the
    # B-rep), so the tail-sensitive p99 is the head that actually carries
    # signal. The p95 column stays in the dataset as recorded evidence.
    "geo_p99": ("geo_fidelity_dist_p99", 1e-12),
    "dof": ("n_dof", 1.0),
    "mesh_ms": ("mesh_ms", 1e-3),
    "solve_ms": ("solve_ms", 1e-3),
    "solve_flops": ("solve_flops", 1.0),
    "solve_bytes": ("solve_bytes", 1.0),
    "mesh_work": ("mesh_work", 1e-12),
}

#: statuses that are *not* a failure (C7 failure head definition)
OK_STATUSES: frozenset[str] = frozenset({"ok", "solve_suspect", "cost_only"})

# --- C4 clamp box -----------------------------------------------------------

#: Only orders 1 and 2 exist. ``fea::promote_to_quadratic``
#: (``src/fea/include/fea/p_elevate.hpp:33-38``) is a single linear->quadratic
#: step, and ``apps/cli/main.cpp:814-818`` warns and downgrades anything higher.
#: The vocabulary previously advertised 3 and 4, so the policy head spent two of
#: its ten outputs on actions that could never be performed.
ORDER_CHOICES: list[int] = [1, 2]
CONTINUOUS_ACTION_DIMS: list[str] = ["h_rel", "adapt_passes", "eta_target"]
CLAMP_BOX: dict[str, tuple[float, float]] = {
    "h_rel": (0.005, 0.2),
    # The plan's floor of 0.005 excluded eta_target's own default. In the
    # harness 0.0 means "no adaptive error target", which is a legal and common
    # action (every adapt_passes=0 row uses it), so the box has to contain it —
    # otherwise the clamp would silently turn adaptivity on.
    "eta_target": (0.0, 0.3),
    "adapt_passes": (0.0, 6.0),
}
#: Refuses the whole recommendation after the fact — an abstention.
VETO_THRESHOLD = 0.5
#: Drops individual candidates before ranking. A DIFFERENT decision from the
#: veto, and deliberately a different number: the C++ used to inherit this from
#: `veto_threshold` when the key was absent, which shipped the gate at 0.5 —
#: the weakest member of its own sweep — while looking deliberate. The C++ now
#: rejects a clamps.json that omits it.
#:
#: 0.05 is chosen on pick-failure rate, not regret. Across thresholds 0.05–0.8
#: held-out regret spans only 0.3233–0.3350 (leave-one-family-out, 8 families,
#: 5 seeds), so regret does not single out any threshold — 0.2 is nominally best
#: by 0.012 decades. Pick-failure does separate them: 27.5 % at 0.05 against
#: 31.3 % at 0.2 and 31.2 % at 0.5. Given the failure head's calibration is
#: mediocre (ECE 0.263), a rule that avoids doomed picks is worth more to a user
#: than a hair of median accuracy inside the noise band.
GATE_THRESHOLD = 0.05
ACTION_DEFAULTS: dict[str, Any] = {
    "mesher": "hybrid_zoo",
    "h_rel": 0.1,
    "order": 1,
    "adapt_passes": 0,
    "eta_target": 0.0,
}

STD_FLOOR = 1e-9


# --------------------------------------------------------------------------- #
# CSV parsing helpers
# --------------------------------------------------------------------------- #

def to_float(raw: Any) -> float:
    """Parse a CSV cell into a float, mapping blanks/booleans/garbage sanely."""
    if raw is None:
        return math.nan
    if isinstance(raw, bool):
        return 1.0 if raw else 0.0
    if isinstance(raw, (int, float)):
        value = float(raw)
        return value if math.isfinite(value) else math.nan
    text = raw.strip()
    if not text:
        return math.nan
    lowered = text.lower()
    if lowered in ("true", "yes"):
        return 1.0
    if lowered in ("false", "no"):
        return 0.0
    if lowered in ("nan", "none", "null", "na"):
        return math.nan
    try:
        value = float(text)
    except ValueError:
        return math.nan
    return value if math.isfinite(value) else math.nan


def to_bool(raw: Any) -> bool | None:
    """Tri-state boolean parse: ``None`` when the cell is blank/unknown."""
    if raw is None:
        return None
    if isinstance(raw, bool):
        return raw
    text = str(raw).strip().lower()
    if text in ("true", "1", "yes"):
        return True
    if text in ("false", "0", "no"):
        return False
    return None


def row_key(row: dict[str, str]) -> str:
    """Stable identity for a dataset row, used by the pruning ledger."""
    return "|".join(
        str(row.get(name, "")).strip()
        for name in ("campaign", "cfg_id", "part", "tier")
    )


#: parts are named ``<family>_s<shape>_c<case>``; ``_s\d+_c\d+`` is the suffix
#: that distinguishes a shape variant and a load case of one family.
PART_SUFFIX_RE = re.compile(r"_s\d+_c\d+$")
CASE_SUFFIX_RE = re.compile(r"_c\d+$")

#: How :func:`load_dataset` groups parts before holding a fold out.
#:
#: ``family``   all shape variants and load cases of one base geometry, e.g.
#:              every ``box_hole_*``. Six groups today. This is the only split
#:              that measures generalization to an unseen geometry family, and
#:              it is the default because it is the only defensible one.
#: ``geometry`` one CAD solid, its load cases held together, e.g. every
#:              ``box_hole_s0_*``. Twenty-four groups today. Weaker: a held-out
#:              geometry still has three siblings from its family in train.
#: ``part``     one (geometry, load case) pair. Measured on the v3 corpus,
#:              *every* held-out row then has a row in train with an identical
#:              geometry-feature and action vector, so this mode exists only to
#:              reproduce the leakage it causes and must never ship a number.
SPLIT_MODES: tuple[str, ...] = ("family", "geometry", "part")
DEFAULT_SPLIT_MODE = "family"


def family_of(part: str) -> str:
    """``box_hole_s0_c1`` -> ``box_hole``."""
    return PART_SUFFIX_RE.sub("", part)


def geometry_of(part: str) -> str:
    """``box_hole_s0_c1`` -> ``box_hole_s0`` (one CAD solid, any load case)."""
    return CASE_SUFFIX_RE.sub("", part)


def group_of(part: str, mode: str = DEFAULT_SPLIT_MODE) -> str:
    """The hold-out group a part belongs to under ``mode``."""
    if mode == "family":
        return family_of(part)
    if mode == "geometry":
        return geometry_of(part)
    if mode == "part":
        return part
    raise ValueError(f"unknown split mode {mode!r}; expected one of {SPLIT_MODES}")


def split_groups(parts: list[str], mode: str = DEFAULT_SPLIT_MODE) -> list[str]:
    """Sorted unique hold-out groups present in ``parts``."""
    return sorted({group_of(part, mode) for part in parts})


# --------------------------------------------------------------------------- #
# Split container
# --------------------------------------------------------------------------- #

@dataclass
class Split:
    """One side of the part-hash split, fully materialized as numpy arrays."""

    name: str
    keys: list[str] = field(default_factory=list)
    parts: list[str] = field(default_factory=list)
    cfg_ids: list[str] = field(default_factory=list)
    x: np.ndarray = field(default_factory=lambda: np.zeros((0, 0), dtype=np.float32))
    targets: dict[str, np.ndarray] = field(default_factory=dict)
    masks: dict[str, np.ndarray] = field(default_factory=dict)
    failure: np.ndarray = field(default_factory=lambda: np.zeros(0, dtype=np.float32))
    policy_target: np.ndarray = field(default_factory=lambda: np.zeros((0, 0), dtype=np.float32))
    policy_mask: np.ndarray = field(default_factory=lambda: np.zeros((0, 0), dtype=bool))

    @property
    def n_rows(self) -> int:
        return int(self.x.shape[0])

    def select(self, keep: np.ndarray) -> "Split":
        """Return a row-filtered copy (used to apply the pruning ledger)."""
        keep = np.asarray(keep, dtype=bool)
        index = np.flatnonzero(keep)
        return Split(
            name=self.name,
            keys=[self.keys[i] for i in index],
            parts=[self.parts[i] for i in index],
            cfg_ids=[self.cfg_ids[i] for i in index],
            x=self.x[keep],
            targets={head: values[keep] for head, values in self.targets.items()},
            masks={head: values[keep] for head, values in self.masks.items()},
            failure=self.failure[keep],
            policy_target=self.policy_target[keep],
            policy_mask=self.policy_mask[keep],
        )


@dataclass
class AdvisorData:
    """Everything ``train.py`` / ``export_onnx.py`` need from the CSV."""

    train: Split
    val: Split
    normalization: dict[str, Any]
    clamps: dict[str, Any]
    input_columns: list[str]
    action_dims: list[str]
    order_choices: list[int]
    mesher_choices: list[str]
    n_rows: int
    csv_path: Path
    #: Which hold-out grouping produced ``train``/``val``, and which fold of it.
    #: Persisted into every checkpoint and report so a number can never be read
    #: without knowing how hard the split that produced it was.
    split_mode: str = DEFAULT_SPLIT_MODE
    fold: int = 0
    n_folds: int = 1
    val_groups: list[str] = field(default_factory=list)

    @property
    def n_inputs(self) -> int:
        return len(self.input_columns)

    @property
    def n_actions(self) -> int:
        return len(self.action_dims)

    def model_config(self) -> dict[str, Any]:
        """Architecture descriptor persisted inside every checkpoint."""
        return model_config(self.input_columns, self.action_dims,
                            self.order_choices, self.mesher_choices)


def model_config(input_columns: list[str], action_dims: list[str],
                 order_choices: list[int], mesher_choices: list[str],
                 hidden: int = 96, emb_dim: int = 4) -> dict[str, Any]:
    """Build the ``AdvisorNet`` construction descriptor.

    ``hidden`` is deliberately small. `capacity_sweep.py` measured widths from
    32 to 512 and depths 2 to 4 on this data: on the centred accuracy target
    validation MAE stays in 0.29-0.38 across a 320x parameter range, so extra
    capacity buys nothing, and on the *absolute* target it actively hurts
    (val 0.99 at 2.5k params, 1.11 at 811k, while train falls 0.093 -> 0.007).
    96 keeps the neuron map in the dashboard legible.
    """
    return {
        "input_columns": list(input_columns),
        "action_dims": list(action_dims),
        "order_choices": list(order_choices),
        "mesher_choices": list(mesher_choices),
        "order_column": input_columns.index("order_idx"),
        "mesher_column": input_columns.index("mesher_idx"),
        "hidden": hidden,
        "emb_dim": emb_dim,
        "output_names": list(OUTPUT_NAMES),
    }


# --------------------------------------------------------------------------- #
# Action encoding (C4)
# --------------------------------------------------------------------------- #

def build_action_dims(mesher_choices: list[str]) -> list[str]:
    """The policy head's output layout: 3 continuous dims then two argmax blocks.

    Width is ``3 + len(ORDER_CHOICES) + len(mesher_choices)`` = 7 today. It was
    10 while a ``p_elevate_logit`` and two unreachable order logits were carried.
    """
    dims = ["h_rel", "adapt_passes", "eta_target"]
    dims += [f"order_logit_{value}" for value in ORDER_CHOICES]
    dims += [f"mesher_logit_{name}" for name in mesher_choices]
    return dims


def action_group_slices(mesher_choices: list[str]) -> dict[str, slice]:
    """Index ranges of each logical group inside the action vector."""
    n_continuous = len(CONTINUOUS_ACTION_DIMS)
    n_order = len(ORDER_CHOICES)
    n_mesher = len(mesher_choices)
    return {
        "continuous": slice(0, n_continuous),
        "order": slice(n_continuous, n_continuous + n_order),
        "mesher": slice(n_continuous + n_order, n_continuous + n_order + n_mesher),
    }


def candidate_grid(rows: list[dict[str, str]], mesher_choices: list[str],
                   max_candidates: int = 128) -> dict[str, Any]:
    """The explicit list of actions a deployed chooser enumerates and scores.

    A LIST of measured actions, not a cross product of per-dial levels. The
    difference matters twice over.

    First, a cross product invents combinations. The corpus runs
    ``adapt_passes = 0`` only at ``h_rel = 0.12``, so crossing the dials would
    manufacture "no adaptivity at h_rel = 0.08" and ask the regression heads to
    extrapolate to it. That is the failure mode that produced
    ``predicted_dof = 1.5e15`` on an unseen part. Every action here was actually
    run, so no query leaves the training support.

    Second, it lets provably inert dials be collapsed. Measured on this corpus,
    ``order`` has NO effect when ``adapt_passes > 0``: of 264 matched pairs
    differing only in ``order``, 264 are bit-identical in ``n_dof``, ``n_nodes``
    and ``rel_err``, because that path takes the adaptive driver's marked p-set
    and never consults ``cfg.order`` (``src/pipeline/src/scene.cpp:4408``). Those
    candidates are duplicates and are dropped -- 26 distinct measured tuples
    collapse to 20. ``eta_target`` is NOT collapsed: at 193 of 237 matched pairs
    it is inert too, but not always, so dropping it would discard real actions.

    ``max_candidates`` is a latency budget: each action costs one forward pass in
    the C++ chooser, against roughly 2 for the retired single-shot rule.
    """
    seen: dict[tuple[Any, ...], int] = {}
    for row in rows:
        mesher = str(row.get("mesher", "") or "").strip()
        if mesher not in mesher_choices:
            continue
        order = to_float(row.get("order"))
        h_rel = to_float(row.get("h_rel"))
        passes = to_float(row.get("adapt_passes"))
        eta = to_float(row.get("eta_target"))
        if not all(math.isfinite(v) for v in (order, h_rel, passes, eta)):
            continue
        order_int = int(round(order))
        passes_int = int(round(passes))
        if order_int not in ORDER_CHOICES:
            continue
        # Collapse the inert order dial rather than scoring duplicate actions.
        if passes_int > 0:
            order_int = ORDER_CHOICES[0]
        key = (mesher, order_int, round(h_rel, 6), passes_int, round(eta, 6))
        seen[key] = seen.get(key, 0) + 1

    actions = [
        {"mesher": m, "order": o, "h_rel": h, "adapt_passes": p, "eta_target": e,
         "measured_rows": n}
        for (m, o, h, p, e), n in sorted(seen.items(), key=lambda kv: kv[0])
    ]
    grid: dict[str, Any] = {
        "actions": actions,
        "n_candidates": len(actions),
        "order_collapsed_when_adapt_passes_positive": True,
        "collapse_evidence": ("264 of 264 matched pairs differing only in order at "
                              "adapt_passes > 0 are bit-identical in n_dof, n_nodes "
                              "and rel_err"),
        # Kept for readability and for the figures generator; the C++ side reads
        # `actions`, never these.
        "observed_levels": {
            "h_rel": sorted({a["h_rel"] for a in actions}),
            "adapt_passes": sorted({a["adapt_passes"] for a in actions}),
            "eta_target": sorted({a["eta_target"] for a in actions}),
            "order": sorted({a["order"] for a in actions}),
            "mesher": sorted({a["mesher"] for a in actions}),
        },
    }
    if not actions:
        raise SystemExit("candidate grid is empty; the dataset has no usable actions")
    if len(actions) > max_candidates:
        raise SystemExit(
            f"candidate grid has {len(actions)} actions, over the {max_candidates} "
            "ceiling; each one costs a forward pass in the C++ chooser"
        )
    return grid


def clamp_table(mesher_choices: list[str],
                rows: list[dict[str, str]] | None = None) -> dict[str, Any]:
    """The C4 ``clamps.json`` payload."""
    # The default action is what a feasibility veto falls back to, so it has to
    # be a legal action for THIS model: a default mesher absent from the trained
    # vocabulary would hand the C++ side a name its own clamp table rejects.
    defaults = dict(ACTION_DEFAULTS)
    if defaults["mesher"] not in mesher_choices:
        defaults["mesher"] = mesher_choices[0]
    if defaults["order"] not in ORDER_CHOICES:
        defaults["order"] = ORDER_CHOICES[0]
    lo, hi = CLAMP_BOX["h_rel"]
    defaults["h_rel"] = min(max(float(defaults["h_rel"]), lo), hi)
    lo, hi = CLAMP_BOX["eta_target"]
    defaults["eta_target"] = min(max(float(defaults["eta_target"]), lo), hi)
    return {
        "h_rel": list(CLAMP_BOX["h_rel"]),
        "eta_target": list(CLAMP_BOX["eta_target"]),
        "adapt_passes": [int(CLAMP_BOX["adapt_passes"][0]), int(CLAMP_BOX["adapt_passes"][1])],
        "order_choices": list(ORDER_CHOICES),
        "mesher_choices": list(mesher_choices),
        "action_dims": build_action_dims(mesher_choices),
        "veto_threshold": VETO_THRESHOLD,
        "gate_threshold": GATE_THRESHOLD,
        "candidate_grid": candidate_grid(rows, mesher_choices) if rows else None,
        "defaults": defaults,
    }


def continuous_box_halfwidths() -> np.ndarray:
    """Barrier half-widths for the three continuous policy dims.

    The penalty is ``beta * sum(relu(|value| - halfwidth))``, i.e. a box
    centred on the origin, so the half-width of dim *d* is
    ``max(|lo|, |hi|)`` of its clamp interval.
    """
    return np.asarray(
        [max(abs(CLAMP_BOX[name][0]), abs(CLAMP_BOX[name][1])) for name in CONTINUOUS_ACTION_DIMS],
        dtype=np.float32,
    )


# --------------------------------------------------------------------------- #
# Loading
# --------------------------------------------------------------------------- #

def read_rows(csv_path: Path) -> list[dict[str, str]]:
    if not csv_path.is_file():
        raise SystemExit(
            f"advisor dataset missing: {csv_path}\n"
            f"build it with: python scripts/build_advisor_dataset.py"
        )
    with csv_path.open("r", newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def _failure_flag(row: dict[str, str]) -> float:
    """Did the engine fail to deliver a usable solve?

    Deliberately NOT a function of ``accuracy_trusted``. That column is the
    solve HEALTH gate -- ``apps/testlab/main.cpp:2525`` sets it to ``health_ok``,
    and ``main.cpp:2562`` sets ``status = health_ok ? "ok" : "solve_suspect"``
    from the same flag, so ``accuracy_trusted == false`` is exactly
    ``status == "solve_suspect"`` (verified: 144 of 144 rows on the v3 corpus).

    The old definition was therefore self-contradictory: ``OK_STATUSES``
    explicitly whitelists ``solve_suspect`` as not-a-failure, and then the
    ``accuracy_trusted`` clause re-flagged every one of those rows as a failure.
    The cost of that was real. A residual- or reaction-gate wobble made the row
    a "failure" for the feasibility head, which is meant to predict "will this
    mesh and solve at all", and simultaneously discarded its ``n_dof``,
    ``mesh_ms``, ``solve_ms`` and B-rep distances -- none of which depend on the
    health gate or on the accuracy reference at all.

    Health failures are still worth excluding from the ACCURACY heads, because a
    solve that failed its residual check has an untrustworthy answer. That is
    what :func:`_trust_flag` does, and it stops there.
    """
    status = str(row.get("status", "") or "").strip()
    error = str(row.get("error", "") or "").strip()
    if error and error.lower() not in OK_STATUSES:
        return 1.0
    if status and status.lower() not in OK_STATUSES:
        return 1.0
    return 0.0


def _trust_flag(row: dict[str, str]) -> bool:
    """Did this row's solve pass its health gate, so its answer can be believed?

    Masks the accuracy heads only. Note this is a statement about the SOLVE, not
    about how close the answer landed to the reference: a coarse mesh that is
    50 % off is a perfectly trustworthy measurement of a bad action, and it is
    exactly the signal the advisor has to learn from. Masking on closeness would
    be selecting on the outcome. A blank column is treated as trusted; only an
    explicit ``false`` is not.
    """
    return to_bool(row.get("accuracy_trusted")) is not False


def _raw_matrix(rows: list[dict[str, str]], mesher_choices: list[str]) -> np.ndarray:
    """Raw (un-imputed, un-standardized) input matrix, NaN where missing."""
    n = len(rows)
    x = np.full((n, len(INPUT_COLUMNS)), np.nan, dtype=np.float64)
    plain = FEATURE_COLUMNS + CASE_COLUMNS + CONTINUOUS_ACTION_COLUMNS
    # Column positions are looked up by NAME, never by enumeration order: the
    # geometry descriptors sit between FEATURE_COLUMNS and CASE_COLUMNS in
    # INPUT_COLUMNS, so a positional loop over `plain` would silently write every
    # case and action value into the wrong column.
    plain_cols = [INPUT_COLUMNS.index(name) for name in plain]
    geo_cols = [INPUT_COLUMNS.index(name) for name in GEOMETRY_FEATURE_COLUMNS]
    order_lookup = {value: index for index, value in enumerate(ORDER_CHOICES)}
    mesher_lookup = {name: index for index, name in enumerate(mesher_choices)}
    order_unknown = float(len(ORDER_CHOICES))
    mesher_unknown = float(len(mesher_choices))
    order_col = INPUT_COLUMNS.index("order_idx")
    mesher_col = INPUT_COLUMNS.index("mesher_idx")
    derived_cols = [INPUT_COLUMNS.index(name) for name in DERIVED_FEATURE_COLUMNS]
    for r, row in enumerate(rows):
        for name, c in zip(plain, plain_cols):
            x[r, c] = to_float(row.get(name))
        if geo_cols:
            # Joined on the CAD solid, so all three load cases of a geometry
            # share it. A missing geometry leaves NaN and is imputed like any
            # other absent column rather than silently becoming zero.
            descriptors = GEOMETRY_FEATURE_TABLE.get(
                geometry_of(str(row.get("part", "") or "")))
            if descriptors:
                for name, c in zip(GEOMETRY_FEATURE_COLUMNS, geo_cols):
                    x[r, c] = descriptors.get(name, math.nan)
        order_value = to_float(row.get("order"))
        if math.isfinite(order_value) and int(round(order_value)) in order_lookup:
            x[r, order_col] = float(order_lookup[int(round(order_value))])
        else:
            x[r, order_col] = order_unknown
        mesher_value = str(row.get("mesher", "") or "").strip()
        x[r, mesher_col] = float(mesher_lookup.get(mesher_value, int(mesher_unknown)))
        derived = derived_features(to_float(row.get("volume")),
                                   to_float(row.get("diag")),
                                   to_float(row.get("h_rel")))
        for name, c in zip(DERIVED_FEATURE_COLUMNS, derived_cols):
            x[r, c] = derived[name]
    return x


def centre_by_case(target: np.ndarray, mask: np.ndarray,
                   parts: list[str]) -> np.ndarray:
    """``target`` minus the per-case median over that case's masked rows.

    Masked-out rows are left untouched; they carry no target anyway. The median
    (not the mean) so one blown-up action cannot drag a whole case's offset.
    """
    out = target.astype(np.float64).copy()
    by_case: dict[str, list[int]] = {}
    for i, part in enumerate(parts):
        if mask[i]:
            by_case.setdefault(part, []).append(i)
    for indices in by_case.values():
        offset = float(np.median(out[indices]))
        for i in indices:
            out[i] -= offset
    return out.astype(np.float32)


def _raw_targets(rows: list[dict[str, str]]) -> tuple[dict[str, np.ndarray], dict[str, np.ndarray]]:
    """log10 targets plus their presence masks (before the failure veto)."""
    n = len(rows)
    targets: dict[str, np.ndarray] = {}
    masks: dict[str, np.ndarray] = {}
    portable_heads = {"solve_flops", "solve_bytes", "mesh_work"}
    for head, (column, floor) in TARGET_SOURCES.items():
        raw = np.asarray(
            [portable_cost_label(row, head) if head in portable_heads
             else to_float(row.get(column)) for row in rows],
            dtype=np.float64,
        )
        present = np.isfinite(raw)
        values = np.zeros(n, dtype=np.float64)
        if present.any():
            values[present] = np.log10(np.maximum(raw[present], floor))
        finite = present & np.isfinite(values)
        targets[head] = values.astype(np.float32)
        masks[head] = finite
    # Centring happens over the WHOLE table, before the split: a case lives
    # entirely on one side of a part-hash split, so its median is identical
    # either way, and doing it here keeps train and val definitions identical.
    parts = [row.get("part", "") for row in rows]
    for head, source in CENTRED_HEADS.items():
        masks[head] = masks[source].copy()
        targets[head] = centre_by_case(targets[source], masks[head], parts)
    return targets, masks


def _best_actions(rows: list[dict[str, str]], failure: np.ndarray,
                  rel_err: np.ndarray, rel_err_mask: np.ndarray,
                  mesher_choices: list[str]) -> tuple[np.ndarray, np.ndarray]:
    """Behaviour-cloning target: the best feasible action of each row's part.

    "Best" = lowest ``rel_err`` target among the part's non-failure rows, ties
    broken by lower ``solve_ms``. Dims whose source value is missing for the
    chosen row are masked out instead of being invented.
    """
    n = len(rows)
    dims = build_action_dims(mesher_choices)
    groups = action_group_slices(mesher_choices)
    target = np.zeros((n, len(dims)), dtype=np.float32)
    mask = np.zeros((n, len(dims)), dtype=bool)

    solve_ms = np.asarray([to_float(row.get("solve_ms")) for row in rows], dtype=np.float64)
    order_lookup = {value: index for index, value in enumerate(ORDER_CHOICES)}
    mesher_lookup = {name: index for index, name in enumerate(mesher_choices)}

    by_part: dict[str, list[int]] = {}
    for index, row in enumerate(rows):
        by_part.setdefault(str(row.get("part", "") or ""), []).append(index)

    for part, indices in by_part.items():
        feasible = [i for i in indices if failure[i] == 0.0 and rel_err_mask[i]]
        if not feasible:
            continue
        def rank(i: int) -> tuple[float, float]:
            cost = solve_ms[i]
            return (float(rel_err[i]), cost if math.isfinite(cost) else math.inf)
        best = min(feasible, key=rank)
        row = rows[best]

        vector = np.zeros(len(dims), dtype=np.float32)
        valid = np.zeros(len(dims), dtype=bool)
        for offset, name in enumerate(CONTINUOUS_ACTION_DIMS):
            value = to_float(row.get(name))
            if math.isfinite(value):
                lo, hi = CLAMP_BOX[name]
                vector[groups["continuous"].start + offset] = float(min(max(value, lo), hi))
                valid[groups["continuous"].start + offset] = True
        # `order >= 2` is the p-elevation actuator; there is no separate
        # p_elevate dimension to clone into.
        order_value = to_float(row.get("order"))
        if math.isfinite(order_value) and int(round(order_value)) in order_lookup:
            vector[groups["order"].start + order_lookup[int(round(order_value))]] = 1.0
            valid[groups["order"]] = True
        mesher_value = str(row.get("mesher", "") or "").strip()
        if mesher_value in mesher_lookup:
            vector[groups["mesher"].start + mesher_lookup[mesher_value]] = 1.0
            valid[groups["mesher"]] = True

        for index in indices:
            target[index] = vector
            mask[index] = valid
    return target, mask


def _reject_non_advisor_rows(rows: list[dict[str, str]], path: Path) -> list[dict[str, str]]:
    """Keep only ``advisor-row-v3`` rows; abort with the counts we did find.

    Legacy rows carry real outcome targets but no features, so they impute to
    the training median and become constant inputs. Training on them produces a
    model whose ``normalization.json`` is degenerate and whose geometry heads
    never saw a single supervised example, with no error anywhere in the
    pipeline. The LightGBM baseline already refuses such data; so does this.
    """
    counts = Counter(str(row.get("schema", "") or "").strip() or "<blank>" for row in rows)
    kept = [row for row in rows if str(row.get("schema", "") or "").strip() == ADVISOR_ROW_SCHEMA]
    if kept:
        return kept
    inventory = ", ".join(f"{name}={counts[name]}" for name in sorted(counts))
    raise SystemExit(
        f"advisor dataset has no '{ADVISOR_ROW_SCHEMA}' rows: {path}\n"
        f"rows by schema: {inventory} ({len(rows)} total)\n"
        f"Legacy rows have no CaseFeatures and no action vector, so every model "
        f"input would be an imputed constant. Run an advisor campaign and rebuild:\n"
        f"  python scripts/advisor/run_batch.py --batch 1\n"
        f"  python scripts/build_advisor_dataset.py"
    )


def _require_stage_a_supervision(masks: dict[str, np.ndarray], is_train: np.ndarray,
                                 failure: np.ndarray, path: Path) -> None:
    """Abort when a Stage-A head has no unmasked training row to learn from."""
    train_counts = {head: int(masks[head][is_train].sum()) for head in REGRESSION_HEADS}
    starved = [head for head in ACCURACY_HEADS if train_counts[head] == 0]
    if not starved:
        return
    inventory = ", ".join(f"{head}={train_counts[head]}" for head in REGRESSION_HEADS)
    n_train = int(is_train.sum())
    raise SystemExit(
        f"advisor dataset has no training supervision for Stage-A head(s) "
        f"{', '.join(starved)}: {path}\n"
        f"unmasked training rows per head: {inventory}\n"
        f"train rows={n_train}, val rows={int((~is_train).sum())}, "
        f"failure rows={int(failure.sum())} (failures are masked out of every "
        f"regression head by design)\n"
        f"Stage A optimizes rel_err/geo_chamfer/geo_p99 only; with zero rows it "
        f"would export an untrained head. Re-run the campaign so the rows carry "
        f"accuracy_rel_err and geo_fidelity_*, then rebuild:\n"
        f"  python scripts/build_advisor_dataset.py"
    )


def load_dataset(csv_path: Path | str | None = None,
                 split: str = DEFAULT_SPLIT_MODE,
                 fold: int = 0,
                 n_folds: int | None = None,
                 max_train_groups: int | None = None,
                 group_seed: int = 0) -> AdvisorData:
    """Read the advisor CSV and produce standardized train/val splits.

    ``split`` names the hold-out grouping (see :data:`SPLIT_MODES`) and ``fold``
    selects which group block is held out. ``n_folds`` defaults to the number
    of groups, i.e. true leave-one-group-out: with today's six families that is
    six folds of exactly one family each.
    """
    path = Path(csv_path) if csv_path is not None else DATASET_CSV
    rows = read_rows(path)
    if not rows:
        raise SystemExit(f"advisor dataset is empty: {path}")
    rows = _reject_non_advisor_rows(rows, path)

    mesher_choices = sorted({
        str(row.get("mesher", "") or "").strip()
        for row in rows
        if str(row.get("mesher", "") or "").strip()
    })
    action_dims = build_action_dims(mesher_choices)

    keys = [row_key(row) for row in rows]
    parts = [str(row.get("part", "") or "") for row in rows]
    cfg_ids = [str(row.get("cfg_id", "") or "") for row in rows]

    x_raw = _raw_matrix(rows, mesher_choices)
    targets, presence = _raw_targets(rows)
    failure = np.asarray([_failure_flag(row) for row in rows], dtype=np.float32)
    ok = failure == 0.0
    trusted = np.asarray([_trust_flag(row) for row in rows], dtype=bool)
    # Accuracy heads additionally require a trusted label; geometry and cost
    # heads do not, because they are measured off the mesh and the clock rather
    # than against the reference.
    accuracy_masked = {"rel_err", "rel_err_rel"}
    masks = {
        head: (present & ok & trusted) if head in accuracy_masked else (present & ok)
        for head, present in presence.items()
    }
    policy_target, policy_mask = _best_actions(
        rows, failure, targets["rel_err"], masks["rel_err"], mesher_choices
    )

    is_val = split_mask(parts, split, fold, n_folds)
    is_train = ~is_val
    if max_train_groups is not None:
        # Learning-curve support: keep only ``max_train_groups`` of the training
        # groups, chosen by a seeded shuffle so the subset is reproducible and
        # so averaging over seeds averages over which families were kept. Rows
        # of dropped groups leave the training set entirely -- they must not
        # reach the imputer or the standardiser either, or the curve would be
        # measuring a model that had partial sight of data it was denied.
        train_groups = sorted({group_of(part, split) for i, part in enumerate(parts)
                               if is_train[i]})
        rng = np.random.default_rng(group_seed)
        keep = set(rng.permutation(np.asarray(train_groups, dtype=object))
                   [:max(1, int(max_train_groups))].tolist())
        is_train = np.asarray(
            [is_train[i] and group_of(part, split) in keep for i, part in enumerate(parts)],
            dtype=bool)
    _require_stage_a_supervision(masks, is_train, failure, path)

    impute = _column_medians(x_raw[is_train], mesher_choices)
    x_filled = np.where(np.isfinite(x_raw), x_raw, impute[None, :])

    mean, std = _standardizer(x_filled[is_train])
    x_std = ((x_filled - mean[None, :]) / std[None, :]).astype(np.float32)

    normalization = {
        "input_columns": list(INPUT_COLUMNS),
        "mean": [float(value) for value in mean],
        "std": [float(value) for value in std],
        "impute": [float(value) for value in impute],
        "passthrough_columns": list(PASSTHROUGH_COLUMNS),
        "order_choices": list(ORDER_CHOICES),
        "mesher_choices": list(mesher_choices),
        "output_names": list(OUTPUT_NAMES),
        "regression_heads": {head: "log10" for head in REGRESSION_HEADS},
        "action_dims": list(action_dims),
    }

    def build(name: str, selector: np.ndarray) -> Split:
        index = np.flatnonzero(selector)
        return Split(
            name=name,
            keys=[keys[i] for i in index],
            parts=[parts[i] for i in index],
            cfg_ids=[cfg_ids[i] for i in index],
            x=x_std[selector],
            targets={head: values[selector] for head, values in targets.items()},
            masks={head: values[selector] for head, values in masks.items()},
            failure=failure[selector],
            policy_target=policy_target[selector],
            policy_mask=policy_mask[selector],
        )

    groups = split_groups(parts, split)
    n_folds_effective = len(groups) if n_folds is None else int(n_folds)
    return AdvisorData(
        train=build("train", is_train),
        val=build("val", is_val),
        normalization=normalization,
        clamps=clamp_table(mesher_choices, rows),
        input_columns=list(INPUT_COLUMNS),
        action_dims=action_dims,
        order_choices=list(ORDER_CHOICES),
        mesher_choices=mesher_choices,
        n_rows=len(rows),
        csv_path=path,
        split_mode=split,
        fold=int(fold),
        n_folds=n_folds_effective,
        val_groups=fold_groups(groups, fold, n_folds_effective),
    )


def fold_groups(groups: list[str], fold: int, n_folds: int) -> list[str]:
    """The groups held out by ``fold``, dealt round-robin over sorted groups.

    Round-robin rather than contiguous blocks: contiguous blocks over an
    alphabetically sorted list would put related families in one fold the
    moment the corpus grows names like ``box_hole`` / ``box_slot``.
    """
    if not groups:
        return []
    n_folds = max(1, min(int(n_folds), len(groups)))
    fold = int(fold) % n_folds
    return [group for i, group in enumerate(groups) if i % n_folds == fold]


def split_mask(parts: list[str], split: str = DEFAULT_SPLIT_MODE,
               fold: int = 0, n_folds: int | None = None) -> np.ndarray:
    """Boolean mask selecting the validation rows of ``fold``.

    Every row of a held-out group goes to validation, so no geometry -- and
    under the default ``family`` mode no *relative* of a geometry -- straddles
    the split.
    """
    groups = split_groups(parts, split)
    if len(groups) < 2:
        raise SystemExit(
            f"split mode {split!r} yields {len(groups)} group(s); at least 2 are "
            "needed to hold one out"
        )
    total = len(groups) if n_folds is None else int(n_folds)
    held = set(fold_groups(groups, fold, total))
    return np.asarray([group_of(part, split) in held for part in parts], dtype=bool)


def _column_medians(x_train: np.ndarray, mesher_choices: list[str]) -> np.ndarray:
    """Per-column training median; unknown-slot for the categorical indices."""
    n_cols = x_train.shape[1] if x_train.size else len(INPUT_COLUMNS)
    impute = np.zeros(n_cols, dtype=np.float64)
    for c in range(n_cols):
        column = x_train[:, c] if x_train.size else np.zeros(0)
        finite = column[np.isfinite(column)]
        impute[c] = float(np.median(finite)) if finite.size else 0.0
    impute[INPUT_COLUMNS.index("order_idx")] = float(len(ORDER_CHOICES))
    impute[INPUT_COLUMNS.index("mesher_idx")] = float(len(mesher_choices))
    return impute


def file_digest(path: Path) -> str:
    """SHA-256 of a file, streamed. Empty string when it is absent."""
    if not path.is_file():
        return ""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_revision() -> str:
    """Current commit, with ``-dirty`` when the tree has uncommitted changes.

    Empty when git is unavailable, so provenance degrades to "unknown" rather
    than to a wrong answer.
    """
    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True,
                              text=True, timeout=10, check=False)
        if head.returncode != 0:
            return ""
        revision = head.stdout.strip()
        status = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                                capture_output=True, text=True, timeout=20, check=False)
        if status.returncode == 0 and status.stdout.strip():
            revision += "-dirty"
        return revision
    except (OSError, subprocess.SubprocessError):
        return ""


def provenance(data: "AdvisorData | None" = None, seed: int | None = None,
               **extra: Any) -> dict[str, Any]:
    """What produced an artifact, recorded inside the artifact.

    A checkpoint that cannot be tied to the CSV that trained it is not
    reproducible evidence, and this matters acutely mid-rebuild: the reference
    truths were replaced under a running analysis, so "which dataset was this?"
    stopped being answerable from the filename. The content hash answers it.

    ``-dirty`` on the revision is deliberate and not a warning to be silenced:
    an artifact produced from an uncommitted tree cannot be regenerated by
    anyone else, and that fact belongs in the artifact.
    """
    record: dict[str, Any] = {
        "git_revision": git_revision(),
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "python": sys.version.split()[0],
        "numpy": np.__version__,
    }
    try:
        import torch  # noqa: PLC0415 - optional at dataset-import time
        record["torch"] = torch.__version__
    except ImportError:
        pass
    if seed is not None:
        record["seed"] = int(seed)
    if data is not None:
        record["dataset_csv"] = str(data.csv_path)
        record["dataset_sha256"] = file_digest(Path(data.csv_path))
        record["dataset_rows"] = data.n_rows
        record["split_mode"] = data.split_mode
        record["fold"] = data.fold
        record["n_folds"] = data.n_folds
        record["held_out_groups"] = list(data.val_groups)
        record["n_inputs"] = data.n_inputs
        record["n_actions"] = data.n_actions
        record["geometry_features"] = list(GEOMETRY_FEATURE_COLUMNS)
        record["geometry_features_sha256"] = file_digest(GEOMETRY_FEATURES_CSV)
    record.update(extra)
    return record


def _standardizer(x_train: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Train mean/std; passthrough columns forced to (0, 1)."""
    n_cols = x_train.shape[1] if x_train.size else len(INPUT_COLUMNS)
    if x_train.size:
        mean = x_train.mean(axis=0)
        std = x_train.std(axis=0)
    else:
        mean = np.zeros(n_cols, dtype=np.float64)
        std = np.ones(n_cols, dtype=np.float64)
    # A degenerate (constant) column must not be divided by ~0: the floor keeps
    # std strictly positive, and constant columns collapse to exactly 1.0 so a
    # 1-ulp mean error cannot blow up into a huge standardized value.
    std = np.where(std > STD_FLOOR, std, 1.0)
    for name in PASSTHROUGH_COLUMNS:
        column = INPUT_COLUMNS.index(name)
        mean[column] = 0.0
        std[column] = 1.0
    return mean.astype(np.float64), std.astype(np.float64)


# --------------------------------------------------------------------------- #
# normalization.json / clamps.json IO
# --------------------------------------------------------------------------- #

def standardize_row(raw: dict[str, Any], normalization: dict[str, Any]) -> np.ndarray:
    """Turn a raw, possibly incomplete feature dict into a model input vector.

    Mirrors exactly what the C++ inference module does: absent or non-finite
    columns fall back to the persisted ``impute`` median, then every column is
    standardized with ``(x - mean) / std``.
    """
    columns = normalization["input_columns"]
    impute = normalization["impute"]
    mean = normalization["mean"]
    std = normalization["std"]
    vector = np.empty(len(columns), dtype=np.float64)
    for index, name in enumerate(columns):
        value = to_float(raw.get(name)) if name in raw else math.nan
        if not math.isfinite(value):
            value = float(impute[index])
        vector[index] = (value - float(mean[index])) / float(std[index])
    return vector


def standardize_matrix(raw: np.ndarray, normalization: dict[str, Any]) -> np.ndarray:
    """Vectorized ``standardize_row`` for a whole ``(n, D)`` raw input matrix.

    ``NaN`` marks an absent column and is filled from ``impute`` exactly as the
    per-row path does. Kept next to ``standardize_row`` so the two can never
    drift apart.
    """
    mean = np.asarray(normalization["mean"], dtype=np.float64)
    std = np.asarray(normalization["std"], dtype=np.float64)
    impute = np.asarray(normalization["impute"], dtype=np.float64)
    matrix = np.asarray(raw, dtype=np.float64)
    if matrix.ndim != 2 or matrix.shape[1] != mean.size:
        raise ValueError(
            f"raw matrix has shape {matrix.shape}, expected (n, {mean.size})")
    filled = np.where(np.isfinite(matrix), matrix, impute[None, :])
    return ((filled - mean[None, :]) / std[None, :]).astype(np.float32)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        json.dump(payload, stream, indent=2, sort_keys=False)
        stream.write("\n")


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def add_split_args(parser: Any) -> None:
    """Attach the ``--split`` / ``--fold`` / ``--n-folds`` trio to a parser.

    Shared so every entry point that reads the dataset describes the hold-out
    the same way and cannot quietly disagree with the others.
    """
    parser.add_argument("--split", choices=list(SPLIT_MODES), default=DEFAULT_SPLIT_MODE,
                        help="hold-out grouping (default: family, the only leakage-safe one)")
    parser.add_argument("--fold", type=int, default=0,
                        help="which group block to hold out (default: 0)")
    parser.add_argument("--n-folds", type=int, default=None,
                        help="fold count (default: one fold per group, i.e. leave-one-out)")


def load_from_args(args: Any, csv_path: Path | str | None = None) -> AdvisorData:
    """``load_dataset`` driven by a parser built with :func:`add_split_args`."""
    return load_dataset(
        csv_path if csv_path is not None else getattr(args, "csv", None),
        split=args.split, fold=args.fold, n_folds=args.n_folds,
    )


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=None, help="dataset CSV (default bench/advisor/dataset.csv)")
    add_split_args(parser)
    args = parser.parse_args()

    data = load_from_args(args)
    print(f"rows            : {data.n_rows}")
    print(f"input_columns D : {data.n_inputs}")
    print(f"action_dims   A : {data.n_actions}")
    print(f"mesher_choices  : {data.mesher_choices}")
    print(f"split           : {data.split_mode} fold {data.fold}/{data.n_folds} "
          f"holding out {data.val_groups}")
    print(f"train rows      : {data.train.n_rows} parts={sorted(set(data.train.parts))}")
    print(f"val rows        : {data.val.n_rows} parts={sorted(set(data.val.parts))}")
    for head in REGRESSION_HEADS:
        print(f"  mask {head:<12}: train={int(data.train.masks[head].sum())} val={int(data.val.masks[head].sum())}")
    print(f"  failure rows  : train={int(data.train.failure.sum())} val={int(data.val.failure.sum())}")
    # Surfaced because nothing else in the pipeline prints it: these are solves
    # that ran but failed their health gate, so they count for the cost and
    # geometry heads and not for the accuracy heads.
    for split_name, split in (("train", data.train), ("val", data.val)):
        solved = int((split.failure == 0.0).sum())
        trusted = int(split.masks["rel_err"].sum())
        share = (1.0 - trusted / solved) if solved else float("nan")
        print(f"  {split_name} accuracy : {trusted}/{solved} solved rows carry a trusted "
              f"label ({share:.1%} untrusted, excluded from the accuracy heads only)")
    print(f"  policy dims ok: train={int(data.train.policy_mask.any(axis=0).sum())}/{data.n_actions}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
