# SPDX-License-Identifier: BSD-3-Clause
"""``network_layout.png``: the trained architecture, read out of the checkpoint."""
from __future__ import annotations

import textwrap
from pathlib import Path
from typing import Any

import numpy as np
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

import figstyle as fs

from .report_common import load_json, save, tint

#: input-column groups drawn in network_layout.png, in trunk-input order. The
#: fills are lightened series colours: the grouping is categorical, so it
#: reuses the one categorical palette rather than inventing pastels.
GROUP_SERIES = {
    "part features": "hybrid_zoo",
    "case context": "graded_tet",
    "continuous action": "hex",
    "categorical action": "hybrid_vem",
}

#: The same groups in the words a reader sees on the diagram. The keys above
#: are the schema names dataset.py uses and stay exactly as they are; only
#: these strings are drawn.
GROUP_TITLES = {
    "part features": "shape of the part",
    "case context": "what is being solved",
    "continuous action": "mesh settings (numbers)",
    "categorical action": "mesh settings (choices)",
    "other": "everything else",
    "input columns": "what the network is given",
}


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
        from .dataset import (  # noqa: PLC0415
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
        from .dataset import OUTPUT_NAMES  # noqa: PLC0415
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
        "AdvisorNet — the network exactly as it was saved after training",
        subtitle=(f"Training run {shape['run']}. It holds "
                  f"{shape['n_parameters']:,} numbers it learned, a shared "
                  f"middle section {hidden} values wide, and {len(heads)} "
                  f"separate predictions."),
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
            f"{GROUP_TITLES.get(label, label)}\n{len(members)} numbers\n{sample}",
            colors.get(label, tint(fs.series("hybrid_zoo").color)), fontsize=8.2)
        group_ports.append((label, (right, mid), members))
        y -= gap

    ax.text(0.1125, top + 0.045,
            f"the {len(shape['input_columns'])} numbers it is given",
            ha="center", va="center", fontsize=fs.FONT_PT["panel"],
            weight="bold")
    ax.text(0.1125, top + 0.016,
            "rescaled to a common range in C++ (normalization.json)",
            ha="center", va="center", fontsize=fs.FONT_PT["footer"],
            color=fs.theme().muted, style="italic")

    # column 1 -- embedding tables for the two categorical columns
    emb_ports: list[tuple[float, float]] = []
    emb_y = bottom + 0.005
    for name, slots in (("mesher_idx", shape["mesher_slots"]),
                        ("order_idx", shape["order_slots"])):
        right, mid = _box(
            ax, 0.265, emb_y, 0.135, 0.075,
            f"{slots} choices → {emb_dim} numbers\n{name}",
            tint(fs.series("hybrid_vem").color), fontsize=8.2)
        emb_ports.append((right, mid))
        emb_y += 0.095
    ax.text(0.3325, emb_y + 0.012,
            "the choice is rounded to a valid slot\nthen looked up inside the model",
            ha="center", va="bottom", fontsize=fs.FONT_PT["footer"],
            color=fs.theme().muted, style="italic")

    # column 2 -- concatenation into the trunk input
    concat_x, concat_w = 0.455, 0.10
    concat_y, concat_h = bottom + 0.06, span - 0.12
    _box(ax, concat_x, concat_y, concat_w, concat_h,
         f"joined together\n\n{shape['trunk_inputs']} numbers in all\n\n"
         f"{n_continuous} plain numbers\n+ {emb_dim} for the order\n"
         f"+ {emb_dim} for the mesher",
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
         f"mix to {hidden} values\n(Linear + GELU)\n\n"
         f"{shape['fc1_parameters']:,} learned\nnumbers",
         tint(fs.series("hybrid_zoo").color, 0.62), fontsize=9,
         weight="bold")
    _box(ax, trunk_x, fc2_y, trunk_w, trunk_h,
         f"mix to {hidden} values\n(Linear + GELU)\n\n"
         f"{shape['fc2_parameters']:,} learned\nnumbers",
         tint(fs.series("hybrid_zoo").color, 0.62), fontsize=9,
         weight="bold")
    _arrow(ax, concat_right, (trunk_x, fc1_y + trunk_h / 2), width=1.4)
    _arrow(ax, (trunk_x + trunk_w / 2, fc1_y),
           (trunk_x + trunk_w / 2, fc2_y + trunk_h), width=1.4)
    ax.text(trunk_x + trunk_w / 2, top + 0.02,
            f"shared middle section\n{hidden} values wide", ha="center",
            va="center", fontsize=fs.FONT_PT["label"], weight="bold")

    # column 4 -- heads. The column is wide enough, and the label wrapped, so
    # that the longest plain-English head name still lands inside its box.
    head_x, head_w = 0.775, 0.205
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
             "\n".join(textwrap.wrap(fs.quantity_label(name), 36))
             + f"\n{width} output{'' if width == 1 else 's'} · "
               f"{params:,} learned numbers",
             face, fontsize=8.0)
        _arrow(ax, trunk_out, (head_x, y + head_h / 2), width=0.8)
        y -= head_gap
    ax.text(head_x + head_w / 2, top + 0.045,
            "what it predicts", ha="center", va="center",
            fontsize=fs.FONT_PT["panel"], weight="bold")
    ax.text(head_x + head_w / 2, top + 0.016,
            "in the order the model returns them",
            ha="center", va="center", fontsize=fs.FONT_PT["footer"],
            color=fs.theme().muted, style="italic")

    if drift:
        fig.text(0.5, 0.028,
                 "the saved network is older than the list of predictions "
                 f"dataset.py now asks for ({', '.join(contract or [])})",
                 ha="center", fontsize=fs.FONT_PT["annot"],
                 color=fs.theme().bad, style="italic")
    save(fig, out_dir, "network_layout.png")
    return True
