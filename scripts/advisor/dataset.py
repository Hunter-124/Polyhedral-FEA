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
* The train/validation split is by a stable ``blake2b`` hash of the ``part``
  string, so no part straddles the split and the assignment is identical on
  every machine and every run.
* ``order_idx`` / ``mesher_idx`` are *passthrough* columns (mean 0, std 1) so
  the plain ``(x - mean) / std`` loop in the C++ inference module stays valid
  for all D columns; the embedding lookup happens inside the graph.
"""

from __future__ import annotations

import csv
import hashlib
import json
import math
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
ADVISOR_DIR = ROOT / "bench" / "advisor"
DATASET_CSV = ADVISOR_DIR / "dataset.csv"
NORMALIZATION_JSON = ADVISOR_DIR / "normalization.json"
CLAMPS_JSON = ADVISOR_DIR / "clamps.json"

#: Only rows emitted by the advisor-aware testlab path carry the full C2
#: feature/action vector. Legacy campaign rows leave all 26 features and most
#: of the action columns empty, so training on them silently learns from
#: imputed constants; ``load_dataset`` rejects them outright.
ADVISOR_ROW_SCHEMA = "advisor-row-v2"

# --- C2 input columns -------------------------------------------------------

FEATURE_COLUMNS: list[str] = [
    "bbox_dx", "bbox_dy", "bbox_dz", "diag", "volume", "surface_area",
    "sa_over_v23", "n_faces", "n_sharp_edges", "sharp_edge_len_total",
    "curved_frac", "kappa_max_h", "kappa_mean_h", "thin_min_over_diag",
    "thin_p10_over_diag", "min_feature_h", "n_fix_faces", "n_load_faces",
    "fix_area_frac", "load_area_frac", "load_dir_x", "load_dir_y", "load_dir_z",
    "fix_load_dist_over_diag", "load_axis_alignment", "poisson",
]
CASE_COLUMNS: list[str] = [
    "case_poisson", "case_n_fix_regions", "case_n_load_regions", "case_load_dir_x",
    "case_load_dir_y", "case_load_dir_z", "case_traction_magnitude",
]
CONTINUOUS_ACTION_COLUMNS: list[str] = [
    "h_rel", "eta_target", "adapt_passes", "p_elevate", "element_tendency",
    "skin_layers", "feature_refine", "bc_grading", "adapt_leb_waves",
]
CATEGORICAL_INDEX_COLUMNS: list[str] = ["order_idx", "mesher_idx"]

INPUT_COLUMNS: list[str] = (
    FEATURE_COLUMNS + CASE_COLUMNS + CONTINUOUS_ACTION_COLUMNS + CATEGORICAL_INDEX_COLUMNS
)
PASSTHROUGH_COLUMNS: list[str] = list(CATEGORICAL_INDEX_COLUMNS)

# --- heads ------------------------------------------------------------------

REGRESSION_HEADS: list[str] = ["rel_err", "geo_chamfer", "geo_p99", "dof", "mesh_ms", "solve_ms"]
ACCURACY_HEADS: list[str] = ["rel_err", "geo_chamfer", "geo_p99"]
COST_HEADS: list[str] = ["dof", "mesh_ms", "solve_ms"]
OUTPUT_NAMES: list[str] = REGRESSION_HEADS + ["failure_logit", "policy"]
HEAD_NAMES: list[str] = REGRESSION_HEADS + ["failure"]

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
}

#: statuses that are *not* a failure (C7 failure head definition)
OK_STATUSES: frozenset[str] = frozenset({"ok", "solve_suspect"})

# --- C4 clamp box -----------------------------------------------------------

ORDER_CHOICES: list[int] = [1, 2, 3, 4]
P_ELEVATE_CHOICES: list[int] = [0, 1]
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
VETO_THRESHOLD = 0.5
ACTION_DEFAULTS: dict[str, Any] = {
    "mesher": "hybrid_zoo",
    "h_rel": 0.1,
    "order": 1,
    "adapt_passes": 0,
    "eta_target": 0.0,
    "p_elevate": False,
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


def part_bucket(part: str) -> float:
    """Deterministic value in ``[0, 1)`` derived from the part name."""
    digest = hashlib.blake2b(part.encode("utf-8")).digest()
    return (int.from_bytes(digest, "big") % 1_000_000_007) / 1_000_000_007


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
                 hidden: int = 64, emb_dim: int = 4) -> dict[str, Any]:
    """Build the ``AdvisorNet`` construction descriptor."""
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
    """``action_dims`` exactly as specified by C4."""
    dims = ["h_rel", "adapt_passes", "eta_target", "p_elevate_logit"]
    dims += [f"order_logit_{value}" for value in ORDER_CHOICES]
    dims += [f"mesher_logit_{name}" for name in mesher_choices]
    return dims


def action_group_slices(mesher_choices: list[str]) -> dict[str, slice]:
    """Index ranges of each logical group inside the action vector."""
    n_order = len(ORDER_CHOICES)
    n_mesher = len(mesher_choices)
    return {
        "continuous": slice(0, 3),
        "p_elevate": slice(3, 4),
        "order": slice(4, 4 + n_order),
        "mesher": slice(4 + n_order, 4 + n_order + n_mesher),
    }


def clamp_table(mesher_choices: list[str]) -> dict[str, Any]:
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
        "p_elevate_choices": list(P_ELEVATE_CHOICES),
        "mesher_choices": list(mesher_choices),
        "action_dims": build_action_dims(mesher_choices),
        "veto_threshold": VETO_THRESHOLD,
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
    status = str(row.get("status", "") or "").strip()
    error = str(row.get("error", "") or "").strip()
    if error and error.lower() not in OK_STATUSES:
        return 1.0
    if status and status.lower() not in OK_STATUSES:
        return 1.0
    if to_bool(row.get("accuracy_trusted")) is False:
        return 1.0
    return 0.0


def _raw_matrix(rows: list[dict[str, str]], mesher_choices: list[str]) -> np.ndarray:
    """Raw (un-imputed, un-standardized) input matrix, NaN where missing."""
    n = len(rows)
    x = np.full((n, len(INPUT_COLUMNS)), np.nan, dtype=np.float64)
    plain = FEATURE_COLUMNS + CASE_COLUMNS + CONTINUOUS_ACTION_COLUMNS
    order_lookup = {value: index for index, value in enumerate(ORDER_CHOICES)}
    mesher_lookup = {name: index for index, name in enumerate(mesher_choices)}
    order_unknown = float(len(ORDER_CHOICES))
    mesher_unknown = float(len(mesher_choices))
    order_col = INPUT_COLUMNS.index("order_idx")
    mesher_col = INPUT_COLUMNS.index("mesher_idx")
    for r, row in enumerate(rows):
        for c, name in enumerate(plain):
            x[r, c] = to_float(row.get(name))
        order_value = to_float(row.get("order"))
        if math.isfinite(order_value) and int(round(order_value)) in order_lookup:
            x[r, order_col] = float(order_lookup[int(round(order_value))])
        else:
            x[r, order_col] = order_unknown
        mesher_value = str(row.get("mesher", "") or "").strip()
        x[r, mesher_col] = float(mesher_lookup.get(mesher_value, int(mesher_unknown)))
    return x


def _raw_targets(rows: list[dict[str, str]]) -> tuple[dict[str, np.ndarray], dict[str, np.ndarray]]:
    """log10 targets plus their presence masks (before the failure veto)."""
    n = len(rows)
    targets: dict[str, np.ndarray] = {}
    masks: dict[str, np.ndarray] = {}
    for head, (column, floor) in TARGET_SOURCES.items():
        raw = np.asarray([to_float(row.get(column)) for row in rows], dtype=np.float64)
        present = np.isfinite(raw)
        values = np.zeros(n, dtype=np.float64)
        if present.any():
            values[present] = np.log10(np.maximum(raw[present], floor))
        finite = present & np.isfinite(values)
        targets[head] = values.astype(np.float32)
        masks[head] = finite
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
        p_elevate = to_bool(row.get("p_elevate"))
        if p_elevate is None:
            numeric = to_float(row.get("p_elevate"))
            p_elevate = bool(round(numeric)) if math.isfinite(numeric) else None
        if p_elevate is not None:
            vector[groups["p_elevate"]] = 1.0 if p_elevate else 0.0
            valid[groups["p_elevate"]] = True
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
    """Keep only ``advisor-row-v2`` rows; abort with the counts we did find.

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


