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
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from advisor.dashboard_charts import (  # noqa: E402
    panel_baseline,
    panel_benchmark,
    panel_pruning_throughput,
    panel_val_metrics,
)
from advisor.dashboard_network import panel_activations  # noqa: E402
from advisor.paths import ADVISOR_DIR  # noqa: E402

# The cache location is fixed by contract, independent of --advisor-dir.
PLOTLY_CACHE = ADVISOR_DIR / ".cache" / "plotly.min.js"
PLOTLY_VERSION = "3.7.0"
PLOTLY_URLS = (
    f"https://cdn.jsdelivr.net/npm/plotly.js@{PLOTLY_VERSION}/dist/plotly.min.js",
    f"https://cdn.plot.ly/plotly-{PLOTLY_VERSION}.min.js",
)


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
