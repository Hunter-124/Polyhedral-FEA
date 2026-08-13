#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Export the advisor results report to docs/advisor/figures/.

``figures.py`` covers the two training-time assets (per-epoch curves and the
activation map). This module covers everything else the report needs -- the
network it converged to, what the mesh search actually bought, and pictures of
the meshes themselves:

  network_layout.png    the final architecture, read out of the checkpoint:
                        grouped input columns, embedding tables, trunk width
                        and every head with its output width + parameter count
  mesh_progress.png     anytime curve -- best-so-far accuracy and geometric
                        fidelity against cumulative solver wall time, median
                        over cases with an inter-quartile band
  accuracy_vs_cost.png  accuracy_rel_err against n_dof and against solve_ms,
                        one point per successful row, coloured by mesher, with
                        the Pareto front drawn
  fidelity_vs_h.png     chamfer mean and surface-distance p99 against h_rel,
                        split by element order
  mesh_before_after.png real wireframe renders from the campaign warehouse:
                        the coarse baseline mesh beside the best-accuracy mesh
                        for three parts from three different families
  external_comparison.png
                        external mesh-source rel_err against active DOF,
                        split by case family and element order

Missing inputs skip the affected figure with a printed "no data yet" note --
the script still exits 0 so it is safe to run mid-campaign. Every number that
lands in a figure is also printed, so the values can be quoted directly.

Run from anywhere:

    python scripts/advisor/report.py
    python scripts/advisor/report.py --advisor-dir bench/advisor \
        --out-dir docs/advisor/figures
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
import textwrap
from pathlib import Path
from typing import Any, Sequence

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import figstyle as fs  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ADVISOR_DIR = ROOT / "bench" / "advisor"
CAMPAIGNS_DIR = ROOT / "bench" / "campaigns"
FIGURES_DIR = ROOT / "docs" / "advisor" / "figures"
EXTERNAL_RESULTS = ROOT / "bench" / "results" / "gmsh-peer.json"
CORPUS_REFERENCE_DIR = ROOT / "bench" / "reference" / "corpus"

#: committed PNGs stay small; the palette pass in figstyle.finish handles the
#: mesh renders, which are the only figures here that get near the ceiling.
MAX_PNG_BYTES = 400 * 1024

#: Geometric-fidelity residuals are distances in model units. On most surfaces
#: the boundary residual is legitimately at machine precision (~1e-15 of the
#: part diagonal), so a raw log axis spends sixteen decades on noise and
#: flattens every real trend. Values at or below this floor are pinned to the
#: floor line and labelled as machine precision — never dropped.
PRECISION_FLOOR = 1e-12

#: only these rows carry the full C2 feature/action vector (see dataset.py)
CORPUS_SCHEMA = "advisor-row-v3"
#: statuses that produced a usable solve
OK_STATUS = "ok"
#: parts are named ``<family>_s<shape>_c<case>``; the family is the stem
FAMILY_RE = re.compile(r"_s\d+_c\d+$")
#: mesh_progress starts its window once this share of cases has a first
#: result, so the anytime median is taken over a fixed set of cases
START_COVERAGE_PCT = 85.0

#: h_rel rungs get their own marker so a resolution level is readable without
#: colour; the mapping is by sorted rung, not by hardcoded value, so a new
#: refinement level slots in without touching this table.
def h_rel_marker(h_rel: float, rungs: list[float]) -> str:
    try:
        index = rungs.index(round(h_rel, 4))
    except ValueError:
        index = len(rungs)
    return fs.MARKERS[index % len(fs.MARKERS)]


#: Four mesh sources now, and the fourth is the point: ``uniform-p2`` promotes
#: every element, so it is the only native variant that is order-for-order
#: comparable with Gmsh's uniformly quadratic tet10 meshes.
EXTERNAL_SOURCES = ["gmsh-mesh+polymesh-solver", "polymesh-native",
                    "polymesh-native-graded", "polymesh-native-uniform-p2"]
#: slot 2 (bluish green) is free in the mesher palette; pinning it here keeps
#: the new variant the same colour+marker+dash wherever it is drawn next.
fs.register_series("polymesh-native-uniform-p2", 2, label="native uniform p2")
EXTERNAL_LABELS = {name: fs.series(name).label for name in EXTERNAL_SOURCES}

#: input-column groups drawn in network_layout.png, in trunk-input order. The
#: fills are lightened series colours: the grouping is categorical, so it
#: reuses the one categorical palette rather than inventing pastels.
GROUP_SERIES = {
    "part features": "hybrid_zoo",
    "case context": "graded_tet",
    "continuous action": "hex",
    "categorical action": "hybrid_vem",
}


def tint(color: str, amount: float = 0.78) -> str:
    """Lighten a palette colour towards the page, for large filled areas."""
    from matplotlib.colors import to_hex, to_rgb

    rgb = np.array(to_rgb(color))
    page = np.array(to_rgb(fs.theme().panel))
    return to_hex(rgb + (page - rgb) * amount)


try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import (FancyArrowPatch, FancyBboxPatch, Patch,
                                    Rectangle)
    from matplotlib.lines import Line2D
except ImportError:
    raise SystemExit(
        "matplotlib is required for report.py — install it with:\n"
        "  pip install matplotlib"
    )

try:
    from PIL import Image
except ImportError:
    Image = None  # type: ignore[assignment]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--advisor-dir", type=Path, default=ADVISOR_DIR,
                        help="directory holding dataset.csv and runs/ "
                             "(default: bench/advisor)")
    parser.add_argument("--campaigns-dir", type=Path, default=CAMPAIGNS_DIR,
                        help="warehouse root holding <campaign>/runs/<cfg_id>/"
                             "<part>/t0/wire.png (default: bench/campaigns)")
    parser.add_argument("--out-dir", type=Path, default=FIGURES_DIR,
                        help="figure output directory (default: "
                             "docs/advisor/figures)")
    parser.add_argument("--external-results", type=Path, default=EXTERNAL_RESULTS,
                        help="Gmsh peer rows (default: bench/results/gmsh-peer.json)")
    parser.add_argument("--external-only", action="store_true",
                        help="render only external_comparison.png")
    return parser.parse_args()


# --- small helpers ----------------------------------------------------------


def load_json(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def to_float(value: Any) -> float:
    """CSV cell -> float, with blanks and junk mapped to NaN."""
    try:
        result = float(value)
    except (TypeError, ValueError):
        return math.nan
    return result if math.isfinite(result) else math.nan


def family_of(part: str) -> str:
    return FAMILY_RE.sub("", part)


def read_rows(dataset_csv: Path) -> list[dict[str, str]]:
    if not dataset_csv.is_file():
        return []
    with dataset_csv.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def corpus_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [row for row in rows if row.get("schema") == CORPUS_SCHEMA]


def save(fig: Any, out_dir: Path, name: str) -> Path:
    return fs.finish(fig, out_dir / name, max_bytes=MAX_PNG_BYTES)


def fmt(value: float, digits: int = 4) -> str:
    if not math.isfinite(value):
        return "n/a"
    if value != 0 and (abs(value) < 1e-3 or abs(value) >= 1e5):
        return f"{value:.{digits}g}"
    return f"{value:.{digits}g}"


# --- figure 1: network_layout.png -------------------------------------------


def checkpoint_shape(advisor_dir: Path) -> dict[str, Any] | None:
    """Architecture read out of ``runs/latest.pt`` + ``normalization.json``.

    Nothing here is hardcoded: widths, head names and the parameter count all
    come from the saved tensors, so the diagram cannot drift from the model.
    """
    checkpoint_path = advisor_dir / "runs" / "latest.pt"
    normalization = load_json(advisor_dir / "normalization.json") or {}
    if not checkpoint_path.is_file():
        return None
    try:
        import torch
    except ImportError:
        print("  torch is not importable — cannot read runs/latest.pt")
        return None

    blob = torch.load(checkpoint_path, map_location="cpu", weights_only=False)
    config = dict(blob.get("config") or {})
    state = blob.get("model") or blob.get("state_dict") or {}
    if not config or not state:
        return None

    shapes = {key: tuple(tensor.shape) for key, tensor in state.items()}
    n_parameters = int(sum(int(np.prod(shape)) for shape in shapes.values()))

    input_columns = list(config.get("input_columns")
                         or normalization.get("input_columns") or [])
    action_dims = list(config.get("action_dims")
                       or normalization.get("action_dims") or [])
    hidden = int(shapes.get("fc1.weight", (0, 0))[0])
    trunk_inputs = int(shapes.get("fc1.weight", (0, 0))[1])
    emb_dim = int(shapes.get("order_embedding.weight", (0, 0))[1])

    heads: list[tuple[str, int, int]] = []  # (name, width, parameters)
    for name in config.get("output_names") or normalization.get("output_names") or []:
        if name == "policy":
            key = "policy_head.weight"
        elif name == "failure_logit":
            key = "failure_head.weight"
        else:
            key = f"regression_heads.{name}.weight"
        if key not in shapes:
            continue
        width = int(shapes[key][0])
        heads.append((name, width, width * hidden + width))

    return {
        "run": blob.get("run"),
        "input_columns": input_columns,
        "action_dims": action_dims,
        "hidden": hidden,
        "trunk_inputs": trunk_inputs,
        "emb_dim": emb_dim,
        "order_slots": int(shapes.get("order_embedding.weight", (0, 0))[0]),
        "mesher_slots": int(shapes.get("mesher_embedding.weight", (0, 0))[0]),
        "n_parameters": n_parameters,
        "heads": heads,
        "fc1_parameters": trunk_inputs * hidden + hidden,
        "fc2_parameters": hidden * hidden + hidden,
    }


def input_groups(input_columns: list[str]) -> list[tuple[str, list[str]]]:
    """Split the C2 input vector into its four semantic groups.

    The membership lists come from ``dataset.py`` so the labels track the
    schema. If that import fails the diagram falls back to one flat group.
    """
    try:
        if __package__ in (None, ""):
            sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
        from advisor.dataset import (  # noqa: PLC0415
            CASE_COLUMNS,
            CATEGORICAL_INDEX_COLUMNS,
            CONTINUOUS_ACTION_COLUMNS,
            FEATURE_COLUMNS,
        )
    except ImportError:
        return [("input columns", list(input_columns))]

    known = {
        "part features": FEATURE_COLUMNS,
        "case context": CASE_COLUMNS,
        "continuous action": CONTINUOUS_ACTION_COLUMNS,
        "categorical action": CATEGORICAL_INDEX_COLUMNS,
    }
    groups = [(label, [c for c in input_columns if c in set(members)])
              for label, members in known.items()]
    claimed = {c for _, members in groups for c in members}
    leftover = [c for c in input_columns if c not in claimed]
    if leftover:
        groups.append(("other", leftover))
    return [(label, members) for label, members in groups if members]


def contract_heads() -> list[str] | None:
    """``OUTPUT_NAMES`` from dataset.py, for the drift check."""
    try:
        if __package__ in (None, ""):
            sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
        from advisor.dataset import OUTPUT_NAMES  # noqa: PLC0415
    except ImportError:
        return None
    return list(OUTPUT_NAMES)


def _box(ax: Any, x: float, y: float, w: float, h: float, text: str,
         face: str, edge: str | None = None, fontsize: float = 8.5,
         weight: str = "normal") -> tuple[float, float]:
    fs.assert_glyphs(text)
    ax.add_patch(FancyBboxPatch(
        (x, y), w, h, boxstyle="round,pad=0.004,rounding_size=0.008",
        facecolor=face, edgecolor=edge or fs.theme().rule, linewidth=1.0,
        zorder=2))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
            color=fs.theme().ink, fontsize=fontsize, zorder=3, weight=weight,
            linespacing=1.35)
    return x + w, y + h / 2


