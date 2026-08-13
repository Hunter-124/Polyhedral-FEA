#!/usr/bin/env python3
# PolyMesh competitive scoreboard renderer.
# Reads bench/results/*.json, writes docs/bench/scoreboard.md and its chart.
# All visual choices come from scripts/figstyle.py -- no local style here.
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import figstyle as fs  # noqa: E402


ROOT = Path(__file__).resolve().parents[2]
RESULTS_DIR = ROOT / "bench" / "results"
OUT_PATH = ROOT / "docs" / "bench" / "scoreboard.md"
CHART_PATH = ROOT / "docs" / "bench" / "scoreboard.png"
SCHEMA_HINT = "bench/competitive/schema.json"

ROW_REQUIRED_FIELDS = (
    "solver",
    "version",
    "case_id",
    "dofs",
    "wall_time_s",
    "accuracy",
    "label",
    "timestamp",
)


def skip_non_row(path: Path, reason: str) -> list[dict[str, Any]]:
    print(f"skip {path.relative_to(ROOT)}: {reason}")
    return []


def load_results(path: Path) -> list[dict[str, Any]]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(raw, dict) and "schema" in raw:
        return skip_non_row(path, f"non-row document declares schema {raw['schema']!r}")
    if isinstance(raw, list):
        items = raw
    elif isinstance(raw, dict) and "results" in raw and isinstance(raw["results"], list):
        items = raw["results"]
    elif isinstance(raw, dict):
        items = [raw]
    else:
        return skip_non_row(path, "non-row document is not an object or array of rows")

    for index, item in enumerate(items):
        if not isinstance(item, dict):
            return skip_non_row(path, f"non-row document has non-object entry {index}")
        missing = [key for key in ROW_REQUIRED_FIELDS if key not in item]
        if missing:
            return skip_non_row(path, f"non-row document entry {index} lacks {', '.join(missing)}")
    return items


def fmt_num(v: Any, digits: int = 4) -> str:
    if v is None:
        return "—"
    if isinstance(v, bool):
        return str(v)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if abs(v) >= 100 or (abs(v) >= 1 and abs(v - round(v)) < 1e-9):
            return f"{v:.4g}"
        return f"{v:.{digits}g}"
    return str(v)


def sparkline(values: list[float | None], width: int = 24) -> str:
    """Markdown-friendly ASCII sparkline; skips None."""
    nums = [v for v in values if v is not None]
    if not nums:
        return "(no data)"
    lo, hi = min(nums), max(nums)
    glyphs = "▁▂▃▄▅▆▇█"
    if hi <= lo:
        return glyphs[0] * min(width, max(1, len(values)))
    parts: list[str] = []
    for v in values:
        if v is None:
            parts.append("·")
            continue
        t = (v - lo) / (hi - lo)
        parts.append(glyphs[min(len(glyphs) - 1, int(t * (len(glyphs) - 1) + 1e-9))])
    return "".join(parts)


