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
from pathlib import Path
from typing import Any

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
ADVISOR_DIR = ROOT / "bench" / "advisor"
CAMPAIGNS_DIR = ROOT / "bench" / "campaigns"
FIGURES_DIR = ROOT / "docs" / "advisor" / "figures"

DPI = 110  # committed PNGs stay small; every figure lands under 400 KB
MAX_PNG_BYTES = 400 * 1024

#: only these rows carry the full C2 feature/action vector (see dataset.py)
CORPUS_SCHEMA = "advisor-row-v3"
#: statuses that produced a usable solve
OK_STATUS = "ok"
#: parts are named ``<family>_s<shape>_c<case>``; the family is the stem
FAMILY_RE = re.compile(r"_s\d+_c\d+$")
#: mesh_progress starts its window once this share of cases has a first
#: result, so the anytime median is taken over a fixed set of cases
START_COVERAGE_PCT = 85.0

BLUE = "#2166ac"
ORANGE = "#b35806"
GREEN = "#1b7837"
PURPLE = "#762a83"
RED = "#b2182b"
GREY = "#4d4d4d"
LIGHT_BLUE = "#92c5de"
LIGHT_ORANGE = "#f4a582"
GRID = "#e4e1da"

MESHER_COLORS = {
    "hybrid_zoo": BLUE,
    "graded_tet": ORANGE,
    "hex": GREEN,
    "hybrid_vem": PURPLE,
    "varyhedron": RED,
    "cvt_poly": GREY,
}
ORDER_COLORS = {1: BLUE, 2: ORANGE, 3: GREEN, 4: PURPLE}

#: input-column groups drawn in network_layout.png, in trunk-input order
GROUP_STYLE = [
    ("part features", LIGHT_BLUE),
    ("case context", LIGHT_ORANGE),
    ("continuous action", "#b8e186"),
    ("categorical action", "#d9b3e6"),
]

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyArrowPatch, FancyBboxPatch
except ImportError:
    raise SystemExit(
        "matplotlib is required for report.py — install it with:\n"
        "  pip install matplotlib"
    )

try:
    from PIL import Image, ImageEnhance
except ImportError:
    Image = None  # type: ignore[assignment]
    ImageEnhance = None  # type: ignore[assignment]


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


def style_axes(ax: Any) -> None:
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    ax.grid(color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def save(fig: Any, out_dir: Path, name: str) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / name
    fig.savefig(path, dpi=DPI, facecolor="white",
                pil_kwargs={"optimize": True, "compress_level": 9})
    plt.close(fig)
    if path.stat().st_size > MAX_PNG_BYTES and Image is not None:
        # Agg writes RGBA, which compresses badly for the mesh renders. These
        # figures are flat colour plus wire lines, so a palette costs nothing
        # visually and roughly thirds the file.
        with Image.open(path) as canvas:
            palette = canvas.convert("RGB").quantize(colors=192,
                                                     method=Image.MEDIANCUT)
        palette.save(path, "PNG", optimize=True, compress_level=9)
    print(f"  wrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}"
          f"  ({path.stat().st_size / 1024:.0f} KB)")
    return path


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
         face: str, edge: str = "#33333a", fontsize: float = 8.5,
         weight: str = "normal") -> tuple[float, float]:
    ax.add_patch(FancyBboxPatch(
        (x, y), w, h, boxstyle="round,pad=0.004,rounding_size=0.008",
        facecolor=face, edgecolor=edge, linewidth=1.0, zorder=2))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
            fontsize=fontsize, zorder=3, weight=weight, linespacing=1.35)
    return x + w, y + h / 2