def _arrow(ax: Any, start: tuple[float, float], end: tuple[float, float],
           color: str | None = None, width: float = 0.9) -> None:
    ax.add_patch(FancyArrowPatch(
        start, end, arrowstyle="-|>", mutation_scale=9, linewidth=width,
        color=color or fs.theme().muted, zorder=1, shrinkA=1.5, shrinkB=1.5))


def network_layout(advisor_dir: Path, out_dir: Path) -> bool:
    shape = checkpoint_shape(advisor_dir)
    if shape is None:
        print("no data yet — expected "
              f"{advisor_dir}/runs/latest.pt; skipping network_layout.png")
        return False

    hidden = shape["hidden"]
    emb_dim = shape["emb_dim"]
    groups = input_groups(shape["input_columns"])
    heads = shape["heads"]
    n_continuous = len(shape["input_columns"]) - 2

    print("\nnetwork_layout.png — architecture read from runs/latest.pt")
    print(f"  training run          : {shape['run']}")
    print(f"  input columns         : {len(shape['input_columns'])}"
          + "".join(f"\n    {label:<20}: {len(members)}"
                    for label, members in groups))
    print(f"  embeddings            : order {shape['order_slots']}x{emb_dim}, "
          f"mesher {shape['mesher_slots']}x{emb_dim}")
    print(f"  trunk input width     : {shape['trunk_inputs']} "
          f"(= {n_continuous} continuous + 2 x {emb_dim} embedding)")
    print(f"  trunk                 : Linear({shape['trunk_inputs']}->{hidden}) "
          f"-> GELU -> Linear({hidden}->{hidden}) -> GELU  "
          f"({shape['fc1_parameters'] + shape['fc2_parameters']} parameters)")
    for name, width, params in heads:
        print(f"  head {name:<16}: width {width:<3d} ({params} parameters)")
    print(f"  total parameters      : {shape['n_parameters']}")

    contract = contract_heads()
    drift = contract is not None and contract != [name for name, _, _ in heads]
    if drift:
        print(f"  NOTE checkpoint heads {[n for n, _, _ in heads]} differ from "
              f"the dataset.py contract {contract}")

    fig, axes = fs.figure(
        "AdvisorNet — architecture as saved in the checkpoint",
        subtitle=(f"training run {shape['run']} · {shape['n_parameters']:,} "
                  f"parameters · trunk width {hidden} · {len(heads)} heads"),
        footer=fs.footer_source(advisor_dir / "runs" / "latest.pt",
                                advisor_dir / "normalization.json"),
        size=(13.2, 7.2))
    ax = axes[0][0]
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")

    top, bottom = 0.90, 0.10
    span = top - bottom

    # column 0 -- input groups, height proportional to column count but with a
    # floor so a 2-column group still fits its own caption.
    total_columns = sum(len(members) for _, members in groups)
    gap = 0.018
    usable = span - gap * (len(groups) - 1)
    minimum = min(0.085, usable / max(len(groups), 1))
    raw = [usable * len(members) / total_columns for _, members in groups]
    heights = [max(value, minimum) for value in raw]
    excess = sum(heights) - usable
    slack = sum(h - minimum for h in heights)
    if excess > 0 and slack > 0:  # shrink the oversized groups back to fit
        heights = [h - (h - minimum) * excess / slack for h in heights]
    colors = {label: tint(fs.series(name).color)
              for label, name in GROUP_SERIES.items()}
    y = top
    group_ports: list[tuple[str, tuple[float, float], list[str]]] = []
    for (label, members), h in zip(groups, heights):
        y -= h
        sample = ", ".join(members[:2])
        if len(members) > 2:
            sample += ", ..."
        right, mid = _box(
            ax, 0.02, y, 0.185, h,
            f"{label}\n{len(members)} columns\n{sample}",
            colors.get(label, tint(fs.series("hybrid_zoo").color)), fontsize=8.2)
        group_ports.append((label, (right, mid), members))
        y -= gap

    ax.text(0.1125, top + 0.045, f"input features  x[{len(shape['input_columns'])}]",
            ha="center", va="center", fontsize=fs.FONT_PT["panel"],
            weight="bold")
    ax.text(0.1125, top + 0.016,
            "standardized in C++ by normalization.json (mean/std/impute)",
            ha="center", va="center", fontsize=fs.FONT_PT["footer"],
            color=fs.theme().muted, style="italic")

    # column 1 -- embedding tables for the two categorical columns
    emb_ports: list[tuple[float, float]] = []
    emb_y = bottom + 0.005
    for name, slots in (("mesher_idx", shape["mesher_slots"]),
                        ("order_idx", shape["order_slots"])):
        right, mid = _box(
            ax, 0.265, emb_y, 0.135, 0.075,
            f"Embedding({slots}, {emb_dim})\n{name}",
            tint(fs.series("hybrid_vem").color), fontsize=8.2)
        emb_ports.append((right, mid))
        emb_y += 0.095
    ax.text(0.3325, emb_y + 0.012, "round + clamp, then gather\n(inside the ONNX graph)",
            ha="center", va="bottom", fontsize=fs.FONT_PT["footer"],
            color=fs.theme().muted, style="italic")

    # column 2 -- concatenation into the trunk input
    concat_x, concat_w = 0.455, 0.10
    concat_y, concat_h = bottom + 0.06, span - 0.12
    _box(ax, concat_x, concat_y, concat_w, concat_h,
         f"concat\n\nD_eff = {shape['trunk_inputs']}\n\n"
         f"{n_continuous} continuous\n+ {emb_dim} order\n+ {emb_dim} mesher",
         tint(fs.theme().muted, 0.90), fontsize=8.6)
    concat_left = (concat_x, concat_y + concat_h / 2)
    concat_right = (concat_x + concat_w, concat_y + concat_h / 2)

    for label, port, _ in group_ports:
        if label == "categorical action":
            for emb_right, emb_mid in emb_ports:
                _arrow(ax, port, (0.265, emb_mid),
                       color=fs.series("hybrid_vem").color)
                _arrow(ax, (emb_right, emb_mid), (concat_x, emb_mid),
                       color=fs.series("hybrid_vem").color)
        else:
            _arrow(ax, port, concat_left)

    # column 3 -- trunk
    trunk_x, trunk_w = 0.605, 0.115
    trunk_h = 0.155
    fc1_y = 0.545
    fc2_y = 0.305
    _box(ax, trunk_x, fc1_y, trunk_w, trunk_h,
         f"Linear({shape['trunk_inputs']} -> {hidden})\nGELU\n\n"
         f"{shape['fc1_parameters']:,} params",
         tint(fs.series("hybrid_zoo").color, 0.62), fontsize=9,
         weight="bold")
    _box(ax, trunk_x, fc2_y, trunk_w, trunk_h,
         f"Linear({hidden} -> {hidden})\nGELU\n\n"
         f"{shape['fc2_parameters']:,} params",
         tint(fs.series("hybrid_zoo").color, 0.62), fontsize=9,
         weight="bold")
    _arrow(ax, concat_right, (trunk_x, fc1_y + trunk_h / 2), width=1.4)
    _arrow(ax, (trunk_x + trunk_w / 2, fc1_y),
           (trunk_x + trunk_w / 2, fc2_y + trunk_h), width=1.4)
    ax.text(trunk_x + trunk_w / 2, top + 0.02,
            f"shared trunk\nwidth {hidden}", ha="center", va="center",
            fontsize=fs.FONT_PT["label"], weight="bold")

    # column 4 -- heads
    head_x, head_w = 0.795, 0.185
    head_gap = 0.012
    head_h = (span - head_gap * (len(heads) - 1)) / max(len(heads), 1)
    trunk_out = (trunk_x + trunk_w, fc2_y + trunk_h / 2)
    y = top
    for name, width, params in heads:
        y -= head_h
        face = tint(fs.series("graded_tet").color, 0.62) if name == "policy" else (
            tint(fs.theme().bad, 0.80) if name == "failure_logit"
            else tint(fs.series("graded_tet").color, 0.86))
        _box(ax, head_x, y, head_w, head_h,
             f"{name}   [{width}]   {params} params",
             face, fontsize=8.6)
        _arrow(ax, trunk_out, (head_x, y + head_h / 2), width=0.8)
        y -= head_gap
    ax.text(head_x + head_w / 2, top + 0.045,
            f"output heads  ({len(heads)})", ha="center", va="center",
            fontsize=fs.FONT_PT["panel"], weight="bold")
    ax.text(head_x + head_w / 2, top + 0.016,
            "ONNX output order, top to bottom",
            ha="center", va="center", fontsize=fs.FONT_PT["footer"],
            color=fs.theme().muted, style="italic")

    if drift:
        fig.text(0.5, 0.028,
                 "checkpoint predates the current dataset.py head contract "
                 f"({', '.join(contract or [])})",
                 ha="center", fontsize=fs.FONT_PT["annot"],
                 color=fs.theme().bad, style="italic")
    save(fig, out_dir, "network_layout.png")
    return True


