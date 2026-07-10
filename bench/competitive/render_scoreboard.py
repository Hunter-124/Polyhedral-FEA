#!/usr/bin/env python3
# PolyMesh competitive scoreboard renderer — stdlib only.
# Reads bench/results/*.json, writes docs/bench/scoreboard.md
from __future__ import annotations

import json
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
RESULTS_DIR = ROOT / "bench" / "results"
OUT_PATH = ROOT / "docs" / "bench" / "scoreboard.md"
SCHEMA_HINT = "bench/competitive/schema.json"


def load_results(path: Path) -> list[dict[str, Any]]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(raw, list):
        items = raw
    elif isinstance(raw, dict) and "results" in raw and isinstance(raw["results"], list):
        items = raw["results"]
    elif isinstance(raw, dict):
        items = [raw]
    else:
        raise ValueError(f"{path}: expected object, array, or {{results: [...]}}")
    out: list[dict[str, Any]] = []
    for i, item in enumerate(items):
        if not isinstance(item, dict):
            raise ValueError(f"{path}[{i}]: not an object")
        for key in (
            "solver",
            "version",
            "case_id",
            "dofs",
            "wall_time_s",
            "accuracy",
            "label",
            "timestamp",
        ):
            if key not in item:
                raise ValueError(f"{path}[{i}]: missing required field {key!r}")
        out.append(item)
    return out


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


def svg_sparkline(values: list[float | None], label: str) -> str:
    """Minimal inline SVG polyline for accuracy trend (lower often better)."""
    pts = [(i, v) for i, v in enumerate(values) if v is not None]
    if len(pts) < 2:
        return ""
    w, h, pad = 180, 36, 3
    ys = [p[1] for p in pts]
    lo, hi = min(ys), max(ys)
    span = hi - lo if hi > lo else 1.0
    xs = [p[0] for p in pts]
    x0, x1 = min(xs), max(xs)
    xspan = x1 - x0 if x1 > x0 else 1.0

    def xy(i: int, v: float) -> tuple[float, float]:
        x = pad + (i - x0) / xspan * (w - 2 * pad)
        y = pad + (1.0 - (v - lo) / span) * (h - 2 * pad)
        return x, y

    poly = " ".join(f"{xy(i, v)[0]:.1f},{xy(i, v)[1]:.1f}" for i, v in pts)
    safe = label.replace("&", "&amp;").replace("<", "&lt;").replace('"', "&quot;")
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'role="img" aria-label="{safe}">'
        f'<polyline fill="none" stroke="#4a9" stroke-width="1.5" points="{poly}"/>'
        f"</svg>"
    )


def main() -> int:
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
        lines.append(
            "ASCII sparkline scales within each case/metric series (height ∝ value). "
            "SVG polyline when ≥2 numeric points. Lower is better for `*_err_*` metrics."
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
            lines.append(f"### `{case_id}` — `{metric}`")
            lines.append("")
            lines.append(f"- labels: {' → '.join(f'`{x}`' for x in labels)}")
            lines.append(f"- solvers: {', '.join(solvers)}")
            lines.append(f"- values: {', '.join(fmt_num(v) for v in vals)}")
            lines.append(f"- sparkline: `{sparkline(vals)}`")
            svg = svg_sparkline(vals, f"{case_id} {metric}")
            if svg:
                lines.append("")
                lines.append(svg)
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
