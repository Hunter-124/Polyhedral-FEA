# SPDX-License-Identifier: BSD-3-Clause
"""Network-snapshot panel of the advisor training dashboard (``dashboard.py``).

Renders each run's ``activations.json`` as an SVG layer graph behind a run slider.
"""
from __future__ import annotations

import html
import math
from typing import Any

import figstyle as fs

from .dashboard_charts import js_json, no_data

# Input layer is subsampled down to this many neurons when D is larger; the
# fact is stated in the activation-view legend. Trunk is 64-wide by design.
MAX_INPUT_NEURONS = 32
MAX_LAYER_NEURONS = 64
# An edge is drawn only when |weight| is at or above this quantile of |w|
# within its layer pair. The numeric thresholds are printed in the legend.
EDGE_QUANTILE = 0.90


def subsample_indices(size: int, cap: int) -> list[int]:
    if size <= cap:
        return list(range(size))
    if cap == 1:
        return [size // 2]
    return sorted({round(i * (size - 1) / (cap - 1)) for i in range(cap)})


def render_activation_svg(act: dict[str, Any]) -> tuple[str, list[str]]:
    """Render one run's layer graph as SVG; return (svg, legend notes)."""
    layers = act.get("layers", [])
    edges = act.get("edges", [])
    notes: list[str] = []

    kept: list[list[int]] = []
    for layer in layers:
        cap = MAX_INPUT_NEURONS if layer.get("name") == "input" \
            else MAX_LAYER_NEURONS
        idx = subsample_indices(int(layer.get("size", 0)), cap)
        kept.append(idx)
        if len(idx) < int(layer.get("size", 0)):
            notes.append(f"{layer.get('name')}: showing {len(idx)} of "
                         f"{layer.get('size')} neurons (evenly spaced)")

    layer_index = {layer.get("name"): pos for pos, layer in enumerate(layers)}

    # Global activation magnitude for this run (normalisation stated below).
    amax = 0.0
    for layer, idx in zip(layers, kept):
        values = layer.get("values", [])
        amax = max(amax, *(abs(float(values[i])) for i in idx
                           if i < len(values)), 0.0)

    # Layout geometry.
    col_x = [90 + i * 260 for i in range(len(layers))]
    spacing = 14.0
    top = 46.0
    max_nodes = max((len(idx) for idx in kept), default=1)
    height = top + max_nodes * spacing + 26
    # Right-hand room for the head labels, which are spelled out rather than
    # printed as head identifiers: "relative error, against the case's own
    # median" needs about twice what "rel_err_rel" did.
    width = col_x[-1] + 330 if col_x else 400

    def node_pos(col: int, row: int, count: int) -> tuple[float, float]:
        col_height = count * spacing
        y0 = top + (max_nodes * spacing - col_height) / 2 + spacing / 2
        return col_x[col], y0 + row * spacing

    parts: list[str] = []
    edge_notes: list[str] = []
    for edge in edges:
        src = layer_index.get(edge.get("from"))
        dst = layer_index.get(edge.get("to"))
        if src is None or dst is None:
            continue
        weights = edge.get("weights", [])
        src_kept, dst_kept = kept[src], kept[dst]
        flat = [abs(float(weights[j][i])) for j in dst_kept if j < len(weights)
                for i in src_kept if i < len(weights[j])]
        if not flat:
            continue
        wmax = max(flat)
        ordered = sorted(flat)
        threshold = ordered[min(len(ordered) - 1,
                                int(EDGE_QUANTILE * (len(ordered) - 1)))]
        edge_notes.append(f"{edge.get('from')}→{edge.get('to')}: "
                          f"strength {threshold:.3f} and up")
        if wmax <= 0:
            continue
        drawn = 0
        for dj, j in enumerate(dst_kept):
            if j >= len(weights):
                continue
            for si, i in enumerate(src_kept):
                if i >= len(weights[j]):
                    continue
                w = float(weights[j][i])
                mag = abs(w)
                if mag < threshold or mag <= 0:
                    continue
                x1, y1 = node_pos(src, si, len(src_kept))
                x2, y2 = node_pos(dst, dj, len(dst_kept))
                frac = mag / wmax
                color = "#2166ac" if w > 0 else "#b35806"
                parts.append(
                    f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" '
                    f'y2="{y2:.1f}" stroke="{color}" '
                    f'stroke-opacity="{0.16 + 0.44 * frac:.3f}" '
                    f'stroke-width="{0.4 + 1.8 * frac:.2f}"/>'
                )
                drawn += 1
        if drawn == 0:
            edge_notes[-1] += " (nothing that strong)"

    last_col = len(layers) - 1
    for col, (layer, idx) in enumerate(zip(layers, kept)):
        name = html.escape(str(layer.get("name")))
        size = int(layer.get("size", 0))
        caption = f"{name} · {size}" + (" (sample shown)" if len(idx) < size
                                        else "")
        parts.append(f'<text x="{col_x[col]}" y="{top - 26:.0f}" '
                     f'text-anchor="middle" class="layer-name">{caption}'
                     "</text>")
        values = layer.get("values", [])
        labels = layer.get("labels") or []
        # Permanent side labels only on the heads layer; other layers carry
        # their labels in the hover tooltip to keep the graph legible.
        side_labels = labels if col == last_col else []
        for row, i in enumerate(idx):
            x, y = node_pos(col, row, len(idx))
            a = float(values[i]) if i < len(values) else 0.0
            frac = (abs(a) / amax) if amax > 0 else 0.0
            color = "#2166ac" if a >= 0 else "#b35806"
            radius = 2.5 + 6.5 * math.sqrt(frac)
            label_txt = f" · {labels[i]}" if i < len(labels) else ""
            parts.append(
                f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{radius:.2f}" '
                f'fill="{color}" fill-opacity="{0.22 + 0.78 * frac:.3f}">'
                f"<title>{name}[{i}]{html.escape(label_txt)} = "
                f"{a:.4f}</title></circle>"
            )
            # Index by i, the neuron this circle actually is, not by row, its
            # slot in the drawn column. They agree only while the layer fits
            # under the subsample cap; past it the side label named a
            # different output than the tooltip on the same circle.
            if i < len(side_labels):
                parts.append(
                    f'<text x="{x + 13:.1f}" y="{y + 3.5:.1f}" '
                    "class=\"head-label\">"
                    f"{html.escape(fs.quantity_label(str(labels[i])))}"
                    "</text>"
                )
    parts.append(
        f'<text x="{width - 8}" y="{height - 8:.0f}" text-anchor="end" '
        f'class="scale-note">'
        f"strongest signal this run = {amax:.3f}</text>"
    )
    svg = (f'<svg viewBox="0 0 {width:.0f} {height:.0f}" role="img" '
           f'aria-label="network signals for run {act.get("run")}" '
           'xmlns="http://www.w3.org/2000/svg">' + "".join(parts) + "</svg>")
    notes.append("a line is drawn only where a connection is stronger than "
                 f"{int(EDGE_QUANTILE * 100)} percent of the connections "
                 "between those two layers: "
                 + "; ".join(edge_notes))
    return svg, notes


def panel_activations(acts: list[dict[str, Any]]) -> str:
    if not acts:
        return no_data("bench/advisor/runs/<NNN>/activations.json")
    runs = [act.get("run") for act in acts]
    panes: list[str] = []
    all_notes: list[str] = []
    for act in acts:
        svg, notes = render_activation_svg(act)
        all_notes = notes  # thresholds are per-run; show the latest run's
        case = act.get("input_case") or {}
        caption = ""
        if case:
            caption = (f'<span class="case">example part fed in: '
                       f"{html.escape(str(case.get('part', '?')))} · "
                       f"{html.escape(str(case.get('cfg_id', '?')))}</span>")
        panes.append(f'<div class="act-run" data-run="{act.get("run")}" '
                     f'style="display:none">{caption}{svg}</div>')
    legend = (
        '<p class="legend">Each circle is one neuron — the bigger the circle '
        "and the stronger the color, the bigger the signal; blue is positive, "
        "orange is negative, measured against the strongest signal in that "
        "run (printed at the bottom right). "
        + " \u00b7 ".join(html.escape(n) for n in all_notes)
        + ". Lines are connections: blue adds to the next neuron, orange "
          "subtracts from it.</p>"
    )
    slider = (
        '<div class="act-controls">'
        f'<input type="range" id="act-slider" min="0" max="{len(runs) - 1}" '
        f'step="1" value="{len(runs) - 1}" '
        'aria-label="move through the training runs">'
        f'<span id="act-label">run {runs[-1]}</span></div>'
    )
    script = (
        "<script>(function(){"
        f"var runs={js_json(runs)};"
        "var panes=document.querySelectorAll('.act-run');"
        "var label=document.getElementById('act-label');"
        "function show(i){panes.forEach(function(p,j){"
        "p.style.display=(j===i)?'block':'none';});"
        "label.textContent='run '+runs[i];}"
        "document.getElementById('act-slider').addEventListener('input',"
        "function(e){show(+e.target.value);});"
        f"show({len(runs) - 1});"
        "})();</script>"
    )
    return slider + "".join(panes) + legend + script