def render_chart(
    by_series: dict[tuple[str, str], list[dict[str, Any]]],
    results: list[dict[str, Any]],
    files: list[Path],
) -> Path | None:
    """Accuracy trend and cost profile, drawn entirely through figstyle."""
    if not by_series:
        return None

    fs.use("light")

    # Panel A -- one column per (case, metric); points keyed by solver so a
    # single legend covers both panels and nothing is colour-only.
    #
    # A row without a numeric accuracy is NOT interchangeable with a missing
    # measurement. Since the peer matrix started recording refusals as
    # first-class outcomes they are the plurality of rows, and lumping them
    # into one "excluded" count reads as data loss when a refusal is the
    # mesher correctly declining a size it cannot represent. Count them by the
    # status they carry and say so on the figure.
    columns: list[tuple[str, list[tuple[str, float]]]] = []
    outcomes: Counter[str] = Counter()
    for case_id, metric in sorted(by_series.keys()):
        rows = sorted(
            by_series[(case_id, metric)],
            key=lambda r: (r.get("timestamp") or "", r.get("label") or ""),
        )
        points: list[tuple[str, float]] = []
        for r in rows:
            v = (r.get("accuracy") or {}).get("value")
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                points.append((str(r.get("solver", "?")), float(v)))
            else:
                outcomes[str(r.get("status") or "no accuracy value")] += 1
        if points:
            columns.append((f"{case_id} · {metric}", points))
    missing = sum(outcomes.values())
    if not columns:
        return None

    # Panel B -- cost profile per solver, from the same rows.
    cost: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for r in results:
        dofs = r.get("dofs")
        total = (r.get("wall_time_s") or {}).get("total")
        if isinstance(dofs, (int, float)) and isinstance(total, (int, float)):
            if dofs > 0 and total > 0:
                cost[str(r.get("solver", "?"))].append((float(dofs), float(total)))

    solvers = sorted({s for _, pts in columns for s, _ in pts} | set(cost))
    fs.assert_glyphs(*solvers, *(name for name, _ in columns))

    composition = ", ".join(f"{count} {status}"
                            for status, count in outcomes.most_common())
    subtitle = ("Accuracy per case/metric and wall-clock cost per solver; "
                "colour, marker and dash all encode the solver.")
    if composition:
        subtitle += (f"\n{missing} of {len(results)} rows carry no accuracy "
                     f"value: {composition}.")
        if outcomes.get("refused"):
            subtitle += (" A refusal is the mesher declining an h it cannot "
                         "resolve and naming a finer one — an outcome, not a "
                         "missing measurement.")
    footer = fs.footer_source(RESULTS_DIR, n=len(results),
                              note=f"schema {SCHEMA_HINT}")
    fig, axes = fs.figure(
        "Competitive benchmark scoreboard",
        subtitle=subtitle,
        footer=footer,
        size="hero",
        ncols=2,
        share_y_axis="left panel is an accuracy metric, right panel is seconds",
    )
    ax_acc, ax_cost = axes[0][0], axes[0][1]

    flat = [v for _, pts in columns for _, v in pts]
    info = fs.loglim(ax_acc, flat, axis="x")
    for i, (_, pts) in enumerate(columns):
        by_solver: dict[str, list[float]] = defaultdict(list)
        for solver, v in pts:
            by_solver[solver].append(v)
        ax_acc.axhline(i, color=fs.theme().grid, linewidth=0.6, zorder=0)
        for solver, vals in by_solver.items():
            st = fs.series(solver)
            x = fs.clamp_to_floor(vals, info.floor)
            spread = 0.34 * (len(vals) > 1)
            y = [i - spread / 2 + spread * k / max(1, len(vals) - 1)
                 for k in range(len(vals))]
            ax_acc.scatter(x, y, **st.scatter(s=24))
    fs.panel_title(ax_acc, "Accuracy metric by case")
    ax_acc.set_xlabel("accuracy metric value, own unit per metric "
                      "(lower is better for *_err_*)")
    ax_acc.set_ylim(len(columns) - 0.4, -0.6)
    ax_acc.set_yticks(range(len(columns)))
    ax_acc.set_yticklabels([name for name, _ in columns],
                           fontsize=fs.FONT_PT["tick"] - 1.5)
    ax_acc.grid(axis="y", visible=False)
    # Composition lives in the subtitle: at 25 case rows this panel has no
    # empty corner big enough to hold three lines without covering data.
    fs.annotate_n(ax_acc, len(flat), excluded=missing,
                  what="numeric accuracy points", extra=info.note(),
                  loc="upper left")

    if cost:
        xs = [p[0] for pts in cost.values() for p in pts]
        ys = [p[1] for pts in cost.values() for p in pts]
        cost_info = fs.loglim(ax_cost, ys, axis="y")
        fs.loglim(ax_cost, xs, axis="x")
        for solver in sorted(cost):
            st = fs.series(solver)
            pts = sorted(cost[solver])
            ax_cost.plot([p[0] for p in pts],
                         fs.clamp_to_floor([p[1] for p in pts], cost_info.floor),
                         **st.line(linewidth=1.6))
        fs.annotate_n(ax_cost, len(ys), what="timed runs",
                      extra=cost_info.note(), loc="lower right")
    else:
        fs.annotate_n(ax_cost, 0, what="timed runs", loc="lower right")
    fs.panel_title(ax_cost, "Total wall time vs DOFs")
    ax_cost.set_xlabel("degrees of freedom")
    ax_cost.set_ylabel("total wall time (s)")
    ax_cost.legend(handles=fs.series_handles(solvers), loc="upper left",
                   frameon=False, fontsize=fs.FONT_PT["legend"],
                   title="solver (both panels)",
                   title_fontsize=fs.FONT_PT["legend"])

    return fs.finish(fig, CHART_PATH)


