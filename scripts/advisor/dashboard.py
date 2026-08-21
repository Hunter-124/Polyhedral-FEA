#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Regenerate the advisor training dashboard (bench/advisor/dashboard.html).

Self-contained: plotly.js is INLINED into the HTML. The bundle is downloaded
once and cached at bench/advisor/.cache/plotly.min.js; later runs reuse the
cache fully offline. If the cache is absent and the network is unavailable
the script fails with a clear message naming the cache path — it never emits
a CDN-dependent file.

Inputs (every one optional; a missing file degrades its panel to an explicit
"no data yet" note):

  <advisor-dir>/runs/history.jsonl          one JSON object per training run (C7)
  <advisor-dir>/runs/<NNN>/activations.json network activations per run (C8)
  <advisor-dir>/baseline_metrics.json       LightGBM baseline metrics (C7)
  <advisor-dir>/throughput.json             batch campaign throughput (C9)
  <advisor-dir>/weights.json                stage head weights (guardrails)

Run from anywhere:

    python scripts/advisor/dashboard.py
    python scripts/advisor/dashboard.py --runs-dir /tmp/empty --out /tmp/dash.html
"""
from __future__ import annotations

import argparse
import html
import json
import math
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

# The legend entries and the table's row names come from the one label table
# in figstyle, so a dashboard axis cannot disagree with the report figure or
# the doc that quotes it. This is the module's only matplotlib-backed import.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import figstyle as fs  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ADVISOR_DIR = ROOT / "bench" / "advisor"
# The cache location is fixed by contract, independent of --advisor-dir.
PLOTLY_CACHE = ADVISOR_DIR / ".cache" / "plotly.min.js"
PLOTLY_VERSION = "3.7.0"
PLOTLY_URLS = (
    f"https://cdn.jsdelivr.net/npm/plotly.js@{PLOTLY_VERSION}/dist/plotly.min.js",
    f"https://cdn.plot.ly/plotly-{PLOTLY_VERSION}.min.js",
)

# Input layer is subsampled down to this many neurons when D is larger; the
# fact is stated in the activation-view legend. Trunk is 64-wide by design.
MAX_INPUT_NEURONS = 32
MAX_LAYER_NEURONS = 64
# An edge is drawn only when |weight| is at or above this quantile of |w|
# within its layer pair. The numeric thresholds are printed in the legend.
EDGE_QUANTILE = 0.90

PLOT_CONFIG = {"responsive": True, "displaylogo": False}

METRIC_COLORS = {
    "rel_err_mae": "#2166ac",
    "geo_chamfer_mae": "#1b7837",
    "geo_p99_mae": "#5aa469",
    "dof_mae": "#762a83",
    "mesh_ms_mae": "#b35806",
    "solve_ms_mae": "#d73027",
    "failure_bce": "#c51b7d",
    "failure_acc": "#01665e",
    "failure_auc": "#35978f",
    "policy_mse": "#4d4d4d",
    "total_loss": "#333333",
}

BASE_LAYOUT = {
    "paper_bgcolor": "rgba(0,0,0,0)",
    "plot_bgcolor": "rgba(0,0,0,0)",
    "font": {"family": "system-ui, Segoe UI, Helvetica, Arial, sans-serif",
             "size": 12, "color": "#20242a"},
    "margin": {"l": 56, "r": 24, "t": 30, "b": 44},
    # Pinned to the bottom of the chart box, not to the plot area: now that
    # the x-axis title actually renders, a plot-area-relative legend lands on
    # top of it and on the tick labels when a chart is tall.
    "legend": {"orientation": "h", "yref": "container", "y": 0.0,
               "yanchor": "bottom"},
    # plotly.js 3.x dropped the bare-string form of an axis title: a string
    # here renders as nothing at all, which is how every axis on this page
    # ended up unlabelled. Titles are objects.
    "xaxis": {"title": {"text": "training run"}, "automargin": True,
              "gridcolor": "#e4e1da", "zerolinecolor": "#e4e1da"},
    "yaxis": {"gridcolor": "#e4e1da", "zerolinecolor": "#e4e1da"},
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--advisor-dir", type=Path, default=ADVISOR_DIR,
                        help="directory holding runs/, baseline_metrics.json, "
                             "throughput.json, weights.json (default: bench/advisor)")
    parser.add_argument("--runs-dir", type=Path, default=None,
                        help="override the runs directory (default: <advisor-dir>/runs)")
    parser.add_argument("--out", type=Path, default=None,
                        help="output HTML path (default: <advisor-dir>/dashboard.html)")
    return parser.parse_args()


def load_json_file(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def load_history(runs_dir: Path) -> tuple[list[dict[str, Any]], int]:
    """Return (run records sorted by run index, number of skipped bad lines)."""
    path = runs_dir / "history.jsonl"
    if not path.is_file():
        return [], 0
    records: list[dict[str, Any]] = []
    skipped = 0
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                skipped += 1
    records.sort(key=lambda rec: rec.get("run", 0))
    return records, skipped


def load_activations(runs_dir: Path) -> list[dict[str, Any]]:
    if not runs_dir.is_dir():
        return []
    acts: list[dict[str, Any]] = []
    for child in sorted(runs_dir.iterdir()):
        path = child / "activations.json"
        if child.is_dir() and path.is_file():
            with path.open("r", encoding="utf-8") as stream:
                acts.append(json.load(stream))
    acts.sort(key=lambda rec: rec.get("run", 0))
    return acts


def ensure_plotly() -> str:
    """Return the plotly.js source, using the on-disk cache when present."""
    if PLOTLY_CACHE.is_file():
        js = PLOTLY_CACHE.read_text(encoding="utf-8")
    else:
        js = None
        errors: list[str] = []
        for url in PLOTLY_URLS:
            try:
                with urllib.request.urlopen(url, timeout=60) as response:
                    js = response.read().decode("utf-8")
                break
            except OSError as exc:  # network down, DNS, TLS, ...
                errors.append(f"{url}: {exc}")
        if js is None:
            raise SystemExit(
                "plotly.js is not cached and could not be downloaded.\n"
                f"  expected cache: {PLOTLY_CACHE}\n"
                "  tried:\n    " + "\n    ".join(errors) + "\n"
                f"  fix: place plotly.min.js (v{PLOTLY_VERSION}) at the cache "
                "path above, or run again with network access."
            )
        PLOTLY_CACHE.parent.mkdir(parents=True, exist_ok=True)
        PLOTLY_CACHE.write_text(js, encoding="utf-8")
    if "</script" in js:
        raise SystemExit(
            f"cached plotly bundle at {PLOTLY_CACHE} contains '</script' and "
            "cannot be inlined safely; delete it and re-run to re-download."
        )
    return js


def js_json(value: Any) -> str:
    """json.dumps safe for embedding inside an inline <script> block."""
    return json.dumps(value, separators=(",", ":")).replace("</", "<\\/")


def chart(div_id: str, traces: list[dict[str, Any]],
          layout: dict[str, Any], height: int = 320) -> str:
    merged = {**BASE_LAYOUT, **layout, "height": height}
    return (
        f'<div class="chart" id="{div_id}"></div>'
        f"<script>Plotly.newPlot({js_json(div_id)},{js_json(traces)},"
        f"{js_json(merged)},{js_json(PLOT_CONFIG)});</script>"
    )


def no_data(expected: str) -> str:
    return (f'<p class="empty">no data yet — expected '
            f"<code>{html.escape(expected)}</code></p>")


def series(history: list[dict[str, Any]], section: str,
           key: str) -> tuple[list[Any], list[float]]:
    xs: list[Any] = []
    ys: list[float] = []
    for record in history:
        container = record if not section else (record.get(section) or {})
        value = container.get(key)
        if isinstance(value, (int, float)) and math.isfinite(value):
            xs.append(record.get("run"))
            ys.append(float(value))
    return xs, ys


def trace(history: list[dict[str, Any]], section: str, key: str,
          mode: str = "lines+markers", **extra: Any) -> dict[str, Any]:
    xs, ys = series(history, section, key)
    out = {"x": xs, "y": ys, "name": fs.metric_label(key), "mode": mode,
           "line": {"color": METRIC_COLORS.get(key, "#555555"), "width": 2},
           "marker": {"size": 5}}
    out.update(extra)
    return out


def stage_transitions(history: list[dict[str, Any]]) -> list[Any]:
    marks: list[Any] = []
    prev: Any = None
    for record in history:
        stage = record.get("stage")
        if record.get("stage_transition") or (prev == "A" and stage == "B"):
            marks.append(record.get("run"))
        prev = stage
    return marks


def stage_marker_layout(history: list[dict[str, Any]]) -> dict[str, Any]:
    shapes: list[dict[str, Any]] = []
    annotations: list[dict[str, Any]] = []
    for run in stage_transitions(history):
        shapes.append({"type": "line", "x0": run, "x1": run,
                       "yref": "paper", "y0": 0, "y1": 1,
                       "line": {"color": "#8a6d1c", "width": 1.5,
                                "dash": "dot"}})
        # Inside the plot area, not above it: at y = 1.0 the label sits in the
        # title band and prints on top of the chart title whenever the
        # transition lands near the middle of the run history.
        annotations.append({"x": run, "yref": "paper", "y": 1.0,
                            "text": "Stage A to B", "showarrow": False,
                            "yanchor": "top", "xanchor": "left",
                            "font": {"color": "#8a6d1c", "size": 11}})
    return {"shapes": shapes, "annotations": annotations}


def begin_end_annotations(history: list[dict[str, Any]]) -> list[dict[str, Any]]:
    xs, ys = series(history, "val", "rel_err_mae")
    if not xs:
        return []
    out = []
    for label, x, y in (("begin", xs[0], ys[0]), ("end", xs[-1], ys[-1])):
        out.append({"x": x, "y": y,
                    "text": f"{label} {y:.3f} ({fs.times_off(y)} off)",
                    "showarrow": True, "arrowhead": 2, "ax": 34, "ay": -30,
                    "font": {"size": 11, "color": "#2166ac"}})
    return out


def panel_val_metrics(history: list[dict[str, Any]]) -> str:
    if not history:
        return no_data("bench/advisor/runs/history.jsonl")
    keys = ["rel_err_mae", "geo_chamfer_mae", "geo_p99_mae", "dof_mae",
            "mesh_ms_mae", "solve_ms_mae", "failure_bce", "policy_mse"]
    traces = [trace(history, "val", key) for key in keys]
    marker = stage_marker_layout(history)
    annotations = marker["annotations"] + begin_end_annotations(history)
    layout = {"title": {"text": "How each prediction did on parts it never "
                                "trained on"},
              "yaxis": {"title": {"text": "score, lower is better<br>"
                                          f"misses are in {fs.DECADES_NOTE}",
                                  "font": {"size": 11}},
                        "automargin": True,
                        "gridcolor": "#e4e1da", "zerolinecolor": "#e4e1da"},
              "shapes": marker["shapes"], "annotations": annotations}
    return chart("val-metrics", traces, layout, height=440)


def panel_benchmark(history: list[dict[str, Any]]) -> str:
    if not history:
        return no_data("bench/advisor/runs/history.jsonl")
    accuracy = [
        trace(history, "val", "rel_err_mae"),
        trace(history, "val", "geo_chamfer_mae"),
        trace(history, "val", "geo_p99_mae"),
        trace(history, "val", "failure_bce"),
        trace(history, "val", "failure_acc", **dash_y2("failure_acc")),
        trace(history, "val", "failure_auc", **dash_y2("failure_auc")),
    ]
    acc_layout = {
        "title": {"text": "Accuracy, on parts it never trained on"},
        "yaxis": {"title": {"text": "average miss, or cross-entropy<br>"
                                    f"{fs.DECADES_NOTE}",
                            "font": {"size": 11}},
                  "automargin": True,
                  "gridcolor": "#e4e1da", "zerolinecolor": "#e4e1da"},
        "yaxis2": {"title": {"text": "accuracy and ranking quality (0 to 1)",
                             "font": {"size": 11}},
                   "automargin": True, "overlaying": "y",
                   "side": "right", "range": [0, 1], "showgrid": False},
        **stage_marker_layout(history),
    }
    cost = [
        trace(history, "val", "dof_mae"),
        trace(history, "val", "mesh_ms_mae"),
        trace(history, "val", "solve_ms_mae"),
    ]
    cost_layout = {"title": {"text": "Cost, on parts it never trained on"},
                   "yaxis": {"title": {"text": "average miss<br>"
                                               f"{fs.DECADES_NOTE}",
                                       "font": {"size": 11}},
                             "automargin": True,
                             "gridcolor": "#e4e1da",
                             "zerolinecolor": "#e4e1da"},
                   **stage_marker_layout(history)}
    return ('<div class="grid-2">'
            + chart("bench-accuracy", accuracy, acc_layout, height=430)
            + chart("bench-cost", cost, cost_layout, height=430)
            + "</div>")


def dash_y2(key: str) -> dict[str, Any]:
    return {"yaxis": "y2", "line": {"color": METRIC_COLORS.get(key, "#555555"),
                                    "width": 2, "dash": "dash"}}


def panel_pruning_throughput(history: list[dict[str, Any]],
                             throughput: dict[str, Any] | None) -> str:
    cells: list[str] = []
    if history:
        xs, dropped = series(history, "", "pruned_rows")
        _, cumulative = series(history, "", "pruned_total")
        traces = [
            {"x": xs, "y": dropped, "name": fs.quantity_label("pruned_rows"),
             "type": "bar", "marker": {"color": "#c51b7d"}},
            {"x": xs, "y": cumulative,
             "name": fs.quantity_label("pruned_total"),
             "mode": "lines+markers", "yaxis": "y2",
             "line": {"color": "#4d4d4d", "width": 2}},
        ]
        layout = {"title": {"text": "Rows dropped for poor fit, per training "
                                    "run"},
                  "barmode": "overlay",
                  "yaxis": {"title": {"text": "rows dropped this run"},
                            "automargin": True,
                            "gridcolor": "#e4e1da", "zerolinecolor": "#e4e1da"},
                  "yaxis2": {"title": {"text": "rows dropped so far"},
                             "automargin": True, "overlaying": "y",
                             "side": "right", "showgrid": False}}
        cells.append(chart("pruning", traces, layout, height=320))
    else:
        cells.append(no_data("bench/advisor/runs/history.jsonl (pruning log)"))

    batches = (throughput or {}).get("batches") if throughput else None
    if batches:
        labels = [f"batch {b.get('batch')}" for b in batches]
        rate = [b.get("rows_per_s") for b in batches]
        dedup = [b.get("dedup_hits") for b in batches]
        rate_traces = [
            {"x": labels, "y": rate, "name": "rows per second", "type": "bar",
             "marker": {"color": "#2166ac"}},
            {"x": labels, "y": dedup, "name": "duplicates skipped",
             "type": "bar",
             "marker": {"color": "#1b7837"}, "yaxis": "y2"},
        ]
        rate_layout = {"title": {"text": "How fast the data was generated, "
                                         "per batch"},
                       "barmode": "group",
                       "xaxis": {"title": {"text": ""}, "gridcolor": "#e4e1da",
                                 "zerolinecolor": "#e4e1da"},
                       "yaxis": {"title": {"text": "rows per second"},
                                 "automargin": True, "gridcolor": "#e4e1da",
                                 "zerolinecolor": "#e4e1da"},
                       "yaxis2": {"title": {"text": "duplicates skipped"},
                                  "automargin": True, "overlaying": "y",
                                  "side": "right", "showgrid": False}}
        shard_traces: list[dict[str, Any]] = []
        shard_ids = sorted({s.get("shard") for b in batches
                            for s in b.get("shards", [])})
        for shard in shard_ids:
            ys: list[Any] = []
            texts: list[str] = []
            for b in batches:
                match = next((s for s in b.get("shards", [])
                              if s.get("shard") == shard), None)
                ys.append(match.get("wall_s") if match else None)
                texts.append(f"{match.get('rows')} rows" if match else "")
            shard_traces.append({"x": labels, "y": ys, "type": "bar",
                                 "name": f"chunk {shard}", "text": texts})
        shard_layout = {"title": {"text": "Time each chunk of the batch took"},
                        "barmode": "group",
                        "xaxis": {"title": {"text": ""}, "gridcolor": "#e4e1da",
                                  "zerolinecolor": "#e4e1da"},
                        "yaxis": {"title": {"text": "time taken (seconds)"},
                                  "automargin": True, "gridcolor": "#e4e1da",
                                  "zerolinecolor": "#e4e1da"}}
        cells.append(chart("throughput-rate", rate_traces, rate_layout,
                           height=320))
        cells.append(chart("throughput-shards", shard_traces, shard_layout,
                           height=320))
    else:
        cells.append(no_data("bench/advisor/throughput.json"))
    return '<div class="grid-2">' + "".join(cells) + "</div>"


def panel_baseline(history: list[dict[str, Any]],
                   baseline: dict[str, Any] | None) -> str:
    if baseline is None and not history:
        return no_data("bench/advisor/baseline_metrics.json and "
                       "bench/advisor/runs/history.jsonl")
    targets = (baseline or {}).get("targets", {})
    latest_val = (history[-1].get("val") or {}) if history else {}
    latest_run = history[-1].get("run") if history else None
    head_key = {"rel_err": "rel_err_mae", "geo_chamfer": "geo_chamfer_mae",
                "geo_p99": "geo_p99_mae", "dof": "dof_mae",
                "mesh_ms": "mesh_ms_mae", "solve_ms": "solve_ms_mae"}
    rows: list[str] = []
    names = list(targets) if targets else [k for k in head_key
                                           if head_key[k] in latest_val]
    for name in names:
        base = targets.get(name, {})
        base_mae = base.get("val_mae")
        base_rmse = base.get("val_rmse")
        mlp_mae = latest_val.get(head_key.get(name, f"{name}_mae"))
        delta = (mlp_mae - base_mae) if isinstance(mlp_mae, (int, float)) \
            and isinstance(base_mae, (int, float)) else None
        winner = ""
        if delta is not None:
            winner = ('<span class="win">our model</span>' if delta < 0
                      else '<span class="lose">LightGBM</span>')
        # Plain name for the reader, the JSON key beside it: this table is read
        # against baseline_metrics.json, so dropping the key would cost a grep.
        quantity = fs.quantity_label(str(name))
        cell = html.escape(quantity)
        if quantity != str(name):
            cell += f" <code>{html.escape(str(name))}</code>"
        rows.append(
            "<tr>"
            f"<td>{cell}</td>"
            f"<td>{fmt(base_mae)}</td><td>{fmt(base_rmse)}</td>"
            f"<td>{fmt(mlp_mae)}</td><td>{fmt(delta, signed=True)}</td>"
            f"<td>{winner}</td></tr>"
        )
    note_bits = []
    if baseline:
        note_bits.append(f"LightGBM trained on {baseline.get('n_train')} rows "
                         f"and scored on {baseline.get('n_val')} it never saw")
    if latest_run is not None:
        note_bits.append(f"our model: latest run {latest_run}, misses in "
                         f"{fs.DECADES_NOTE}")
    if not baseline:
        note_bits.append("baseline_metrics.json not present yet — the "
                         "LightGBM columns fill in after "
                         "train.py --baseline")
    if not names:
        return no_data("baseline scores or our model's scores on unseen parts")
    return (
        f'<p class="note">{" · ".join(html.escape(b) for b in note_bits)}</p>'
        '<table class="compare"><thead><tr><th>what it predicts</th>'
        "<th>LightGBM: average miss</th>"
        "<th>LightGBM: average miss, RMS</th>"
        "<th>our model: average miss</th>"
        "<th>difference (ours − LightGBM)</th><th>smaller miss</th>"
        "</tr></thead><tbody>" + "".join(rows) + "</tbody></table>"
    )


def fmt(value: Any, signed: bool = False) -> str:
    if not isinstance(value, (int, float)) or not math.isfinite(value):
        return '<span class="na">n/a</span>'
    sign = "+" if signed and value > 0 else ""
    return f"{sign}{value:.4f}"


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


def guardrails_block(weights: dict[str, Any] | None) -> str:
    if weights is None:
        return ('<p class="empty">no data yet — expected '
                "<code>bench/advisor/weights.json</code></p>")

    def flatten(d: dict[str, Any], prefix: str = "") -> list[tuple[str, Any]]:
        rows: list[tuple[str, Any]] = []
        for key, value in d.items():
            if isinstance(value, dict):
                rows.extend(flatten(value, f"{prefix}{key}."))
            else:
                rows.append((f"{prefix}{key}", value))
        return rows

    rows = "".join(
        f"<tr><td>{html.escape(key)}</td>"
        f"<td>{html.escape(json.dumps(value) if isinstance(value, (list, dict)) else str(value))}</td></tr>"
        for key, value in flatten(weights)
    )
    return ('<table class="compare slim"><thead><tr><th>guardrail</th>'
            f"<th>value</th></tr></thead><tbody>{rows}</tbody></table>")


CSS = """
:root { color-scheme: light; }
body { margin: 0; background: #f6f5f1; color: #20242a;
       font-family: system-ui, "Segoe UI", Helvetica, Arial, sans-serif; }
