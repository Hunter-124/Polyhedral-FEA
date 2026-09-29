# SPDX-License-Identifier: BSD-3-Clause
"""Plotly panels of the advisor training dashboard (``dashboard.py``).

Every chart is a ``<div>`` plus an inline ``Plotly.newPlot`` call; the page that
embeds them inlines plotly.js itself. Legend entries and table row names come
from the one label table in figstyle, so a dashboard axis cannot disagree with
the report figure or the doc that quotes it.
"""
from __future__ import annotations

import html
import json
import math
from typing import Any

import figstyle as fs

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