# --- figure 2: mesh_progress.png --------------------------------------------


def _best_so_far(values: np.ndarray) -> np.ndarray:
    """Running minimum that ignores NaN and stays NaN until the first value."""
    out = np.full(values.shape, np.nan)
    best = math.inf
    for index, value in enumerate(values):
        if math.isfinite(value) and value < best:
            best = value
        if math.isfinite(best):
            out[index] = best
    return out


def _step_sample(times: np.ndarray, values: np.ndarray,
                 grid: np.ndarray) -> np.ndarray:
    """Right-continuous step lookup: value in force at each grid time."""
    index = np.searchsorted(times, grid, side="right") - 1
    out = np.full(grid.shape, np.nan)
    valid = index >= 0
    out[valid] = values[index[valid]]
    return out


def mesh_progress(rows: list[dict[str, str]], out_dir: Path) -> bool:
    corpus = corpus_rows(rows)
    if not corpus:
        print("no data yet — no advisor-row-v3 rows in dataset.csv; "
              "skipping mesh_progress.png")
        return False

    by_case: dict[str, list[dict[str, str]]] = {}
    for row in corpus:
        by_case.setdefault(row["part"], []).append(row)

    metrics = [
        ("accuracy_rel_err", "best-so-far accuracy  (rel_err)",
         "best-so-far rel_err", "hybrid_zoo"),
        ("geo_fidelity_dist_p99",
         "best-so-far geometric fidelity  (surface distance p99)",
         "best-so-far dist p99  (model units)", "graded_tet"),
    ]
    curves: dict[str, list[tuple[np.ndarray, np.ndarray]]] = {
        k: [] for k, _, _, _ in metrics}
    horizons: list[float] = []
    for case, case_rows in sorted(by_case.items()):
        cost = np.array([to_float(r["mesh_ms"]) + to_float(r["solve_ms"])
                         for r in case_rows])
        cost = np.nan_to_num(cost, nan=0.0)
        times = np.cumsum(cost) / 1000.0  # seconds of solver wall time
        if times[-1] <= 0:
            continue
        horizons.append(float(times[-1]))
        for key, _, _, _ in metrics:
            raw = np.array([to_float(r[key]) if r["status"] == OK_STATUS else math.nan
                            for r in case_rows])
            best = _best_so_far(raw)
            if np.isfinite(best).any():
                curves[key].append((times, best))

    if not horizons or not any(curves.values()):
        print("no data yet — corpus rows carry no timing/metric values; "
              "skipping mesh_progress.png")
        return False

    horizon = float(np.median(horizons))
    print("\nmesh_progress.png — anytime curve over "
          f"{len(by_case)} cases, {len(corpus)} corpus rows")
    print(f"  cumulative solver time per case: median {horizon:.0f} s, "
          f"min {min(horizons):.0f} s, max {max(horizons):.0f} s")
    print("  a case's curve is held flat once it runs out of actions, so the "
          "population is fixed across the window")

    # Reduce first, draw second: the subtitle states the measured gain, so it
    # has to be computed from the data before the figure exists.
    reduced: dict[str, dict[str, Any]] = {}
    for key, label, _short, name in metrics:
        case_curves = curves[key]
        if not case_curves:
            continue
        # The window starts once most cases have produced a first result. A
        # case that starts later is dropped rather than allowed to join
        # mid-curve: a changing population would make the median rise even
        # though every individual curve only ever falls.
        firsts = np.array([float(t[np.isfinite(v)][0]) for t, v in case_curves])
        t_lo = float(np.percentile(firsts, START_COVERAGE_PCT))
        kept = [curve for curve, first in zip(case_curves, firsts) if first <= t_lo]
        dropped = len(case_curves) - len(kept)
        t_hi = max(float(t[-1]) for t, _ in kept)
        grid = np.geomspace(t_lo, t_hi, 240)
        stack = np.vstack([_step_sample(t, v, grid) for t, v in kept])
        median = np.median(stack, axis=0)
        reduced[key] = {
            "label": label, "series": name, "grid": grid, "median": median,
            "lo": np.percentile(stack, 25, axis=0),
            "hi": np.percentile(stack, 75, axis=0),
            "kept": len(kept), "dropped": dropped, "t_lo": t_lo, "t_hi": t_hi,
            "gain": (median[0] / median[-1]) if median[-1] > 0 else math.nan,
        }

    gains = " · ".join(
        f"{key.split('_')[0]} median improves {reduced[key]['gain']:.2f}×"
        for key, _, _, _ in metrics if key in reduced
        and math.isfinite(reduced[key]["gain"]))
    fig, axes = fs.figure(
        "Mesh search over the campaign — best result found so far against "
        "solver time spent",
        subtitle=(f"{gains} over the search window; band is the inter-quartile "
                  f"range across cases"),
        footer=fs.footer_source(ADVISOR_DIR / "dataset.csv", n=len(corpus),
                                note=f"{len(by_case)} cases"),
        size="wide", ncols=2,
        share_y_axis="the two panels measure different quantities "
                     "(relative error vs distance in model units)")

    for ax, (key, label, short, name) in zip(axes[0], metrics):
        stats = reduced.get(key)
        st = fs.series(name)
        if stats is None:
            ax.text(0.5, 0.5, f"no {key} data", ha="center", va="center",
                    transform=ax.transAxes, color=fs.theme().muted)
            fs.axes_off(ax)
            continue
        grid, median = stats["grid"], stats["median"]
        ax.fill_between(grid, stats["lo"], stats["hi"], color=st.color,
                        alpha=0.16, linewidth=0, label="inter-quartile band")
        ax.plot(grid, median, color=st.color, linestyle=st.dash, linewidth=2.2,
                label=f"median over {stats['kept']} cases")
        ax.axvline(horizon, color=fs.theme().rule, linewidth=1.1,
                   linestyle=(0, (1, 2)),
                   label=f"median case budget ({horizon:.0f} s)")
        ax.set_xscale("log")
        info = fs.loglim(ax, np.concatenate([stats["lo"], stats["hi"]]))
        ax.set_xlabel("cumulative solver wall time per case  [s]")
        ax.set_ylabel(short)
        fs.panel_title(ax, label)
        ax.legend(loc="lower left")
        fs.annotate_n(ax, stats["kept"], excluded=stats["dropped"],
                      what="cases", extra=info.note())
        print(f"  {key}: median {fmt(float(median[0]))} at {stats['t_lo']:.1f} s -> "
              f"{fmt(float(median[-1]))} at {stats['t_hi']:.0f} s  "
              f"({stats['gain']:.2f}x better); "
              f"start IQR [{fmt(float(stats['lo'][0]))}, {fmt(float(stats['hi'][0]))}] -> "
              f"final IQR [{fmt(float(stats['lo'][-1]))}, {fmt(float(stats['hi'][-1]))}]; "
              f"{stats['kept']} cases in the window, {stats['dropped']} started too late")

    save(fig, out_dir, "mesh_progress.png")
    return True


# --- figure 3: accuracy_vs_cost.png -----------------------------------------