main { max-width: 1180px; margin: 0 auto; padding: 28px 24px 64px; }
h1 { font-size: 1.55rem; margin: 0 0 4px; letter-spacing: -0.01em; }
h2 { font-size: 1.05rem; margin: 0 0 10px; color: #3c4048; font-weight: 650; }
.meta { color: #6b6f77; font-size: 0.85rem; margin-bottom: 20px; }
.meta code, .empty code { background: #eceae4; border-radius: 4px;
       padding: 1px 5px; font-size: 0.85em; }
.panel { background: #fdfcf9; border: 1px solid #e4e1da; border-radius: 10px;
         padding: 18px 20px; margin: 18px 0; }
.note { color: #6b6f77; font-size: 0.85rem; margin: 0 0 8px; }
.empty { color: #8a6d1c; background: #fbf6e7; border: 1px dashed #d9c98d;
         border-radius: 8px; padding: 12px 14px; font-size: 0.9rem; }
.grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 18px; }
@media (max-width: 960px) { .grid-2 { grid-template-columns: 1fr; } }
table.compare { border-collapse: collapse; width: 100%; font-size: 0.9rem; }
table.compare th { text-align: left; color: #6b6f77; font-weight: 600;
       border-bottom: 2px solid #e4e1da; padding: 6px 10px; }
table.compare td { border-bottom: 1px solid #efede7; padding: 6px 10px;
       font-variant-numeric: tabular-nums; }
table.compare.slim { max-width: 640px; }
.win { color: #1b7837; font-weight: 650; }
.lose { color: #b35806; font-weight: 650; }
.na { color: #9a9ea6; }
.act-controls { display: flex; align-items: center; gap: 14px; margin: 6px 0 10px; }
.act-controls input[type=range] { flex: 1; accent-color: #2166ac; }
#act-label { font-family: ui-monospace, Consolas, monospace; font-size: 0.9rem;
       background: #eceae4; border-radius: 5px; padding: 3px 9px; }
.act-run svg { width: 100%; height: auto; display: block; }
.layer-name { font: 600 12px system-ui, sans-serif; fill: #3c4048; }
.head-label { font: 10px ui-monospace, Consolas, monospace; fill: #6b6f77; }
.scale-note { font: 10px ui-monospace, Consolas, monospace; fill: #9a9ea6; }
.case { display: block; color: #6b6f77; font-size: 0.82rem; margin: 2px 0 6px; }
.legend { color: #6b6f77; font-size: 0.82rem; border-top: 1px solid #efede7;
       padding-top: 10px; margin-top: 10px; }
"""


def main() -> int:
    args = parse_args()
    advisor_dir = args.advisor_dir
    runs_dir = args.runs_dir or advisor_dir / "runs"
    out_path = args.out or advisor_dir / "dashboard.html"

    plotly_js = ensure_plotly()
    history, skipped = load_history(runs_dir)
    acts = load_activations(runs_dir)
    baseline = load_json_file(advisor_dir / "baseline_metrics.json")
    throughput = load_json_file(advisor_dir / "throughput.json")
    weights = load_json_file(advisor_dir / "weights.json")

    generated = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    warnings = ""
    if skipped:
        warnings = (f'<p class="empty">warning: skipped {skipped} malformed '
                    f"line(s) in {html.escape(str(runs_dir / 'history.jsonl'))}</p>")

    latest_stage = history[-1].get("stage") if history else None
    summary = (f"{len(history)} training run(s) on record"
               + (f" · latest stage {html.escape(str(latest_stage))}"
                  if latest_stage else "")
               + f" · {len(acts)} network snapshot(s)"
               + f" · plotly.js {PLOTLY_VERSION} inlined")

    document = f"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PolyMesh — learned mesh advisor dashboard</title>
<style>{CSS}</style>
<script>{plotly_js}</script>
</head><body><main>
<h1>Learned mesh advisor — training dashboard</h1>
<p class="meta">generated {generated} from
<code>{html.escape(str(advisor_dir))}</code> · {summary} · regenerable via
<code>python scripts/advisor/dashboard.py</code></p>
{warnings}
<section class="panel"><h2>Guardrails — how much each prediction counts</h2>
{guardrails_block(weights)}</section>
<section class="panel"><h2>1 · How each prediction is doing</h2>
<p class="note">Every prediction is scored with a penalty that goes easy on the
biggest misses (a Huber loss), and training runs in two stages: Stage A teaches
the accuracy, shape and failure predictions, and Stage B adds the cost ones.
The begin and end markers follow the main one, predicted relative error
(<code>rel_err</code>).</p>
{panel_val_metrics(history)}</section>
<section class="panel"><h2>2 · Scorecard — accuracy and cost on unseen parts</h2>
{panel_benchmark(history)}</section>
<section class="panel"><h2>3 · Rows dropped, and how fast the data was made</h2>
<p class="note">Each run drops the worst 5% of rows by how badly the model fits
them; rows that record a failure are never dropped. The speed charts show
duplicates skipped, rows per second, and how long each chunk of the batch took,
as reported by the campaign runner.</p>
{panel_pruning_throughput(history, throughput)}</section>
<section class="panel"><h2>4 · Our model against the LightGBM baseline</h2>
{panel_baseline(history, baseline)}</section>
<section class="panel"><h2>5 · Inside the network</h2>
{panel_activations(acts)}</section>
</main></body></html>
"""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(document, encoding="utf-8")
    print(f"wrote {out_path} ({len(document) / 1e6:.2f} MB, "
          f"plotly.js {PLOTLY_VERSION} inlined from {PLOTLY_CACHE})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
