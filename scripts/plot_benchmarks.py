#!/usr/bin/env python3
"""PolyMesh benchmark charts for the showcase gallery.

Every number plotted here is PARSED from a committed file. Nothing is typed in
by hand and nothing is estimated:

  bench_dof_time.png  <- bench/results/polymesh-d6-l-domain.json
  bench_tier1.png     <- bench/results/polymesh-gate1-p1.json
                         + bench/reports/p1-gate1-convergence.md (tolerances)
  bench_mms.png       <- bench/reports/p1-gate1-convergence.md (Tier-2 table)
                         + docs/progress.md (hierarchical p=1..4 rates)

Honesty rules baked into the titles/subtitles (non-negotiable, from the ADRs):
  * Speedup is against PolyMesh's OWN frozen uniform tet10 baseline. It is
    NEVER a comparison against CalculiX, Elmer or any external solver.
  * Tier-1 analytical accuracy was measured on structured parametric meshes,
    not on product Cartesian grid-fill meshes.

Usage:
    python3 scripts/plot_benchmarks.py [--outdir docs/assets/showcase]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

# ---------------------------------------------------------------------------
# Studio palette
# ---------------------------------------------------------------------------
CHROME_BG = "#0E1116"
PANEL_BG = "#161B22"
BORDER = "#2A3240"
TEXT = "#E6EAF0"
TEXT_DIM = "#8A93A3"
ACCENT = "#4CC2FF"
ACCENT_DIM = "#2A6E96"
OK = "#2DD4BF"
WARN = "#F5C542"
ERR = "#F5876C"
NEUTRAL = "#8B95A5"

DPI = 170

REPO = Path(__file__).resolve().parent.parent
D6_JSON = REPO / "bench/results/polymesh-d6-l-domain.json"
GATE1_JSON = REPO / "bench/results/polymesh-gate1-p1.json"
GATE1_MD = REPO / "bench/reports/p1-gate1-convergence.md"
PROGRESS_MD = REPO / "docs/progress.md"


def rel(path: Path) -> str:
    """Repo-relative path string, for footers."""
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)


def apply_style() -> None:
    plt.rcParams.update(
        {
            "font.family": "sans-serif",
            "font.sans-serif": ["Liberation Sans", "DejaVu Sans", "Arial"],
            "figure.facecolor": CHROME_BG,
            "savefig.facecolor": CHROME_BG,
            "axes.facecolor": PANEL_BG,
            "axes.edgecolor": BORDER,
            "axes.labelcolor": TEXT,
            "axes.titlecolor": TEXT,
            "text.color": TEXT,
            "xtick.color": TEXT_DIM,
            "ytick.color": TEXT_DIM,
            "grid.color": BORDER,
            "axes.grid": True,
            "grid.linewidth": 0.8,
            "axes.axisbelow": True,
            "axes.linewidth": 1.0,
            "figure.dpi": DPI,
            "savefig.dpi": DPI,
            "legend.facecolor": PANEL_BG,
            "legend.edgecolor": BORDER,
            "legend.labelcolor": TEXT,
        }
    )


def footer(fig, text: str) -> None:
    fig.text(0.008, 0.013, text, color=TEXT_DIM, fontsize=9.5, ha="left", va="bottom")


def finish(fig, out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, facecolor=CHROME_BG)
    plt.close(fig)
    px = int(fig.get_size_inches()[0] * DPI)
    print(f"wrote {rel(out)}  ({px}px wide, {DPI} dpi)")


# ---------------------------------------------------------------------------
# Parsers -- fail loudly rather than invent
# ---------------------------------------------------------------------------
def load_json(path: Path) -> list[dict]:
    if not path.is_file():
        raise SystemExit(f"missing benchmark data file: {rel(path)}")
    return json.loads(path.read_text())


def d6_records() -> dict[str, dict]:
    """Index the D6 L-domain records by case_id (first occurrence wins)."""
    out: dict[str, dict] = {}
    for rec in load_json(D6_JSON):
        out.setdefault(rec["case_id"], rec)
    ratios = {}
    for rec in load_json(D6_JSON):
        name = rec.get("accuracy", {}).get("name", "")
        if name.startswith(("dof_ratio", "time_ratio")):
            ratios[name] = rec["accuracy"]["value"]
    out["_ratios"] = ratios
    for need in ("l-domain-d6-baseline", "l-domain-d6-graded"):
        if need not in out:
            raise SystemExit(f"{rel(D6_JSON)}: case_id '{need}' not found")
    return out


def gate1_records() -> dict[tuple[str, str], dict]:
    return {
        (r["case_id"], r["accuracy"]["name"]): r
        for r in load_json(GATE1_JSON)
        if "accuracy" in r
    }


def parse_tier1_table() -> list[dict]:
    """Tier-1 rows from the GATE-1 report that carry a % error and a ≤ tolerance."""
    text = GATE1_MD.read_text()
    rows: list[dict] = []
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 4:
            continue
        case, metric, result, tol = cells
        pcts = re.findall(r"\*\*([\d.]+)\s*%\*\*", result)
        tol_m = re.search(r"[≤<=]+\s*([\d.]+)\s*%", tol)
        if not pcts or not tol_m:
            continue
        # For "3.056 vs 3 (**1.87%**)" the percentage is the last bold value.
        rows.append(
            {
                "case": case,
                "metric": metric,
                "err_pct": float(pcts[-1]),
                "tol_pct": float(tol_m.group(1)),
                "raw": result,
            }
        )
    if not rows:
        raise SystemExit(f"{rel(GATE1_MD)}: no Tier-1 %-error rows parsed")
    return rows


def parse_mms_elements() -> list[dict]:
    """Tier-2 MMS table: element, theory order, observed order."""
    known = {"tet4", "hex8", "tet10", "hex20"}
    rows: list[dict] = []
    for line in GATE1_MD.read_text().splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 4 or cells[0] not in known:
            continue
        obs = re.search(r"\*\*([\d.]+)\*\*", cells[2])
        if not obs or not cells[1].isdigit():
            continue
        rows.append(
            {
                "label": cells[0],
                "theory": float(cells[1]),
                "observed": float(obs.group(1)),
                # Keep the report's own digits so the chart never implies
                # more (or less) precision than the source file states.
                "observed_text": obs.group(1),
            }
        )
    if not rows:
        raise SystemExit(f"{rel(GATE1_MD)}: no Tier-2 MMS rows parsed")
    return rows


def parse_mms_hierarchical() -> list[dict]:
    """Hierarchical p-basis MMS energy rates from docs/progress.md."""
    text = PROGRESS_MD.read_text()
    m = re.search(r"MMS energy rates p=1\.\.(\d+):\s*\*\*([^*]+)\*\*", text)
    if not m:
        raise SystemExit(f"{rel(PROGRESS_MD)}: 'MMS energy rates p=1..N' line not found")
    pmax = int(m.group(1))
    tokens = re.findall(r"[\d.]+", m.group(2))
    vals = [float(v) for v in tokens]
    if len(vals) != pmax:
        raise SystemExit(
            f"{rel(PROGRESS_MD)}: expected {pmax} MMS rates, parsed {len(vals)}"
        )
    return [
        {
            "label": f"p={i + 1}",
            "theory": float(i + 1),
            "observed": v,
            "observed_text": tokens[i],
        }
        for i, v in enumerate(vals)
    ]


# ---------------------------------------------------------------------------
# (a) bench_dof_time.png
# ---------------------------------------------------------------------------
def plot_dof_time(outdir: Path) -> Path:
    recs = d6_records()
    base = recs["l-domain-d6-baseline"]
    grad = recs["l-domain-d6-graded"]
    ratios = recs["_ratios"]

    dof_ratio = ratios.get(
        "dof_ratio_uniform_over_graded", base["dofs"] / grad["dofs"]
    )
    time_ratio = ratios.get(
        "time_ratio_uniform_over_graded",
        base["wall_time_s"]["total"] / grad["wall_time_s"]["total"],
    )

    panels = [
        {
            "title": "Degrees of freedom",
            "unit": "DOF",
            "vals": [base["dofs"], grad["dofs"]],
            "fmt": lambda v: f"{int(v):,}",
            "ratio": f"{dof_ratio:.2f}\u00d7 fewer DOF",
        },
        {
            "title": "Wall time (mesh + solve)",
            "unit": "seconds",
            "vals": [base["wall_time_s"]["total"], grad["wall_time_s"]["total"]],
            "fmt": lambda v: f"{v:.3f} s",
            "ratio": f"{time_ratio:.1f}\u00d7 faster",
        },
        {
            "title": "Energy deficit (accuracy held)",
            "unit": "percent",
            "vals": [base["accuracy"]["value"], grad["accuracy"]["value"]],
            "fmt": lambda v: f"{v:.4f} %",
            "ratio": "same accuracy band",
        },
    ]

    fig, axes = plt.subplots(1, 3, figsize=(13.0, 5.9))
    names = ["uniform tet10\n(frozen baseline)", "feature-graded\ntet10"]
    colors = [NEUTRAL, ACCENT]

    for ax, panel in zip(axes, panels):
        bars = ax.bar(
            names, panel["vals"], width=0.58, color=colors,
            edgecolor=BORDER, linewidth=1.0, zorder=3,
        )
        ax.set_title(panel["title"], fontsize=13.5, pad=14, fontweight="bold")
        ax.set_ylabel(panel["unit"], fontsize=11)
        ax.set_ylim(0, max(panel["vals"]) * 1.28)
        ax.grid(axis="x", visible=False)
        ax.tick_params(axis="x", labelsize=10.5, colors=TEXT)
        for bar, val in zip(bars, panel["vals"]):
            ax.annotate(
                panel["fmt"](val),
                (bar.get_x() + bar.get_width() / 2, val),
                xytext=(0, 7), textcoords="offset points",
                ha="center", va="bottom", fontsize=12,
                fontweight="bold", color=TEXT,
            )
        ax.annotate(
            panel["ratio"],
            (0.5, 0.955), xycoords="axes fraction",
            ha="center", va="top", fontsize=12, fontweight="bold", color=ACCENT,
            bbox=dict(boxstyle="round,pad=0.42", facecolor=CHROME_BG,
                      edgecolor=ACCENT_DIM, linewidth=1.2),
        )

    fig.suptitle(
        "D6 \u00b7 L-domain: feature-graded vs PolyMesh's own frozen uniform tet10 baseline",
        fontsize=17, fontweight="bold", color=TEXT, y=0.975,
    )
    fig.text(
        0.5, 0.912,
        "Same solver, same element type, same problem \u2014 only the sizing field differs. "
        "This is an internal self-comparison, not a comparison against any external solver.",
        ha="center", va="top", fontsize=11.5, color=TEXT_DIM,
    )
    fig.tight_layout(rect=(0, 0.045, 1, 0.895))
    footer(
        fig,
        f"source: {rel(D6_JSON)} \u2014 cases 'l-domain-d6-baseline' vs "
        f"'l-domain-d6-graded' (label {base['label']}, {base['timestamp']})",
    )
    out = outdir / "bench_dof_time.png"
    finish(fig, out)
    return out


# ---------------------------------------------------------------------------
# (b) bench_tier1.png
# ---------------------------------------------------------------------------
def plot_tier1(outdir: Path) -> Path:
    rows = parse_tier1_table()
    json_recs = gate1_records()

    # Cross-check the md-parsed values against the results JSON where present.
    checked = 0
    for (case_id, metric_name), rec in json_recs.items():
        val = rec["accuracy"]["value"]
        if any(abs(r["err_pct"] - val) < 1e-9 for r in rows):
            checked += 1
        else:
            print(
                f"  note: {case_id}/{metric_name}={val} present in "
                f"{rel(GATE1_JSON)} but not matched in the report table",
                file=sys.stderr,
            )

    # Best margin first (drawn top-down), worst last.
    rows = sorted(rows, key=lambda r: r["err_pct"] / r["tol_pct"], reverse=True)
    labels = [f"{r['case']}\n{r['metric']}" for r in rows]
    err = np.array([r["err_pct"] for r in rows])
    tol = np.array([r["tol_pct"] for r in rows])
    y = np.arange(len(rows))

    fig, ax = plt.subplots(figsize=(13.0, 6.9))

    # Tolerance track: the full allowance, drawn behind the measured bar.
    ax.barh(y, tol, height=0.62, color=CHROME_BG, edgecolor=BORDER,
            linewidth=1.1, zorder=2, label="tolerance allowance")
    # Measured error. One colour: the "% of budget" annotation carries the
    # margin, so a per-bar colour scale would only desync from the legend.
    ax.barh(y, err, height=0.62, color=ACCENT, edgecolor="none",
            zorder=3, label="measured relative error")
    # Tolerance limit marker.
    for yi, t in zip(y, tol):
        ax.plot([t, t], [yi - 0.36, yi + 0.36], color=WARN, lw=2.2,
                ls=(0, (3, 2)), zorder=4)

    for yi, e, t in zip(y, err, tol):
        ax.annotate(
            f"{e:g}%   of \u2264{t:g}%   ({100 * e / t:.0f}% of budget)",
            (t, yi), xytext=(10, 0), textcoords="offset points",
            va="center", ha="left", fontsize=11.5, color=TEXT,
        )

    ax.set_yticks(y, labels, fontsize=11, color=TEXT)
    ax.set_xlabel("relative error vs closed-form analytical solution (%)", fontsize=12)
    # Headroom for the longest right-hand annotation ("… (59% of budget)").
    ax.set_xlim(0, tol.max() * 1.38)
    ax.grid(axis="y", visible=False)
    ax.set_title(
        "Tier-1 analytical verification \u2014 every case inside tolerance",
        fontsize=17, fontweight="bold", pad=42, loc="left",
    )
    ax.annotate(
        "Measured on structured parametric verification meshes (hex20 sectors, "
        "annuli, octants) \u2014 not on product Cartesian grid-fill meshes.",
        (0, 1.045), xycoords="axes fraction", fontsize=11.5, color=TEXT_DIM,
        ha="left", va="bottom",
    )
    handles, lab = ax.get_legend_handles_labels()
    # Upper right is the only region no bar or annotation reaches.
    ax.legend(handles, lab, loc="upper right", fontsize=10.5, framealpha=1.0)

    fig.tight_layout(rect=(0, 0.05, 1, 1))
    footer(
        fig,
        f"sources: {rel(GATE1_JSON)} ({checked}/{len(json_recs)} records "
        f"cross-checked) \u00b7 tolerances from {rel(GATE1_MD)} (Tier-1 table)",
    )
    out = outdir / "bench_tier1.png"
    finish(fig, out)
    return out


# ---------------------------------------------------------------------------
# (c) bench_mms.png
# ---------------------------------------------------------------------------
def plot_mms(outdir: Path) -> Path:
    groups = [
        {
            "title": "Frozen P1 isoparametric elements",
            "sub": "uniform h-halving n=4\u21928, cubic manufactured field",
            "rows": parse_mms_elements(),
            "src": rel(GATE1_MD),
        },
        {
            "title": "Hierarchical p-basis (integrated Legendre)",
            "sub": "integrated-Legendre hierarchical basis, orders p=1..4",
            "rows": parse_mms_hierarchical(),
            "src": rel(PROGRESS_MD),
        },
    ]

    widths = [len(g["rows"]) for g in groups]
    fig, axes = plt.subplots(
        1, 2, figsize=(13.0, 6.5), gridspec_kw={"width_ratios": widths}
    )

    for ax, g in zip(axes, groups):
        rows = g["rows"]
        x = np.arange(len(rows))
        theory = [r["theory"] for r in rows]
        obs = [r["observed"] for r in rows]
        obs_text = [r["observed_text"] for r in rows]
        w = 0.36

        ax.bar(x - w / 2, theory, width=w, color=NEUTRAL, edgecolor=BORDER,
               linewidth=1.0, zorder=3, label="theory order p")
        ax.bar(x + w / 2, obs, width=w, color=ACCENT, edgecolor=BORDER,
               linewidth=1.0, zorder=3, label="measured order")

        for xi, t, o, otext in zip(x, theory, obs, obs_text):
            ax.annotate(f"{t:g}", (xi - w / 2, t), xytext=(0, 6),
                        textcoords="offset points", ha="center", va="bottom",
                        fontsize=11, color=TEXT_DIM)
            ax.annotate(otext, (xi + w / 2, o), xytext=(0, 6),
                        textcoords="offset points", ha="center", va="bottom",
                        fontsize=11.5, color=ACCENT, fontweight="bold")

        ax.set_xticks(x, [r["label"] for r in rows], fontsize=12, color=TEXT)
        ax.set_ylim(0, max(max(theory), max(obs)) * 1.26)
        ax.set_ylabel("energy-norm convergence order", fontsize=11.5)
        ax.grid(axis="x", visible=False)
        ax.set_title(g["title"], fontsize=13.5, fontweight="bold", pad=26)
        ax.annotate(g["sub"], (0, 1.012), xycoords="axes fraction",
                    fontsize=10.5, color=TEXT_DIM, ha="left", va="bottom")
        ax.legend(loc="upper left", fontsize=10, framealpha=1.0)

    fig.suptitle(
        "Method-of-manufactured-solutions convergence \u2014 measured order matches theory",
        fontsize=17, fontweight="bold", color=TEXT, y=0.975,
    )
    fig.tight_layout(rect=(0, 0.048, 1, 0.925))
    footer(
        fig,
        "sources: "
        + " \u00b7 ".join(
            f"{g['src']} ({g['title'].lower()})" for g in groups
        ),
    )
    out = outdir / "bench_mms.png"
    finish(fig, out)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render PolyMesh benchmark charts.")
    ap.add_argument(
        "--outdir", type=Path, default=REPO / "docs/assets/showcase",
        help="output directory for the PNGs",
    )
    ap.add_argument(
        "--only", action="append", default=[],
        help="render only these charts (dof_time|tier1|mms); repeatable",
    )
    args = ap.parse_args(argv)

    apply_style()
    args.outdir.mkdir(parents=True, exist_ok=True)

    charts = {"dof_time": plot_dof_time, "tier1": plot_tier1, "mms": plot_mms}
    wanted = args.only or list(charts)
    unknown = [w for w in wanted if w not in charts]
    if unknown:
        raise SystemExit(f"unknown chart(s): {', '.join(unknown)}")
    for key in wanted:
        charts[key](args.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