def _pareto(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    """Indices of the lower-left Pareto front: minimize both axes."""
    keep: list[int] = []
    best = math.inf
    for index in np.lexsort((y, x)):
        if y[index] < best:
            best = float(y[index])
            keep.append(int(index))
    return np.array(keep, dtype=int)


def accuracy_vs_cost(rows: list[dict[str, str]], out_dir: Path) -> bool:
    every = corpus_rows(rows)
    corpus = [r for r in every if r["status"] == OK_STATUS]
    usable = [r for r in corpus if math.isfinite(to_float(r["accuracy_rel_err"]))]
    if not usable:
        print("no data yet — no successful corpus rows with accuracy_rel_err; "
              "skipping accuracy_vs_cost.png")
        return False
    failed = len(every) - len(corpus)
    no_metric = len(corpus) - len(usable)

    accuracy = np.array([to_float(r["accuracy_rel_err"]) for r in usable])
    meshers = [r["mesher"] for r in usable]
    panels = [("n_dof", "degrees of freedom  n_dof"),
              ("solve_ms", "solve time  [ms]")]

    print(f"\naccuracy_vs_cost.png — {len(usable)} successful corpus rows "
          f"({failed} failed meshes and {no_metric} rows without a metric "
          f"excluded from {len(every)})")
    fig, axes = fs.figure(
        "Accuracy against cost for every corpus mesh",
        subtitle="point cloud is the whole campaign; the heavy line is each "
                 "mesher's binned median, so density is readable instead of "
                 "overplotted",
        footer=fs.footer_source(ADVISOR_DIR / "dataset.csv", n=len(every)),
        size="wide", ncols=2)

    for ax, (key, xlabel) in zip(axes[0], panels):
        cost = np.array([to_float(r[key]) for r in usable])
        keep = np.isfinite(cost) & (cost > 0) & (accuracy > 0)
        for mesher in sorted(set(meshers)):
            sel = keep & np.array([m == mesher for m in meshers])
            if not sel.any():
                continue
            st = fs.series(mesher)
            # A thin, small marker cloud plus a binned median: 2,000 points at
            # alpha on top of each other hid which mesher owned which region.
            ax.scatter(cost[sel], accuracy[sel], s=7, alpha=0.22,
                       marker=st.marker, color=st.color, edgecolors="none",
                       zorder=2)
            edges = np.geomspace(cost[sel].min(), cost[sel].max(), 9)
            centres, medians = [], []
            for lo_edge, hi_edge in zip(edges[:-1], edges[1:]):
                bucket = accuracy[sel & (cost >= lo_edge) & (cost < hi_edge)]
                if bucket.size >= 3:
                    centres.append(math.sqrt(lo_edge * hi_edge))
                    medians.append(float(np.median(bucket)))
            if centres:
                ax.plot(centres, medians, color=st.color, linestyle=st.dash,
                        marker=st.marker, markersize=5.5, linewidth=2.2,
                        markeredgecolor=fs.theme().bg, markeredgewidth=0.6,
                        zorder=4,
                        label=f"{st.label}  median  (n={int(sel.sum()):,})")
            else:
                ax.plot([], [], color=st.color, linestyle=st.dash,
                        marker=st.marker, label=f"{st.label}  (n={int(sel.sum()):,})")

        front = _pareto(cost[keep], accuracy[keep])
        kept_rows = [row for row, flag in zip(usable, keep) if flag]
        px, py = cost[keep][front], accuracy[keep][front]
        owners = [kept_rows[index]["part"] for index in front]
        ax.plot(px, py, color=fs.theme().ink, linewidth=1.5,
                drawstyle="steps-post", zorder=5,
                label=f"Pareto front  ({len(px)} "
                      f"point{'' if len(px) == 1 else 's'})")
        ax.scatter(px, py, s=26, facecolors="none", edgecolors=fs.theme().ink,
                   linewidths=1.1, zorder=6)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel(xlabel)
        # The front is a staircase along the lower-left, exactly where an
        # unframed key would sit on top of it. Give the key an opaque card so
        # the front reads as passing behind it rather than through it.
        legend = ax.legend(loc="lower left", frameon=True, framealpha=0.94)
        legend.get_frame().set_facecolor(fs.theme().panel)
        legend.get_frame().set_edgecolor(fs.theme().rule)
        fs.annotate_n(ax, int(keep.sum()), excluded=failed + no_metric,
                      what="meshes")

        print(f"  vs {key}: {int(keep.sum())} points, "
              f"{key} {fmt(float(cost[keep].min()))} .. {fmt(float(cost[keep].max()))}, "
              f"rel_err {fmt(float(accuracy[keep].min()))} .. "
              f"{fmt(float(accuracy[keep].max()))}")
        print(f"    Pareto front ({len(px)} point{'' if len(px) == 1 else 's'}, "
              f"{len(set(owners))} distinct case"
              f"{'' if len(set(owners)) == 1 else 's'}): "
              + ", ".join(f"({fmt(a, 3)}, {fmt(b, 3)}) {part}"
                          for a, b, part in zip(px, py, owners)))
        # rel_err carries a per-case reference-truth offset, so a single lucky
        # case can dominate the pooled cloud outright. Say so on the figure
        # rather than letting a one-point front read as a plotting bug.
        if len(set(owners)) <= 2:
            note = (f"front owned by {', '.join(sorted(set(owners)))} — rel_err "
                    "is not\ncomparable across cases (hence the rel_err_rel head)")
            fs.assert_glyphs(note)
            ax.text(0.02, 0.02, note, transform=ax.transAxes, va="bottom",
                    ha="left", fontsize=fs.FONT_PT["annot"] - 0.5,
                    color=fs.theme().muted, style="italic")

    axes[0][0].set_ylabel("accuracy_rel_err  (lower is better)")
    save(fig, out_dir, "accuracy_vs_cost.png")
    return True


# --- figure 4: fidelity_vs_h.png --------------------------------------------


def fidelity_vs_h(rows: list[dict[str, str]], out_dir: Path) -> bool:
    usable = [r for r in corpus_rows(rows)
              if math.isfinite(to_float(r["h_rel"]))
              and (math.isfinite(to_float(r["geo_fidelity_chamfer_mean"]))
                   or math.isfinite(to_float(r["geo_fidelity_dist_p99"])))]
    if not usable:
        print("no data yet — no rows with h_rel and geometric fidelity; "
              "skipping fidelity_vs_h.png")
        return False

    metrics = [("geo_fidelity_chamfer_mean", "chamfer mean distance"),
               ("geo_fidelity_dist_p99", "surface distance p99")]
    orders = sorted({int(to_float(r["order"])) for r in usable
                     if math.isfinite(to_float(r["order"]))})

    print(f"\nfidelity_vs_h.png — {len(usable)} rows with geometric fidelity")
    print(f"  precision floor {PRECISION_FLOOR:g} model units: on most "
          "surfaces the boundary residual is at machine precision, and a raw "
          "log axis over those values shows nothing")
    fig, axes = fs.figure(
        "Geometric fidelity against mesh resolution, split by element order",
        subtitle="markers are per-resolution medians with the inter-quartile "
                 "range; values at the floor line are at machine precision, "
                 "not zero error",
        footer=fs.footer_source(ADVISOR_DIR / "dataset.csv", n=len(usable)),
        size="wide", ncols=2,
        share_y_axis="chamfer mean and p99 surface distance are different "
                     "statistics of the same residual")

    for ax, (key, label) in zip(axes[0], metrics):
        all_levels = sorted({round(to_float(r["h_rel"]), 4) for r in usable
                             if math.isfinite(to_float(r[key]))})
        column = np.array([to_float(r[key]) for r in usable
                           if math.isfinite(to_float(r[key]))])
        # Floor first, then draw: the axis limits come from the floored data so
        # a handful of machine-precision residuals cannot stretch the panel
        # over sixteen empty decades and flatten every real trend.
        info = fs.loglim(ax, column, floor=PRECISION_FLOOR)
        rates: list[str] = []
        for index, order in enumerate(orders):
            sel = [r for r in usable
                   if math.isfinite(to_float(r["order"]))
                   and int(to_float(r["order"])) == order
                   and math.isfinite(to_float(r[key]))]
            if not sel:
                continue
            st = fs.series(f"order {order}")
            # nudge each order sideways: fidelity is a surface property, so
            # the orders land on top of each other without an offset.
            nudge = math.exp((index - (len(orders) - 1) / 2) * 0.02)
            hs = np.array([to_float(r["h_rel"]) for r in sel])
            vs = fs.clamp_to_floor([to_float(r[key]) for r in sel],
                                   info.floor)
            ax.scatter(hs * nudge, vs, s=9, alpha=0.18, marker=st.marker,
                       color=st.color, edgecolors="none", zorder=2)
            levels = sorted({round(float(h), 4) for h in hs})
            med, q25, q75 = [], [], []
            for level in levels:
                bucket = vs[np.isclose(hs, level, rtol=1e-3)]
                med.append(float(np.median(bucket)))
                q25.append(float(np.percentile(bucket, 25)))
                q75.append(float(np.percentile(bucket, 75)))
                at_floor = int((bucket <= info.floor).sum())
                print(f"  {key} order {order} h_rel {level:g}: n={bucket.size} "
                      f"median {fmt(med[-1])} IQR [{fmt(q25[-1])}, {fmt(q75[-1])}]"
                      + (f"  ({at_floor} at the precision floor)" if at_floor else ""))
            fit = fs.fit_loglog(levels, med)
            if fit.reportable:
                # The rate belongs with the other measured text, not in the
                # legend: as a legend suffix it doubled the key's width and
                # left no corner free for the precision note.
                rates.append(f"order {order}: measured rate h^{fit.slope:.2f} "
                             f"(r² {fit.residual:.2f})")
            ax.errorbar(np.array(levels) * nudge, med,
                        yerr=[np.array(med) - np.array(q25),
                              np.array(q75) - np.array(med)],
                        color=st.color, marker=st.marker, markersize=6,
                        linewidth=2.2, linestyle=st.dash, capsize=4, zorder=4,
                        markeredgecolor=fs.theme().bg, markeredgewidth=0.6,
                        label=f"order {order}  (n={len(sel):,})")
            if fit.reportable:
                print(f"    order {order}: measured rate h^{fit.slope:.2f} "
                      f"(r² = {fit.residual:.3f}) over {len(levels)} resolutions")
            elif len(levels) >= 2 and med[0] > 0:
                print(f"    order {order}: only {len(levels)} resolutions — "
                      "too few for a rate; coarsest "
                      f"{levels[-1]:g} -> finest {levels[0]:g} improves {key} "
                      f"by {med[-1] / med[0]:.2f}x")
        ax.set_xscale("log")
        ax.set_xticks(all_levels)
        ax.set_xticklabels([f"{level:g}" for level in all_levels])
        ax.minorticks_off()
        ax.set_xmargin(0.25)
        # Ascending h_rel, left to right. The old panel put decreasing numbers
        # under a "finer →" arrow, which reads as a reversed axis.
        ax.set_xlabel("h_rel  (element size / diagonal)  —  finer on the left")
        ax.set_ylabel(f"{label}  (model units)")
        fs.panel_title(ax, f"{label} vs resolution")
        ax.legend(loc="upper left")
        corner = rates + ([info.note("at machine precision")]
                          if info.clamped else [])
        if corner:
            ax.text(0.985, 0.97, "\n".join(corner),
                    transform=ax.transAxes, ha="right", va="top",
                    fontsize=fs.FONT_PT["annot"] - 1.0,
                    color=fs.theme().muted, linespacing=1.3)

    save(fig, out_dir, "fidelity_vs_h.png")
    return True


# --- figure 5: mesh_before_after.png ----------------------------------------


#: Renders are read from the campaign warehouse. ``wire_feature.png`` is the
#: bore-framed camera and is preferred when present: on a flat plate the
#: whole-part camera puts the hole rim edge-on, which is exactly the detail a
#: before/after pair is about.
#:
#: It is written by::
#:
#:     python scripts/warehouse_shots.py <campaign> \
#:         --out-name wire_feature.png --hole-zoom
#:
#: which passes ``--hole-zoom --require-hole`` to scripts/vtu_wire_png.py, so
#: the file exists ONLY for a part with a measured bore. An earlier version of
#: this comment credited a ``--feature`` flag that has never existed; because
#: nothing could write the preferred name, the fallback below was the
#: permanent behaviour and the edge-on rim survived every later pass. Check
#: the flag against the script before trusting a comment like this one.
WIRE_NAMES = ("wire_feature.png", "wire.png")


def wire_path(campaigns_dir: Path, row: dict[str, str]) -> Path | None:
    base = (campaigns_dir / row["campaign"] / "runs" / row["cfg_id"]
            / row["part"] / "t0")
    for name in WIRE_NAMES:
        if (base / name).is_file():
            return base / name
    return None


def _ink(path: Path, tol: int = 18) -> tuple[Any, tuple[int, int, int, int]]:
    """Warehouse render -> dark wireframe on the page colour, plus its bbox.

    The warehouse writes near-black lines on a flat saturated background. That
    background is a render setting, not data: it carries no meaning, fights
    the rest of the report and hides the thinnest wires. Everything that is
    not background becomes ink whose darkness tracks how far the pixel sits
    from the background, so the mesh reads as a line drawing.
    """
    image = Image.open(path).convert("RGB")
    pixels = np.asarray(image).astype(np.float32)
    corners = np.array([pixels[0, 0], pixels[0, -1], pixels[-1, 0], pixels[-1, -1]])
    background = np.median(corners, axis=0)
    distance = np.abs(pixels - background).sum(axis=2)
    mask = distance > tol
    if not mask.any():
        return image, (0, 0, image.width, image.height)
    ys, xs = np.nonzero(mask)
    box = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    strength = np.clip(distance / max(float(distance.max()), 1.0), 0.0, 1.0)
    strength = strength ** 0.6  # lift the faint wires out of the background
    page = np.array(matplotlib.colors.to_rgb(fs.theme().panel)) * 255.0
    ink = np.array(matplotlib.colors.to_rgb(fs.theme().ink)) * 255.0
    blended = page + (ink - page) * strength[..., None]
    return Image.fromarray(blended.astype(np.uint8), "RGB"), box


def matched_pair(paths: Sequence[Path], pad: int = 12,
                 target: int = 620) -> list[Any]:
    """Crop a set of renders to ONE common scale and ONE common canvas.

    A before/after pair drawn at different apparent sizes is not a comparison.
    Every panel here is cropped to the union subject box, scaled by the same
    factor and padded onto the same canvas, so a size difference on the page
    is a size difference in the mesh.
    """
    loaded = [_ink(path) for path in paths]
    width = max(box[2] - box[0] for _, box in loaded) + 2 * pad
    height = max(box[3] - box[1] for _, box in loaded) + 2 * pad
    scale = min(1.0, target / max(width, height))
    canvas_size = (max(1, round(width * scale)), max(1, round(height * scale)))

    out = []
    for image, box in loaded:
        crop = image.crop((box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad))
        crop = crop.resize((max(1, round(crop.width * scale)),
                            max(1, round(crop.height * scale))), Image.LANCZOS)
        canvas = Image.new("RGB", canvas_size,
                           tuple(round(c * 255) for c in
                                 matplotlib.colors.to_rgb(fs.theme().panel)))
        canvas.paste(crop, ((canvas_size[0] - crop.width) // 2,
                            (canvas_size[1] - crop.height) // 2))
        out.append(canvas)
    return out


def _caption(row: dict[str, str]) -> str:
    text = (f"h_rel {to_float(row['h_rel']):.3g}  ·  order {row['order']}  ·  "
            f"{fs.series(row['mesher']).label}\n"
            f"{int(to_float(row['n_dof'])):,} DOF  ·  "
            f"rel_err {to_float(row['accuracy_rel_err']):.3g}")
    fs.assert_glyphs(text)
    return text


def pick_before_after(rows: list[dict[str, str]], campaigns_dir: Path,
                      n_parts: int = 3) -> list[tuple[str, dict, dict]]:
    """Per family, the part whose coarse->best accuracy gain is largest."""
    usable = [r for r in corpus_rows(rows)
              if r["status"] == OK_STATUS
              and math.isfinite(to_float(r["accuracy_rel_err"]))
              and math.isfinite(to_float(r["h_rel"]))
              and wire_path(campaigns_dir, r) is not None]
    by_part: dict[str, list[dict[str, str]]] = {}
    for row in usable:
        by_part.setdefault(row["part"], []).append(row)

    best_per_family: dict[str, tuple[float, str, dict, dict]] = {}
    for part, part_rows in sorted(by_part.items()):
        if len(part_rows) < 2:
            continue
        # Baseline = the coarsest mesh actually run: largest h_rel, and among
        # those the least resolved. Without the dof tie-break the "before"
        # tile can end up finer than the "after" one, since a single h_rel
        # level covers both element orders.
        coarse = max(part_rows, key=lambda r: (to_float(r["h_rel"]),
                                               -to_float(r["n_dof"])))
        best = min(part_rows, key=lambda r: to_float(r["accuracy_rel_err"]))
        if coarse is best or to_float(coarse["accuracy_rel_err"]) <= to_float(
                best["accuracy_rel_err"]):
            continue
        gain = to_float(coarse["accuracy_rel_err"]) / to_float(best["accuracy_rel_err"])
        family = family_of(part)
        if family not in best_per_family or gain > best_per_family[family][0]:
            best_per_family[family] = (gain, part, coarse, best)

    ranked = sorted(best_per_family.values(), key=lambda item: -item[0])
    return [(part, coarse, best) for _, part, coarse, best in ranked[:n_parts]]


def mesh_before_after(rows: list[dict[str, str]], campaigns_dir: Path,
                      out_dir: Path) -> bool:
    if Image is None:
        print("no data yet — Pillow is not importable (pip install pillow); "
              "skipping mesh_before_after.png")
        return False
    picks = pick_before_after(rows, campaigns_dir)
    if not picks:
        print(f"no data yet — no warehouse renders "
              f"({' or '.join(WIRE_NAMES)}) under "
              f"{campaigns_dir}/<campaign>/runs/<cfg_id>/<part>/t0/; "
              "skipping mesh_before_after.png")
        return False

    print(f"\nmesh_before_after.png — {len(picks)} parts, real warehouse renders")
    used = [wire_path(campaigns_dir, row)  # type: ignore[misc]
            for _, coarse, best in picks for row in (coarse, best)]
    cameras = {path.name for path in used}  # type: ignore[union-attr]
    # Name the render that was used, never the property it is hoped to have.
    # wire_feature.png is written by scripts/vtu_wire_png.py --hole-zoom, whose
    # detect_hole_roi ESTIMATES a ring and always returns one, hole or no hole;
    # calling that "feature-framed" on the face of the figure would assert a
    # framing nothing verified. Say which camera produced it and let the
    # panels speak.
    framing = ("hole-zoom ROI render" if cameras == {"wire_feature.png"}
               else "whole-part render" if cameras == {"wire.png"}
               else "mixed cameras: " + ", ".join(sorted(cameras)))
    print(f"  camera: {framing} ({', '.join(sorted(cameras))})")

    # Which parts appear is decided ONLY by the coarse->best gain, so say so,
    # and say which families were available. Picking the parts that happen to
    # have a bore-framed render would make the camera choose the data.
    shown = [family_of(part) for part, _, _ in picks]
    bores = sorted({family_of(path.parts[-3])
                    for path in campaigns_dir.rglob("wire_feature.png")})
    selection = (f"rows are the {len(picks)} families with the largest "
                 f"coarse->best accuracy gain ({', '.join(shown)}), chosen on "
                 "the gain alone")
    if bores and not any(family in bores for family in shown):
        selection += (f"; the bore-framed camera exists only for "
                      f"{', '.join(bores)}, which no row here belongs to, so "
                      "every panel is the whole-part camera")

    fig, axes = fs.figure(
        "Meshes before and after — the coarsest run beside the best-accuracy "
        "run for the same part",
        subtitle=("each row is one part; both panels in a row are cropped to "
                  f"the same scale and the same canvas · {framing}\n"
                  f"{selection}"),
        # Stamp the renders actually drawn, not the warehouse root: a digest
        # folded over 20,000 unrelated campaign files identifies nothing.
        footer=fs.footer_source(*used, ADVISOR_DIR / "dataset.csv",
                                note=f"{len(picks)} parts"),
        # One row per part, but the aspect cap is a FLOOR on the height, not a
        # ceiling: at a single qualifying part 10.6 x 4.1 is a 2.59:1
        # letterbox and fs.figure rightly refuses it. Grow the canvas instead
        # of dropping the part.
        size=(10.6, max(4.1 * len(picks), 10.6 / fs.MAX_ASPECT)),
        nrows=len(picks), ncols=2, share_y_axis=False)

    for row_index, (part, coarse, best) in enumerate(picks):
        gain = to_float(coarse["accuracy_rel_err"]) / to_float(best["accuracy_rel_err"])
        print(f"  {part} ({family_of(part)}): "
              f"coarse h_rel {to_float(coarse['h_rel']):.3g} order {coarse['order']} "
              f"{coarse['mesher']} n_dof {int(to_float(coarse['n_dof']))} "
              f"rel_err {fmt(to_float(coarse['accuracy_rel_err']))}"
              f"  ->  best h_rel {to_float(best['h_rel']):.3g} "
              f"order {best['order']} {best['mesher']} "
              f"n_dof {int(to_float(best['n_dof']))} "
              f"rel_err {fmt(to_float(best['accuracy_rel_err']))}"
              f"  ({gain:.1f}x better, "
              f"{to_float(best['n_dof']) / to_float(coarse['n_dof']):.1f}x the dof)")

        paths = [wire_path(campaigns_dir, coarse), wire_path(campaigns_dir, best)]
        images = matched_pair([p for p in paths if p is not None])
        panels = [("baseline — coarsest run", coarse),
                  (f"best accuracy — {gain:.0f}× lower rel_err", best)]
        for col_index, (title, row) in enumerate(panels):
            ax = axes[row_index][col_index]
            ax.set_xticks([])
            ax.set_yticks([])
            for side in ax.spines.values():
                side.set_color(fs.theme().rule)
            if paths[col_index] is None or col_index >= len(images):
                ax.text(0.5, 0.5, "render missing", ha="center", va="center",
                        transform=ax.transAxes, color=fs.theme().muted)
                continue
            # source and axes are near 1:1, so nearest keeps the wire lines
            # crisp instead of smearing them into grey.
            ax.imshow(np.asarray(images[col_index]), interpolation="nearest")
            fs.panel_title(ax, f"{title}\n{_caption(row)}")
            ax.title.set_linespacing(1.45)
        axes[row_index][0].set_ylabel(part, fontsize=fs.FONT_PT["label"],
                                      weight="bold", labelpad=8)

    save(fig, out_dir, "mesh_before_after.png")
    return True


# --- figure 6: external_comparison.png --------------------------------------


def _external_tolerance(case_id: str, metric_name: str) -> float | None:
    reference = load_json(CORPUS_REFERENCE_DIR / f"{case_id}.json")
    if not isinstance(reference, dict):
        return None
    base_name = metric_name.removesuffix("_rel_err")
    for metric in reference.get("metrics", []):
        if metric.get("name") != base_name:
            continue
        tolerance = to_float(metric.get("tol"))
        return tolerance if tolerance > 0.0 else None
    return None


#: Outcome states for the per-panel strip, in legend order. A refusal is the
#: mesher declining an h it cannot represent — an engineering answer, and the
#: plurality outcome in this matrix — so it is drawn as its own informative
#: state: never a gap in a line, never a failure. ``failed`` stays visually
#: separate because that distinction is real upstream. Each state carries a
#: hatch as well as a hue, so the strip survives greyscale printing.
OUTCOME_ORDER = ["measured", "refused", "failed", "timeout"]
OUTCOME_LABELS = {
    "measured": "measured",
    "refused": "refused — mesher declined this h",
    "failed": "failed — real error",
    "timeout": "over the time budget",
}


def _outcome_cell(state: str) -> tuple[str, str, str]:
    """(face, edge, hatch) for one outcome cell, from the active theme."""
    t = fs.theme()
    return {
        "measured": (tint(t.ink, 0.22), t.ink, ""),
        "refused": (tint(t.warn, 0.62), t.warn, "///"),
        "failed": (tint(t.bad, 0.35), t.bad, "xxx"),
        "timeout": (tint(t.muted, 0.72), t.muted, "..."),
    }[state]


def _external_rows(payload: list[Any]) -> list[dict[str, Any]]:
    """Every peer row, classified — refusals and failures kept, not dropped.

    The old filter required a finite ``accuracy.value``, which silently threw
    away 166 of 336 rows and made a refusal indistinguishable from a run that
    never happened.
    """
    out: list[dict[str, Any]] = []
    for row in payload:
        if not isinstance(row, dict) or row.get("solver") not in EXTERNAL_SOURCES:
            continue
        case_id = row.get("case_id")
        order = row.get("order")
        h_rel = to_float(row.get("h_rel"))
        if (not isinstance(case_id, str) or not isinstance(order, int)
                or not math.isfinite(h_rel) or h_rel <= 0):
            continue
        status = str(row.get("status", ""))
        dofs = to_float(row.get("dofs"))
        error = to_float((row.get("accuracy") or {}).get("value"))
        metric = str((row.get("accuracy") or {}).get("name", ""))
        tolerance = _external_tolerance(case_id, metric)
        measured = (status == OK_STATUS and math.isfinite(dofs) and dofs > 0
                    and math.isfinite(error) and error >= 0.0)
        if measured:
            state = "measured"
        elif status == "refused":
            state = "refused"
        elif status == "timeout":
            state = "timeout"
        else:
            # Anything else produced no number without the engine declining,
            # which is an error — including an ``ok`` row whose metric is
            # missing. Calling that a refusal would launder a defect.
            state = "failed"
        refusal = row.get("refusal")
        refusal = refusal if isinstance(refusal, dict) else {}
        diagnosis = row.get("diagnosis")
        diagnosis = diagnosis if isinstance(diagnosis, dict) else {}
        promotion = row.get("promotion")
        promotion = promotion if isinstance(promotion, dict) else {}
        out.append({
            "solver": row["solver"],
            "case_id": case_id,
            "family": family_of(case_id),
            "order": order,
            "h_rel": round(h_rel, 4),
            "state": state,
            "dofs": dofs,
            "error": error,
            "metric": metric,
            "tolerance": tolerance,
            # error in units of the case's own tolerance: 1.0 is the pass line
            "ratio": (error / tolerance) if (measured and tolerance) else math.nan,
            "refusal_kind": str(refusal.get("kind", "")),
            "recommended_h": to_float(refusal.get("recommended_h_m")),
            "diagnosis_kind": str(diagnosis.get("kind", "")),
            "order_pairing": str(promotion.get("order_pairing") or ""),
        })
    return out


def _rung_note(cells: list[dict[str, Any]]) -> list[str]:
    """Two label lines for one h_rel group: how many declined, and the h to use.

    Always two lines, empty when nothing declined, so every strip's tick
    labels are the same height and the row gaps stay even.
    """
    refused = [row for row in cells if row["state"] == "refused"]
    if not refused:
        return ["none declined", ""]
    recommended = [row["recommended_h"] for row in refused
                   if math.isfinite(row["recommended_h"]) and row["recommended_h"] > 0]
    silent = len(refused) - len(recommended)
    head = f"{len(refused)} declined" + (f" ({silent} no h)" if silent else "")
    if not recommended:
        # the fill-stage guard refuses without naming a size; saying so beats
        # inventing one.
        return [head, "no h stated"]
    return [head, f"h ≤ {min(recommended):.3g} m"]


def _outcome_strip(ax: Any, panel: list[dict[str, Any]], variants: list[str],
                   cases: list[str], rungs: list[float],
                   label_variants: bool) -> list[str]:
    """One cell per (variant, case, h_rel): measured / refused / failed.

    Drawn as its own thin axes under the convergence panel rather than as
    markers inside it: the convergence panel's x-axis is DOF, and a refusal
    has no DOF at all, so it cannot honestly be placed there.
    """
    t = fs.theme()
    width = len(rungs) * len(cases)
    by_key = {(row["solver"], row["case_id"], row["h_rel"]): row for row in panel}
    for column, (rung_index, case_index) in enumerate(
            (r, c) for r in range(len(rungs)) for c in range(len(cases))):
        for variant_index, variant in enumerate(variants):
            row = by_key.get((variant, cases[case_index], rungs[rung_index]))
            y = len(variants) - 1 - variant_index
            if row is None:
                ax.add_patch(Rectangle((column + 0.08, y + 0.12), 0.84, 0.76,
                                       facecolor="none", edgecolor=t.grid,
                                       linewidth=0.6, zorder=2))
                continue
            face, edge, hatch = _outcome_cell(row["state"])
            ax.add_patch(Rectangle((column + 0.08, y + 0.12), 0.84, 0.76,
                                   facecolor=face, edgecolor=edge, hatch=hatch,
                                   linewidth=0.7, zorder=2))
    for rung_index in range(1, len(rungs)):
        ax.axvline(rung_index * len(cases), color=t.rule, linewidth=0.9,
                   zorder=3)
    ax.set_xlim(0, width)
    ax.set_ylim(0, len(variants))
    ax.set_xticks([(index + 0.5) * len(cases) for index in range(len(rungs))])
    notes = []
    labels = []
    for rung in rungs:
        cells = [row for row in panel if row["h_rel"] == rung]
        note = _rung_note(cells)
        notes.append(f"h_rel {rung:g}: "
                     + ", ".join(line for line in note if line))
        labels.append("\n".join([f"h_rel {rung:g}", *note]))
    ax.set_xticklabels(labels, fontsize=fs.FONT_PT["annot"] - 2.5,
                       color=fs.theme().muted, linespacing=1.4)
    ax.tick_params(axis="x", length=0)
    ax.set_yticks([index + 0.5 for index in range(len(variants))])
    if label_variants:
        ax.set_yticklabels([fs.series(variant).label
                            for variant in reversed(variants)],
                           fontsize=fs.FONT_PT["annot"] - 1.0)
    else:
        ax.set_yticklabels([])
    ax.tick_params(axis="y", length=0)
    for side in ax.spines.values():
        side.set_visible(False)
    ax.set_axisbelow(True)
    return notes


def _place_rung_labels(fig: Any, panels: Sequence[tuple[Any, Sequence[
        tuple[float, float, str, str]]]], fontsize: float) -> int:
    """Label each point with its h_rel, choosing offsets that do not collide.

    Several mesh variants land on nearly the same (dof, error/tol) in these
    panels, so a fixed offset rule always buried one label under another. Each
    label is instead tried against the ones already placed, in axes-fraction
    space, and takes the first free slot. Returns the number of labels that
    found no free slot, so the figure never claims a placement it did not get.
    """
    # tight_layout here only to read the near-final axes geometry; figstyle's
    # finish() lays the figure out again with its own rect before saving.
    try:
        fig.tight_layout()
    except Exception:
        pass
    candidates = [(5.0, 4.0), (-6.0, 4.0), (5.0, -11.0), (-6.0, -11.0),
                  (5.0, 15.0), (-6.0, 15.0), (5.0, -22.0), (-6.0, -22.0),
                  (5.0, 26.0), (-6.0, 26.0), (5.0, -33.0), (-6.0, -33.0)]
    crowded = 0
    for ax, entries in panels:
        box = ax.get_position()
        width_pt = box.width * fig.get_size_inches()[0] * 72.0
        height_pt = box.height * fig.get_size_inches()[1] * 72.0
        x_lo, x_hi = (math.log10(value) for value in ax.get_xlim())
        y_lo, y_hi = (math.log10(value) for value in ax.get_ylim())
        placed: list[tuple[float, float, float, float]] = []
        for x, y, text, color in entries:
            # 0.58 em per character is the measured average for this font at
            # label sizes; exact metrics need a renderer we do not have yet.
            w = 0.58 * fontsize * len(text) / width_pt
            h = 1.25 * fontsize / height_pt
            fx = (math.log10(x) - x_lo) / (x_hi - x_lo)
            fy = (math.log10(y) - y_lo) / (y_hi - y_lo)
            chosen = None
            for dx, dy in candidates:
                left = fx + (dx / width_pt if dx > 0 else (dx / width_pt) - w)
                bottom = fy + (dy / height_pt if dy > 0
                               else (dy / height_pt) - h)
                if left < 0.01 or left + w > 0.99:
                    continue
                if bottom < 0.01 or bottom + h > 0.99:
                    continue
                if any(left < px + pw and px < left + w
                       and bottom < py + ph and py < bottom + h
                       for px, py, pw, ph in placed):
                    continue
                chosen = (dx, dy, left, bottom)
                break
            if chosen is None:
                crowded += 1
                continue
            dx, dy, left, bottom = chosen
            placed.append((left, bottom, w, h))
            ax.annotate(text, (x, y), xytext=(dx, dy),
                        textcoords="offset points",
                        ha="left" if dx > 0 else "right",
                        fontsize=fontsize, color=color, zorder=5)
    return crowded


def _order_tally(measured: list[dict[str, Any]], families: list[str],
                 order: int) -> list[tuple[str, str, float, float]]:
    """(family, best native variant, its median rel_err, Gmsh median) per family."""
    out = []
    for family in families:
        panel = [row for row in measured
                 if row["family"] == family and row["order"] == order]
        peer = [row["error"] for row in panel
                if row["solver"] == "gmsh-mesh+polymesh-solver"]
        best: tuple[str, float] | None = None
        for solver in EXTERNAL_SOURCES:
            if solver == "gmsh-mesh+polymesh-solver":
                continue
            errors = [row["error"] for row in panel if row["solver"] == solver]
            if not errors:
                continue
            median = float(np.median(errors))
            if best is None or median < best[1]:
                best = (solver, median)
        if not peer or best is None:
            continue
        out.append((family, best[0], best[1], float(np.median(peer))))
    return out


def _cost_tally(measured: list[dict[str, Any]], families: list[str],
                order: int) -> list[tuple[str, float, float]]:
    """(family, best native median rel_err x DOF, Gmsh median) per family.

    The accuracy tally answers "whose mesh is more accurate". It does not
    answer "at what cost", and on this matrix the two answers differ: the
    native wins are bought with 4-10x the degrees of freedom. Charging for
    them is one multiplication, so there is no excuse for showing only the
    flattering basis.
    """
    out = []
    for family in families:
        panel = [row for row in measured
                 if row["family"] == family and row["order"] == order
                 and row.get("dofs")]
        peer = [row["error"] * row["dofs"] for row in panel
                if row["solver"] == "gmsh-mesh+polymesh-solver"]
        native = [row["error"] * row["dofs"] for row in panel
                  if row["solver"] != "gmsh-mesh+polymesh-solver"]
        best: float | None = None
        for solver in EXTERNAL_SOURCES:
            if solver == "gmsh-mesh+polymesh-solver":
                continue
            costs = [row["error"] * row["dofs"] for row in panel
                     if row["solver"] == solver]
            if not costs:
                continue
            median = float(np.median(costs))
            if best is None or median < best:
                best = median
        if not peer or best is None or not native:
            continue
        out.append((family, best, float(np.median(peer))))
    return out


def external_comparison(result_path: Path, out_dir: Path) -> bool:
    payload = load_json(result_path)
    if not isinstance(payload, list):
        print(f"no data yet — expected a JSON row array at {result_path}; "
              "skipping external_comparison.png")
        return False

    rows = _external_rows(payload)
    measured = [row for row in rows if row["state"] == "measured"]
    plotted = [row for row in measured if math.isfinite(row["ratio"])]
    if not plotted:
        print(f"no data yet — no measured external-comparison rows in "
              f"{result_path}; skipping external_comparison.png")
        return False

    preferred = ("box_hole", "stepped_shaft")
    present_families = {row["family"] for row in rows}
    families = [family for family in preferred if family in present_families]
    families.extend(sorted(present_families - set(families)))
    orders = sorted({row["order"] for row in rows})
    rungs = sorted({row["h_rel"] for row in rows}, reverse=True)
    states = {state: sum(row["state"] == state for row in rows)
              for state in OUTCOME_ORDER}
    tolerances = sorted({row["tolerance"] for row in rows
                         if row["tolerance"] is not None})

    print(f"\nexternal_comparison.png — {len(rows)} peer rows from "
          f"{result_path}: " + ", ".join(f"{state} {count}"
                                         for state, count in states.items()))
    print(f"  reference tolerances span {min(tolerances):g}–"
          f"{max(tolerances):g} (median "
          f"{float(np.median(tolerances)):g}) across {len(tolerances)} "
          "distinct values")

    tallies = {order: _order_tally(measured, families, order)
               for order in orders}
    wins = {order: sum(1 for _, _, native, peer in tallies[order]
                       if native < peer) for order in orders}
    for order in orders:
        for family, solver, native, peer in tallies[order]:
            verdict = "ours lower" if native < peer else "Gmsh lower"
            print(f"  order {order} {family}: best native "
                  f"{fs.series(solver).label} median rel_err {native:.4f} vs "
                  f"Gmsh mesh {peer:.4f} — {verdict}")
        print(f"  order {order} tally: ours lower in {wins[order]}/"
              f"{len(tallies[order])} families")

    order1 = orders[0]
    reversal = ("; ".join(f"{family} {native:.4f} vs {peer:.4f}"
                          for family, _, native, peer in tallies[order1]))

    cost = _cost_tally(measured, families, order1)
    cost_wins = sum(1 for _, native, peer in cost if native < peer)
    for family, native, peer in cost:
        print(f"  order {order1} {family} rel_err x DOF: ours "
              f"{native:.0f} vs Gmsh {peer:.0f} — "
              + ("ours lower" if native < peer else "Gmsh lower"))
    print(f"  order {order1} cost tally: ours lower in {cost_wins}/"
          f"{len(cost)} families on rel_err x DOF")
    cost_sentence = ""
    if cost:
        losers = ", ".join(family for family, native, peer in cost
                           if native >= peer)
        cost_sentence = (
            " \u00b7 those wins are bought with degrees of freedom: on median "
            f"rel_err x DOF ours is lower in only {cost_wins}/{len(cost)} "
            "families"
            + (f", with Gmsh more economical on {losers}" if losers else "")
            + " \u2014 more accurate per case is not the same as cheaper per case")
    findings = ROOT / "bench" / "reference" / "external" / \
        "external-truth-findings.json"
    # the parity vocabulary is read off promotion.order_pairing, so no caveat
    # in this figure can outlive the variant that fixed it.
    true_parity_labels = sorted({fs.series(row["solver"]).label for row in rows
                                 if "TRUE PARITY" in row["order_pairing"]})
    approx_labels = sorted({fs.series(row["solver"]).label for row in rows
                            if "APPROXIMATE" in row["order_pairing"]})
    parity_sentence = ""
    if true_parity_labels:
        parity_sentence = (
            " · promotion.order_pairing per row: "
            f"{', '.join(true_parity_labels)} = TRUE PARITY against Gmsh's "
            "uniformly quadratic tet10 (every element promoted)")
        if approx_labels:
            parity_sentence += (f"; {', '.join(approx_labels)} = APPROXIMATE, "
                                "selective p-elevate promotes a marked subset")

    # error/tolerance, not raw rel_err: tolerances are now 0.02–0.087 while
    # errors run 0.004–0.8, so a shaded band on a linear axis is a hairline at
    # the axis floor. Dividing by each case's own tolerance puts the pass line
    # at exactly 1.0 in every panel and makes panels with different tolerances
    # directly comparable, which no shared linear y-axis can do. The axis stays
    # logarithmic because the ratios still span three decades.
    # A tolerance-normalised axis invites exactly one question — how many
    # configurations are actually inside tolerance — so answer it instead of
    # leaving the reader to count markers in the shaded band.
    ratios = [row["error"] / row["tolerance"] for row in rows
              if row.get("state") == "measured" and row.get("tolerance")]
    inside = sum(1 for r in ratios if r <= 1.0)
    pass_sentence = ""
    if ratios:
        best = min(ratios)
        pass_sentence = (
            f" · {inside} of {len(ratios)} measured configurations are inside "
            f"tolerance; the closest is {best:.2f}x the tolerance, so at these "
            "resolutions neither mesh source reaches reference accuracy on "
            "most cases — the comparison is which source is closer, not which "
            "one passes")
    subtitle = (
        "y is rel_err ÷ that case's own reference tolerance, so 1.0 is the "
        "pass line in every panel and panels with different tolerances "
        f"(now {min(tolerances):g}–{max(tolerances):g}, was a hand-picked "
        "0.15) are comparable · the strip under each panel is one cell per "
        "(variant, case, h_rel): a refusal is the mesher declining an h it "
        f"cannot represent ({states['refused']} of {len(rows)} rows, the "
        "plurality) and is shown as an outcome, not as a gap; a failure is a "
        f"real error ({states['failed']}) and stays separate"
        + parity_sentence + pass_sentence + " · order "
        f"{order1}: our best native variant now has the lower median rel_err "
        f"in {wins[order1]}/{len(tallies[order1])} families ({reversal}), "
        "REVERSING the previous regeneration — those earlier numbers were "
        "measured on an engine that silently deleted the bore, so this is a "
        "corrected measurement and not a method improvement (see "
        "external-truth-findings.json and "
        "docs/validation/figures/hole_aliasing.png)"
        + cost_sentence)

    # two axes rows per element order: the convergence panel and its outcome
    # strip. Width grows with the family count; the height is generous because
    # four axes rows plus three-line strip labels need the room, and is held at
    # or above width / MAX_ASPECT so the grid can never letterbox.
    width = 4.7 * len(families)
    height = max(5.6 * len(orders), width / fs.MAX_ASPECT)
    fig, axes = fs.figure(
        "Mesh source against third-party reference truth — error in units of "
        "the reference tolerance",
        subtitle=subtitle,
        footer=fs.footer_source(result_path, CORPUS_REFERENCE_DIR, findings,
                                n=len(rows)),
        size=(width, height),
        nrows=2 * len(orders), ncols=len(families),
        share_y_axis="the outcome strips are categorical and share nothing "
                     "with the convergence panels; the convergence panels are "
                     "put on one common ratio axis explicitly below",
        gridspec_kw={"height_ratios": [3.0, 1.5] * len(orders)})

    shown_solvers: list[str] = []
    chart_axes = []
    deferred_labels: list[tuple[Any, list[tuple[float, float, str, str]]]] = []
    for order_index, order in enumerate(orders):
        for family_index, family in enumerate(families):
            ax = axes[2 * order_index][family_index]
            strip_ax = axes[2 * order_index + 1][family_index]
            panel = [row for row in rows
                     if row["family"] == family and row["order"] == order]
            if not panel:
                ax.set_visible(False)
                strip_ax.set_visible(False)
                continue
            chart_axes.append(ax)

            fs.tolerance_band(ax, 1.0, label="tolerance = 1.0")

            rung_labels: list[tuple[float, float, str, str]] = []
            variants = [solver for solver in EXTERNAL_SOURCES
                        if any(row["solver"] == solver for row in panel)]
            for solver in variants:
                source_rows = [row for row in panel
                               if row["solver"] == solver
                               and row["state"] == "measured"
                               and math.isfinite(row["ratio"])]
                if not source_rows:
                    continue
                points = []
                for h_rel in sorted({row["h_rel"] for row in source_rows},
                                    reverse=True):
                    rung = [row for row in source_rows if row["h_rel"] == h_rel]
                    points.append((
                        float(np.median([row["dofs"] for row in rung])),
                        float(np.median([row["ratio"] for row in rung])),
                        h_rel,
                    ))
                points.sort(key=lambda point: point[0])
                xs = [point[0] for point in points]
                ys = [point[1] for point in points]
                st = fs.series(solver)
                fit = fs.fit_loglog(xs, ys)
                if fit.reportable:
                    ax.plot(xs, ys, color=st.color, linestyle=st.dash,
                            linewidth=2.0, zorder=2)
                ax.scatter(xs, ys, marker=st.marker, s=52, color=st.color,
                           edgecolor=fs.theme().bg, linewidth=0.7, zorder=3)
                merged: list[tuple[float, float, list[float]]] = []
                for x, y, h_rel in points:
                    if merged and math.isclose(merged[-1][0], x, rel_tol=0.02):
                        merged[-1][2].append(h_rel)
                    else:
                        merged.append((x, y, [h_rel]))
                for x, y, hs in merged:
                    rung_labels.append((x, y, "/".join(f"{h:g}"
                                                       for h in sorted(hs)),
                                        st.color))
                if solver not in shown_solvers:
                    shown_solvers.append(solver)
                rate = (f"; rate dof^{fit.slope:.2f}" if fit.reportable
                        else f"; {len(points)} resolution(s) — no rate stated")
                print(f"  {family} order {order} {st.label}: "
                      + ", ".join(
                          f"h={h_rel:g} median(dof={dofs:.0f}, "
                          f"err/tol={ratio:.3g})"
                          for dofs, ratio, h_rel in points) + rate)

            panel_ratios = [row["ratio"] for row in panel
                            if math.isfinite(row["ratio"])]
            if panel_ratios:
                fs.loglim(ax, panel_ratios + [1.0], draw_floor=False)
            # room on the right so the last point does not land on the
            # tolerance-line caption, which figstyle pins to the right edge
            ax.set_xscale("log")
            ax.autoscale_view()
            x_lo, x_hi = ax.get_xlim()
            ax.set_xlim(x_lo / 1.2, x_hi * 4.0)

            # the h_rel labels are placed after the shared y-range is fixed,
            # by _place_rung_labels: several variants land on nearly the same
            # (dof, ratio) in these panels, so the offsets have to be chosen
            # against the other labels rather than by a fixed rule.
            deferred_labels.append((ax, rung_labels))

            family_label = {
                "box_hole": "Box-hole SCF",
                "stepped_shaft": "Stepped-shaft tip deflection",
            }.get(family, family.replace("_", " ").title())
            # the order-2 parity claim is read off promotion.order_pairing, so
            # the blanket "approx. native parity" caveat can no longer outlive
            # the variant that fixed it.
            parity = []
            true_parity = sorted({fs.series(row["solver"]).label
                                  for row in panel
                                  if "TRUE PARITY" in row["order_pairing"]})
            approximate = sorted({fs.series(row["solver"]).label
                                  for row in panel
                                  if "APPROXIMATE" in row["order_pairing"]})
            if true_parity:
                parity.append(f"true parity: {', '.join(true_parity)} "
                              "vs Gmsh tet10")
            if approximate:
                parity.append(f"approx. parity: {', '.join(approximate)}")
            # one title line in every panel: a multi-line left-aligned title
            # forces tight_layout to open the same gap between every axes row,
            # which pushed each outcome strip away from the panel it belongs
            # to. The parity statement therefore lives inside the axes.
            fs.panel_title(ax, f"{family_label} · order {order}")
            if parity:
                ax.text(0.985, 0.985, "\n".join(
                    line for bit in parity for line in textwrap.wrap(bit, 44)),
                    transform=ax.transAxes, ha="right", va="top",
                    fontsize=fs.FONT_PT["annot"] - 0.5, color=fs.theme().ink,
                    linespacing=1.35, zorder=6)
            ax.set_xlabel("active degrees of freedom  (log scale)")
            if family_index == 0:
                ax.set_ylabel("rel_err ÷ reference tolerance\n"
                              "(≤ 1 passes; log scale)")
            # lower right: inside the shaded pass region, which no series
            # reaches on its right-hand side in any panel.
            fs.annotate_n(ax, sum(row["state"] == "measured" for row in panel),
                          excluded=sum(row["state"] != "measured"
                                       for row in panel),
                          what="measured", loc="lower right",
                          extra="excluded rows are in the strip below")

            cases = sorted({row["case_id"] for row in panel})
            notes = _outcome_strip(strip_ax, panel, variants, cases,
                                   [rung for rung in rungs
                                    if any(row["h_rel"] == rung
                                           for row in panel)],
                                   label_variants=family_index == 0)
            print(f"  {family} order {order} outcomes: "
                  + " | ".join(notes))

    # every panel is now in the same unit (multiples of tolerance), so one
    # common y-range across the whole figure is the honest treatment.
    fs.share_y(chart_axes)

    crowded = _place_rung_labels(fig, deferred_labels,
                                 fs.FONT_PT["annot"] - 1.5)
    if crowded:
        print(f"  {crowded} h_rel label(s) had no free slot and were left "
              "off; the printed medians above carry those rungs")

    handles = fs.series_handles(shown_solvers)
    handles.append(Line2D([0], [0], color=fs.theme().band, linewidth=1.2,
                          linestyle=(0, (4, 2)), label="tolerance = 1.0"))
    for state in OUTCOME_ORDER:
        if not states[state]:
            continue
        face, edge, hatch = _outcome_cell(state)
        handles.append(Patch(facecolor=face, edgecolor=edge, hatch=hatch,
                             label=f"{OUTCOME_LABELS[state]} "
                                   f"({states[state]})"))
    # one row, in the free band between the last strip's labels and the
    # two-line provenance footer: two rows collided with both.
    fig.legend(handles=handles, ncol=len(handles), loc="lower center",
               bbox_to_anchor=(0.5, 0.040), frameon=False,
               fontsize=fs.FONT_PT["legend"] - 0.5)
    print(f"  h_rel rungs present: {', '.join(f'{r:g}' for r in rungs)}")
    save(fig, out_dir, "external_comparison.png")
    return True


# --- entry point ------------------------------------------------------------


def main() -> int:
    args = parse_args()
    advisor_dir = args.advisor_dir
    out_dir = args.out_dir
    if args.external_only:
        written = int(external_comparison(args.external_results, out_dir))
        print(f"\n{written}/1 figures written to {out_dir}")
        return 0
    dataset_csv = advisor_dir / "dataset.csv"

    rows = read_rows(dataset_csv)
    if rows:
        print(f"dataset: {len(rows)} rows "
              f"({len(corpus_rows(rows))} advisor-row-v3) from {dataset_csv}")
    else:
        print(f"no data yet — {dataset_csv} is missing or empty")

    written = 0
    written += network_layout(advisor_dir, out_dir)
    written += mesh_progress(rows, out_dir)
    written += accuracy_vs_cost(rows, out_dir)
    written += fidelity_vs_h(rows, out_dir)
    written += mesh_before_after(rows, args.campaigns_dir, out_dir)
    written += external_comparison(args.external_results, out_dir)
    print(f"\n{written}/6 figures written to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