def main(argv: list[str] | None = None) -> int:
    argparse.ArgumentParser(
        prog="render_scoreboard.py",
        description="Render docs/bench/scoreboard.md and its chart from "
                    "bench/results/*.json.",
    ).parse_args(argv)

    if not RESULTS_DIR.is_dir():
        print(f"error: missing {RESULTS_DIR}", file=sys.stderr)
        return 1

    files = sorted(RESULTS_DIR.glob("*.json"))
    results: list[dict[str, Any]] = []
    for f in files:
        results.extend(load_results(f))

    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines: list[str] = [
        "# Benchmark scoreboard",
        "",
        f"_Generated {now} from `bench/results/*.json` "
        f"via `bench/competitive/render_scoreboard.py`. Schema: `{SCHEMA_HINT}`._",
        "",
        "Primary DOF-reduction baseline is PolyMesh's frozen P1 uniform path "
        "(ADR-0005). Peer solvers are audit cross-checks.",
        "",
        "## All runs",
        "",
        "| Solver | Version | Case | DOFs | mesh s | solve s | total s | Accuracy | Value | Label | Timestamp |",
        "|---|---|---|---:|---:|---:|---:|---|---:|---|---|",
    ]

    def sort_key(r: dict[str, Any]) -> tuple:
        return (r.get("timestamp") or "", r.get("solver") or "", r.get("case_id") or "")

    for r in sorted(results, key=sort_key):
        wt = r.get("wall_time_s") or {}
        acc = r.get("accuracy") or {}
        lines.append(
            "| {solver} | {version} | {case} | {dofs} | {mesh} | {solve} | {total} | {aname} | {aval} | `{label}` | {ts} |".format(
                solver=r.get("solver", ""),
                version=r.get("version", ""),
                case=r.get("case_id", ""),
                dofs=fmt_num(r.get("dofs")),
                mesh=fmt_num(wt.get("mesh")),
                solve=fmt_num(wt.get("solve")),
                total=fmt_num(wt.get("total")),
                aname=acc.get("name", ""),
                aval=fmt_num(acc.get("value")),
                label=r.get("label", ""),
                ts=r.get("timestamp", ""),
            )
        )

    if not results:
        lines.append("| — | — | — | — | — | — | — | — | — | — | — |")
        lines.extend(["", "_No result files yet._", ""])
    else:
        lines.append("")

    # Accuracy vs label, grouped by (case_id, metric name).
    by_series: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for r in results:
        metric = str((r.get("accuracy") or {}).get("name") or "?")
        by_series[(str(r["case_id"]), metric)].append(r)

    lines.extend(["## Accuracy vs labeled commits", ""])
    if not by_series:
        lines.append("_No cases to chart._")
    else:
        chart = render_chart(by_series, results, files)
        if chart is not None:
            lines.append(
                f"![Accuracy per labeled run and wall-clock cost per solver]"
                f"({chart.name})"
            )
            lines.append("")
        lines.append(
            "ASCII sparkline scales within each case/metric series (height ∝ value). "
            "Lower is better for `*_err_*` metrics."
        )
        lines.append("")
        for case_id, metric in sorted(by_series.keys()):
            rows = sorted(
                by_series[(case_id, metric)],
                key=lambda r: (r.get("timestamp") or "", r.get("label") or ""),
            )
            labels = [str(r.get("label", "")) for r in rows]
            vals: list[float | None] = []
            for r in rows:
                v = (r.get("accuracy") or {}).get("value")
                vals.append(float(v) if isinstance(v, (int, float)) else None)
            solvers = [str(r.get("solver", "")) for r in rows]
            fs.assert_glyphs(case_id, metric, *labels, *solvers)
            lines.append(f"### `{case_id}` — `{metric}`")
            lines.append("")
            lines.append(f"- labels: {' → '.join(f'`{x}`' for x in labels)}")
            lines.append(f"- solvers: {', '.join(solvers)}")
            lines.append(f"- values: {', '.join(fmt_num(v) for v in vals)}")
            lines.append(f"- sparkline: `{sparkline(vals)}`")
            lines.append("")

    lines.extend(
        [
            "## How to refresh",
            "",
            "```sh",
            "python3 bench/competitive/render_scoreboard.py",
            "```",
            "",
            "See [bench/competitive/README.md](../../bench/competitive/README.md).",
            "",
        ]
    )

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT_PATH} ({len(results)} result(s) from {len(files)} file(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