def _arrow(ax: Any, start: tuple[float, float], end: tuple[float, float],
           color: str = GREY, width: float = 0.9) -> None:
    ax.add_patch(FancyArrowPatch(
        start, end, arrowstyle="-|>", mutation_scale=9, linewidth=width,
        color=color, zorder=1, shrinkA=1.5, shrinkB=1.5))


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

    fig, ax = plt.subplots(figsize=(13.2, 7.0))
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
    colors = dict(GROUP_STYLE)
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
            colors.get(label, LIGHT_BLUE), fontsize=8.2)
        group_ports.append((label, (right, mid), members))
        y -= gap

    ax.text(0.1125, top + 0.045, f"input features  x[{len(shape['input_columns'])}]",
            ha="center", va="center", fontsize=11, weight="bold")
    ax.text(0.1125, top + 0.016,
            "standardized in C++ by normalization.json (mean/std/impute)",
            ha="center", va="center", fontsize=7.6, color=GREY, style="italic")

    # column 1 -- embedding tables for the two categorical columns
    emb_ports: list[tuple[float, float]] = []
    emb_y = bottom + 0.005
    for name, slots in (("mesher_idx", shape["mesher_slots"]),
                        ("order_idx", shape["order_slots"])):
        right, mid = _box(
            ax, 0.265, emb_y, 0.135, 0.075,
            f"Embedding({slots}, {emb_dim})\n{name}", "#d9b3e6", fontsize=8.2)
        emb_ports.append((right, mid))
        emb_y += 0.095
    ax.text(0.3325, emb_y + 0.012, "round + clamp, then gather\n(inside the ONNX graph)",
            ha="center", va="bottom", fontsize=7.6, color=GREY, style="italic")

    # column 2 -- concatenation into the trunk input
    concat_x, concat_w = 0.455, 0.10
    concat_y, concat_h = bottom + 0.06, span - 0.12
    _box(ax, concat_x, concat_y, concat_w, concat_h,
         f"concat\n\nD_eff = {shape['trunk_inputs']}\n\n"
         f"{n_continuous} continuous\n+ {emb_dim} order\n+ {emb_dim} mesher",
         "#f2efe9", fontsize=8.6)
    concat_left = (concat_x, concat_y + concat_h / 2)
    concat_right = (concat_x + concat_w, concat_y + concat_h / 2)

    for label, port, _ in group_ports:
        if label == "categorical action":
            for emb_right, emb_mid in emb_ports:
                _arrow(ax, port, (0.265, emb_mid), color=PURPLE)
                _arrow(ax, (emb_right, emb_mid), (concat_x, emb_mid), color=PURPLE)
        else:
            _arrow(ax, port, concat_left)

    # column 3 -- trunk
    trunk_x, trunk_w = 0.605, 0.115
    trunk_h = 0.155
    fc1_y = 0.545
    fc2_y = 0.305
    _box(ax, trunk_x, fc1_y, trunk_w, trunk_h,
         f"Linear({shape['trunk_inputs']} -> {hidden})\nGELU\n\n"
         f"{shape['fc1_parameters']:,} params", LIGHT_BLUE, fontsize=9,
         weight="bold")
    _box(ax, trunk_x, fc2_y, trunk_w, trunk_h,
         f"Linear({hidden} -> {hidden})\nGELU\n\n"
         f"{shape['fc2_parameters']:,} params", LIGHT_BLUE, fontsize=9,
         weight="bold")
    _arrow(ax, concat_right, (trunk_x, fc1_y + trunk_h / 2), width=1.4)
    _arrow(ax, (trunk_x + trunk_w / 2, fc1_y),
           (trunk_x + trunk_w / 2, fc2_y + trunk_h), width=1.4)
    ax.text(trunk_x + trunk_w / 2, top + 0.02,
            f"shared trunk\nwidth {hidden}", ha="center", va="center",
            fontsize=10, weight="bold")

    # column 4 -- heads
    head_x, head_w = 0.795, 0.185
    head_gap = 0.012
    head_h = (span - head_gap * (len(heads) - 1)) / max(len(heads), 1)
    trunk_out = (trunk_x + trunk_w, fc2_y + trunk_h / 2)
    y = top
    for name, width, params in heads:
        y -= head_h
        face = LIGHT_ORANGE if name == "policy" else (
            "#f7d8d0" if name == "failure_logit" else "#fdf0d5")
        _box(ax, head_x, y, head_w, head_h,
             f"{name}   [{width}]   {params} params",
             face, fontsize=8.6)
        _arrow(ax, trunk_out, (head_x, y + head_h / 2), width=0.8)
        y -= head_gap
    ax.text(head_x + head_w / 2, top + 0.045,
            f"output heads  ({len(heads)})", ha="center", va="center",
            fontsize=11, weight="bold")
    ax.text(head_x + head_w / 2, top + 0.016,
            "ONNX output order, top to bottom",
            ha="center", va="center", fontsize=7.6, color=GREY, style="italic")

    subtitle = (f"AdvisorNet — training run {shape['run']} — "
                f"{shape['n_parameters']:,} parameters")
    fig.suptitle(subtitle, fontsize=13, weight="bold", y=0.985)
    if drift:
        fig.text(0.5, 0.022,
                 "checkpoint predates the current dataset.py head contract "
                 f"({', '.join(contract or [])})",
                 ha="center", fontsize=8.2, color=RED, style="italic")
    fig.tight_layout(rect=(0, 0.03, 1, 0.975))
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
        ("accuracy_rel_err", "best-so-far accuracy  (rel_err)", BLUE),
        ("geo_fidelity_dist_p99", "best-so-far geometric fidelity  (dist p99)", ORANGE),
    ]
    curves: dict[str, list[tuple[np.ndarray, np.ndarray]]] = {k: [] for k, _, _ in metrics}
    horizons: list[float] = []
    for case, case_rows in sorted(by_case.items()):
        cost = np.array([to_float(r["mesh_ms"]) + to_float(r["solve_ms"])
                         for r in case_rows])
        cost = np.nan_to_num(cost, nan=0.0)
        times = np.cumsum(cost) / 1000.0  # seconds of solver wall time
        if times[-1] <= 0:
            continue
        horizons.append(float(times[-1]))
        for key, _, _ in metrics:
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
    fig, axes = plt.subplots(1, 2, figsize=(12.4, 5.0))
    print("\nmesh_progress.png — anytime curve over "
          f"{len(by_case)} cases, {len(corpus)} corpus rows")
    print(f"  cumulative solver time per case: median {horizon:.0f} s, "
          f"min {min(horizons):.0f} s, max {max(horizons):.0f} s")
    print("  a case's curve is held flat once it runs out of actions, so the "
          "population is fixed across the window")

    for ax, (key, label, color) in zip(axes, metrics):
        case_curves = curves[key]
        if not case_curves:
            ax.text(0.5, 0.5, f"no {key} data", ha="center", va="center",
                    transform=ax.transAxes, color=GREY)
            ax.axis("off")
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
        lo = np.percentile(stack, 25, axis=0)
        hi = np.percentile(stack, 75, axis=0)

        ax.fill_between(grid, lo, hi, color=color, alpha=0.20, linewidth=0,
                        label="inter-quartile band")
        ax.plot(grid, median, color=color, linewidth=2.0,
                label=f"median over {len(kept)} cases")
        ax.axvline(horizon, color=GREY, linewidth=1.0, linestyle=":",
                   label=f"median case budget ({horizon:.0f} s)")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("cumulative solver wall time per case  [s]")
        ax.set_ylabel(label)
        ax.set_title(label, fontsize=10.5)
        ax.legend(frameon=False, fontsize=8.5, loc="upper right")
        style_axes(ax)

        gain = median[0] / median[-1] if median[-1] > 0 else math.nan
        ax.annotate(f"{gain:.2f}x better", xy=(grid[-1], median[-1]),
                    xytext=(-6, 14), textcoords="offset points",
                    ha="right", fontsize=9, color=color, weight="bold")
        print(f"  {key}: median {fmt(float(median[0]))} at {t_lo:.1f} s -> "
              f"{fmt(float(median[-1]))} at {t_hi:.0f} s  ({gain:.2f}x better); "
              f"start IQR [{fmt(float(lo[0]))}, {fmt(float(hi[0]))}] -> "
              f"final IQR [{fmt(float(lo[-1]))}, {fmt(float(hi[-1]))}]; "
              f"{len(kept)} cases in the window, {dropped} started too late")

    fig.suptitle("Mesh improvement over the campaign — best result found so far "
                 "against solver time spent", fontsize=12.5, weight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.94))
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
    corpus = [r for r in corpus_rows(rows) if r["status"] == OK_STATUS]
    usable = [r for r in corpus if math.isfinite(to_float(r["accuracy_rel_err"]))]
    if not usable:
        print("no data yet — no successful corpus rows with accuracy_rel_err; "
              "skipping accuracy_vs_cost.png")
        return False

    accuracy = np.array([to_float(r["accuracy_rel_err"]) for r in usable])
    meshers = [r["mesher"] for r in usable]
    panels = [("n_dof", "degrees of freedom  n_dof"),
              ("solve_ms", "solve time  [ms]")]

    print(f"\naccuracy_vs_cost.png — {len(usable)} successful corpus rows")
    fig, axes = plt.subplots(1, 2, figsize=(12.4, 5.0), sharey=True)
    for ax, (key, xlabel) in zip(axes, panels):
        cost = np.array([to_float(r[key]) for r in usable])
        keep = np.isfinite(cost) & (cost > 0) & (accuracy > 0)
        for mesher in sorted(set(meshers)):
            sel = keep & np.array([m == mesher for m in meshers])
            if not sel.any():
                continue
            ax.scatter(cost[sel], accuracy[sel], s=11, alpha=0.45,
                       color=MESHER_COLORS.get(mesher, GREY),
                       edgecolors="none", label=f"{mesher}  (n={int(sel.sum())})")
        front = _pareto(cost[keep], accuracy[keep])
        kept_rows = [row for row, flag in zip(usable, keep) if flag]
        px, py = cost[keep][front], accuracy[keep][front]
        owners = [kept_rows[index]["part"] for index in front]
        ax.plot(px, py, color=GREY, linewidth=1.6, drawstyle="steps-post",
                zorder=4, label=f"Pareto front  ({len(px)} "
                                f"point{'' if len(px) == 1 else 's'})")
        ax.scatter(px, py, s=22, facecolors="none", edgecolors=GREY,
                   linewidths=1.1, zorder=5)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel(xlabel)
        ax.legend(frameon=False, fontsize=8, loc="lower left")
        style_axes(ax)

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
            ax.text(0.02, 0.98,
                    f"front owned by {', '.join(sorted(set(owners)))} — "
                    "rel_err is not\ncomparable across cases (hence the "
                    "rel_err_rel head)",
                    transform=ax.transAxes, va="top", ha="left",
                    fontsize=7.6, color=GREY, style="italic")

    axes[0].set_ylabel("accuracy_rel_err  (lower is better)")
    fig.suptitle("Accuracy against cost — every successful corpus mesh, "
                 "coloured by mesher", fontsize=12.5, weight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.94))
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
    fig, axes = plt.subplots(1, 2, figsize=(12.4, 5.0))
    for ax, (key, label) in zip(axes, metrics):
        all_levels = sorted({round(to_float(r["h_rel"]), 4) for r in usable
                             if math.isfinite(to_float(r[key]))})
        for index, order in enumerate(orders):
            sel = [r for r in usable
                   if math.isfinite(to_float(r["order"]))
                   and int(to_float(r["order"])) == order
                   and math.isfinite(to_float(r[key]))]
            if not sel:
                continue
            color = ORDER_COLORS.get(order, GREY)
            # nudge each order sideways: fidelity is a surface property, so
            # the orders land on top of each other without an offset.
            nudge = math.exp((index - (len(orders) - 1) / 2) * 0.02)
            hs = np.array([to_float(r["h_rel"]) for r in sel])
            vs = np.array([to_float(r[key]) for r in sel])
            ax.scatter(hs * nudge, vs, s=9, alpha=0.20, color=color,
                       edgecolors="none")
            levels = sorted({round(float(h), 4) for h in hs})
            med, q25, q75 = [], [], []
            for level in levels:
                bucket = vs[np.isclose(hs, level, rtol=1e-3)]
                med.append(float(np.median(bucket)))
                q25.append(float(np.percentile(bucket, 25)))
                q75.append(float(np.percentile(bucket, 75)))
                print(f"  {key} order {order} h_rel {level:g}: n={bucket.size} "
                      f"median {fmt(med[-1])} IQR [{fmt(q25[-1])}, {fmt(q75[-1])}]")
            ax.errorbar(np.array(levels) * nudge, med,
                        yerr=[np.array(med) - np.array(q25),
                              np.array(q75) - np.array(med)],
                        color=color, marker="o", markersize=6, linewidth=2.0,
                        linestyle="-" if index == 0 else "--",
                        capsize=4, label=f"order {order}  (n={len(sel)})")
            if len(levels) >= 2 and med[0] > 0:
                # levels is ascending, so med[0] is the finest mesh
                print(f"    order {order}: coarsest h_rel {levels[-1]:g} -> "
                      f"finest {levels[0]:g} improves {key} by "
                      f"{med[-1] / med[0]:.2f}x")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks(all_levels)
        ax.set_xticklabels([f"{level:g}" for level in all_levels])
        ax.minorticks_off()
        ax.set_xmargin(0.25)
        ax.autoscale_view()
        ax.invert_xaxis()  # finer meshes to the right: quality should rise
        ax.set_xlabel("h_rel  (target element size / diagonal)   —   finer →")
        ax.set_ylabel(f"{label}  (model units)")
        ax.set_title(f"{label} vs resolution", fontsize=10.5)
        ax.legend(frameon=False, fontsize=8.5)
        style_axes(ax)

    fig.suptitle("Geometric fidelity against mesh resolution, split by element order",
                 fontsize=12.5, weight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out_dir, "fidelity_vs_h.png")
    return True