def load_dataset(csv_path: Path | str | None = None, val_fraction: float = 0.2) -> AdvisorData:
    """Read the advisor CSV and produce standardized train/val splits."""
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
    masks = {head: (present & ok) for head, present in presence.items()}
    policy_target, policy_mask = _best_actions(
        rows, failure, targets["rel_err"], presence["rel_err"], mesher_choices
    )

    is_val = _split_mask(parts, val_fraction)
    is_train = ~is_val
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

    return AdvisorData(
        train=build("train", is_train),
        val=build("val", is_val),
        normalization=normalization,
        clamps=clamp_table(mesher_choices),
        input_columns=list(INPUT_COLUMNS),
        action_dims=action_dims,
        order_choices=list(ORDER_CHOICES),
        mesher_choices=mesher_choices,
        n_rows=len(rows),
        csv_path=path,
    )


def _split_mask(parts: list[str], val_fraction: float) -> np.ndarray:
    """80/20 part-hash split, guaranteed to leave both sides non-empty."""
    unique = sorted(set(parts))
    buckets = {part: part_bucket(part) for part in unique}
    threshold = 1.0 - float(val_fraction)
    val_parts = {part for part in unique if buckets[part] >= threshold}
    if unique:
        # With very few parts the hash can land entirely on one side; move the
        # single most/least extreme part across so both splits stay usable.
        if not val_parts:
            val_parts = {max(unique, key=lambda part: buckets[part])}
        elif len(val_parts) == len(unique):
            val_parts = set(unique) - {min(unique, key=lambda part: buckets[part])}
    return np.asarray([part in val_parts for part in parts], dtype=bool)


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


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=None, help="dataset CSV (default bench/advisor/dataset.csv)")
    parser.add_argument("--val-fraction", type=float, default=0.2)
    args = parser.parse_args()

    data = load_dataset(args.csv, args.val_fraction)
    print(f"rows            : {data.n_rows}")
    print(f"input_columns D : {data.n_inputs}")
    print(f"action_dims   A : {data.n_actions}")
    print(f"mesher_choices  : {data.mesher_choices}")
    print(f"train rows      : {data.train.n_rows} parts={sorted(set(data.train.parts))}")
    print(f"val rows        : {data.val.n_rows} parts={sorted(set(data.val.parts))}")
    for head in REGRESSION_HEADS:
        print(f"  mask {head:<12}: train={int(data.train.masks[head].sum())} val={int(data.val.masks[head].sum())}")
    print(f"  failure rows  : train={int(data.train.failure.sum())} val={int(data.val.failure.sum())}")
    print(f"  policy dims ok: train={int(data.train.policy_mask.any(axis=0).sum())}/{data.n_actions}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