# --- figure 5: mesh_before_after.png ----------------------------------------


def wire_path(campaigns_dir: Path, row: dict[str, str]) -> Path | None:
    path = (campaigns_dir / row["campaign"] / "runs" / row["cfg_id"]
            / row["part"] / "t0" / "wire.png")
    return path if path.is_file() else None


def crop_subject(path: Path, pad: int = 14, tol: int = 18,
                 target: int = 520) -> Any:
    """Trim the flat background off a warehouse render.

    The background is a single flat colour, so the subject box is just the
    bounding box of every pixel that differs from the corner colour. Nothing
    about the crop is hardcoded: a render with a different framing or canvas
    size trims correctly too.
    """
    image = Image.open(path).convert("RGB")
    pixels = np.asarray(image).astype(np.int16)
    height, width = pixels.shape[:2]
    corners = np.array([pixels[0, 0], pixels[0, -1], pixels[-1, 0], pixels[-1, -1]])
    background = np.median(corners, axis=0)
    mask = np.abs(pixels - background).sum(axis=2) > tol
    if mask.any():
        ys, xs = np.nonzero(mask)
        box = (max(0, int(xs.min()) - pad), max(0, int(ys.min()) - pad),
               min(width, int(xs.max()) + 1 + pad),
               min(height, int(ys.max()) + 1 + pad))
        image = image.crop(box)
    if max(image.size) > target:
        scale = target / max(image.size)
        image = image.resize((max(1, round(image.width * scale)),
                              max(1, round(image.height * scale))),
                             Image.LANCZOS)
    # the wireframe is near-black on navy; downsampling greys the lines out.
    image = ImageEnhance.Contrast(image).enhance(1.7)
    # a resampled wireframe carries tens of thousands of near-identical
    # colours, which is most of the PNG's weight. The subject is two colours
    # plus edge shading, so a small palette is visually lossless here.
    return image.quantize(colors=48, method=Image.MEDIANCUT).convert("RGB")


def _caption(row: dict[str, str]) -> str:
    return (f"h_rel {to_float(row['h_rel']):.3g}   order {row['order']}   "
            f"{row['mesher']}\n"
            f"n_dof {int(to_float(row['n_dof'])):,}   "
            f"rel_err {to_float(row['accuracy_rel_err']):.4g}")


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
        print(f"no data yet — no warehouse wire.png renders under "
              f"{campaigns_dir}/<campaign>/runs/<cfg_id>/<part>/t0/; "
              "skipping mesh_before_after.png")
        return False

    print(f"\nmesh_before_after.png — {len(picks)} parts, real warehouse renders")
    fig, axes = plt.subplots(len(picks), 2, squeeze=False, layout="constrained",
                             figsize=(10.6, 4.1 * len(picks)))
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
        for col_index, (title, row) in enumerate((("baseline (coarsest run)", coarse),
                                                  ("best accuracy", best))):
            ax = axes[row_index][col_index]
            path = wire_path(campaigns_dir, row)
            ax.set_xticks([])
            ax.set_yticks([])
            for side in ax.spines.values():
                side.set_color("#c9c5bd")
            if path is None:
                ax.text(0.5, 0.5, "render missing", ha="center", va="center",
                        transform=ax.transAxes, color=GREY)
                continue
            # source and axes are near 1:1, so nearest keeps both the crisp
            # wire lines and the small palette the PNG size depends on.
            ax.imshow(np.asarray(crop_subject(path)), interpolation="nearest")
            ax.set_title(f"{title}\n{_caption(row)}", fontsize=8.4,
                         linespacing=1.4)
        axes[row_index][0].set_ylabel(part, fontsize=10, weight="bold",
                                      labelpad=8)

    fig.suptitle("Meshes before and after — the coarsest run beside the "
                 "best-accuracy run for the same part",
                 fontsize=12, weight="bold")
    save(fig, out_dir, "mesh_before_after.png")
    return True


# --- entry point ------------------------------------------------------------


def main() -> int:
    args = parse_args()
    advisor_dir = args.advisor_dir
    out_dir = args.out_dir
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
    print(f"\n{written}/5 figures written to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
