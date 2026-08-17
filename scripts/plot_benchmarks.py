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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import figstyle as fs  # noqa: E402
import numpy as np  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
D6_JSON = REPO / "bench/results/polymesh-d6-l-domain.json"
GATE1_JSON = REPO / "bench/results/polymesh-gate1-p1.json"
GATE1_MD = REPO / "bench/reports/p1-gate1-convergence.md"
PROGRESS_MD = REPO / "docs/progress.md"
ADVISOR_SWEEP_JSON = REPO / "bench/results/advisor-budget-sweep.json"


def rel(path: Path) -> str:
    """Repo-relative path string, for footers."""
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)



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


def repeat_note(records: list[dict]) -> str:
    """State the sampling honestly: parsed repeat count, or 'single run'.

    The D6 result schema carries one wall-time sample per case. If a future
    harness starts emitting repeat counts, they are picked up here rather than
    being implied by error bars that the data cannot support.
    """
    keys = ("repeats", "n_repeats", "samples", "n_samples", "runs")
    counts = {
        int(rec[k])
        for rec in records
        for k in keys
        if isinstance(rec.get(k), (int, float))
    }
    if not counts:
        return "single run, no repeats — the source records one timing sample per case"
    if len(counts) == 1:
        n = counts.pop()
        return f"{n} repeat{'' if n == 1 else 's'} per case, as recorded in the source"
    return f"repeats per case vary in the source: {sorted(counts)}"


# ---------------------------------------------------------------------------
# (a) bench_dof_time.png
# ---------------------------------------------------------------------------
def plot_dof_time(outdir: Path) -> Path:
    recs = d6_records()
    base = recs["l-domain-d6-baseline"]
    grad = recs["l-domain-d6-graded"]
    ratios = recs["_ratios"]
    raw = load_json(D6_JSON)

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
        },
        {
            "title": "Wall time (mesh + solve)",
            "unit": "seconds",
            "vals": [base["wall_time_s"]["total"], grad["wall_time_s"]["total"]],
            "fmt": lambda v: f"{v:.3f} s",
        },
        {
            "title": "Energy deficit (accuracy held)",
            "unit": "percent",
            "vals": [base["accuracy"]["value"], grad["accuracy"]["value"]],
            "fmt": lambda v: f"{v:.4f} %",
        },
    ]

    t = fs.theme()
    base_st = fs.series("baseline", "uniform tet10 (frozen baseline)")
    grad_st = fs.series("graded_tet", "feature-graded tet10")
    note = repeat_note(raw)
    # Ratios are stated once, in the subtitle, computed from the parsed values.
    subtitle = (
        f"{dof_ratio:.2f}\u00d7 fewer DOF, {time_ratio:.1f}\u00d7 faster, same energy "
        "band. Internal self-comparison, never an external solver. "
        + note.split(" \u2014 ")[0].capitalize() + "."
    )
    footer = fs.footer_source(
        D6_JSON, n=len(raw),
        note=f"baseline vs graded cases, label {base['label']} \u00b7 {note}",
    )
    fs.assert_glyphs(subtitle, footer, base_st.label, grad_st.label)

    fig, axes = fs.figure(
        "D6 \u00b7 L-domain: feature-graded vs PolyMesh's own frozen uniform tet10 baseline",
        subtitle=subtitle, footer=footer, size="hero", nrows=1, ncols=3,
        share_y_axis="the three panels measure different quantities "
                     "(DOF, seconds, percent); a shared y-axis is meaningless here",
    )

    names = ["uniform tet10\n(frozen baseline)", "feature-graded\ntet10"]
    for ax, panel in zip(axes[0], panels):
        bars = ax.bar(
            names, panel["vals"], width=0.58,
            color=[base_st.color, grad_st.color],
            hatch=["//", ""], edgecolor=t.ink, linewidth=0.9, zorder=3,
        )
        fs.panel_title(ax, panel["title"])
        ax.set_ylabel(panel["unit"])
        ax.set_ylim(0, max(panel["vals"]) * 1.22)
        ax.grid(axis="x", visible=False)
        for bar, val in zip(bars, panel["vals"]):
            text = panel["fmt"](val)
            fs.assert_glyphs(text)
            ax.annotate(
                text, (bar.get_x() + bar.get_width() / 2, val),
                xytext=(0, 5), textcoords="offset points",
                ha="center", va="bottom", fontsize=fs.FONT_PT["annot"],
                color=t.ink,
            )
    # labelpad clears the two-line category ticks ("uniform tet10 / (frozen
    # baseline)"), which the default pad ran into.
    axes[0][1].set_xlabel(
        "same solver, same element type, same problem \u2014 only the sizing "
        "field differs",
        labelpad=14,
    )

    out = outdir / "bench_dof_time.png"
    return fs.finish(fig, out)


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

    # Widest margin first (drawn top-down), tightest last.
    rows = sorted(rows, key=lambda r: r["err_pct"] / r["tol_pct"])
    labels = [f"{r['case']}\n{r['metric']}" for r in rows]
    err = [r["err_pct"] for r in rows]
    tol = [r["tol_pct"] for r in rows]

    # The verdict is computed from the parsed data, never asserted in a string.
    outside = [r for r in rows if r["err_pct"] > r["tol_pct"]]
    n = len(rows)
    if outside:
        # The failing cases are named on the y-axis; the title states the count
        # so it stays readable however many cases fail.
        verdict = f"{len(outside)} of {n} cases OUTSIDE tolerance"
    else:
        verdict = f"all {n} cases inside tolerance"
    title = f"Tier-1 analytical verification \u2014 {verdict}"
    subtitle = (
        "Measured on structured parametric verification meshes (hex20 sectors, "
        "annuli, octants) \u2014 not on product Cartesian grid-fill meshes."
    )
    footer = fs.footer_source(
        GATE1_JSON, GATE1_MD, n=n,
        note=f"{checked}/{len(json_recs)} JSON records cross-checked",
    )
    fs.assert_glyphs(title, subtitle, footer, *labels)

    fig, axes = fs.figure(title, subtitle=subtitle, footer=footer, size="full")
    ax = axes[0][0]
    fs.ratio_bars(ax, labels, err, tol, unit="%")
    ax.set_xlabel("measured error / tolerance (1.0 = at the limit)")

    out = outdir / "bench_tier1.png"
    return fs.finish(fig, out)


# ---------------------------------------------------------------------------
# (c) bench_mms.png
# ---------------------------------------------------------------------------
def plot_mms(outdir: Path) -> Path:
    groups = [
        {
            "title": "Frozen P1 isoparametric elements",
            "sub": "uniform h-halving n=4\u21928, cubic manufactured field",
            "rows": parse_mms_elements(),
            "src": GATE1_MD,
        },
        {
            "title": "Hierarchical p-basis (integrated Legendre)",
            "sub": "integrated-Legendre hierarchical basis, orders p=1..4",
            "rows": parse_mms_hierarchical(),
            "src": PROGRESS_MD,
        },
    ]

    all_rows = [r for g in groups for r in g["rows"]]
    # The verdict is computed: the worst measured order relative to theory.
    worst = min(r["observed"] / r["theory"] for r in all_rows)
    shortfall = max(0.0, 1.0 - worst) * 100.0
    if shortfall <= 5.0:
        verdict = (f"every measured order within {shortfall:.1f}% of theory "
                   f"({len(all_rows)} cases)")
    else:
        verdict = (f"worst measured order {shortfall:.0f}% below theory "
                   f"({len(all_rows)} cases)")
    title = f"MMS convergence \u2014 {verdict}"
    footer = fs.footer_source(
        *[g["src"] for g in groups], n=len(all_rows),
        note="theory order and measured order both read from the committed reports",
    )
    subtitle = (
        "Energy-norm convergence order. Grey hatched bars are the theoretical "
        "order, solid bars the measured order."
    )
    fs.assert_glyphs(title, subtitle, footer,
                     *[r["label"] for r in all_rows],
                     *[r["observed_text"] for r in all_rows])

    t = fs.theme()
    theory_st = fs.series("theory", "theory order p")
    meas_st = fs.series("measured", "measured order")
    fig, axes = fs.figure(
        title, subtitle=subtitle, footer=footer, size="full", nrows=1, ncols=2,
        share_y_axis="both panels plot convergence order but over different order "
                     "ranges (isoparametric p=1..2 vs hierarchical p=1..4); a shared "
                     "scale would compress the element panel to a third of its height",
        gridspec_kw={"width_ratios": [len(g["rows"]) for g in groups]},
    )

    for ax, g in zip(axes[0], groups):
        rows = g["rows"]
        x = np.arange(len(rows))
        theory = [r["theory"] for r in rows]
        obs = [r["observed"] for r in rows]
        w = 0.36

        ax.bar(x - w / 2, theory, width=w, color=theory_st.color, hatch="//",
               edgecolor=t.ink, linewidth=0.9, zorder=3, label=theory_st.label)
        ax.bar(x + w / 2, obs, width=w, color=meas_st.color, hatch="",
               edgecolor=t.ink, linewidth=0.9, zorder=3, label=meas_st.label)

        for xi, th, ob, otext in zip(x, theory, obs, [r["observed_text"] for r in rows]):
            ax.annotate(f"{th:g}", (xi - w / 2, th), xytext=(0, 4),
                        textcoords="offset points", ha="center", va="bottom",
                        fontsize=fs.FONT_PT["annot"] - 0.5, color=t.muted)
            ax.annotate(otext, (xi + w / 2, ob), xytext=(0, 4),
                        textcoords="offset points", ha="center", va="bottom",
                        fontsize=fs.FONT_PT["annot"], color=t.ink)

        ax.set_xticks(x, [r["label"] for r in rows])
        ax.set_ylim(0, max(max(theory), max(obs)) * 1.22)
        ax.set_ylabel("energy-norm convergence order")
        ax.grid(axis="x", visible=False)
        fs.panel_title(ax, g["title"])
        ax.set_xlabel(g["sub"])
        if ax is axes[0][0]:
            ax.legend(loc="upper left", fontsize=fs.FONT_PT["legend"],
                      framealpha=1.0)

    out = outdir / "bench_mms.png"
    return fs.finish(fig, out)


# ---------------------------------------------------------------------------
# (d) bench_advisor_budget.png
# ---------------------------------------------------------------------------
def plot_advisor_budget(outdir: Path) -> Path:
    """What the learned advisor picks as the DOF cap tightens (ADR-0034).

    Every point is a real CLI run recorded in the sweep JSON: the chosen
    action's predicted per-case relative error (rel_err_rel — the ranking
    score the chooser optimizes) against the budget it was given. Runs the
    advisor refused (every candidate over budget) are drawn as refusal
    markers, never silently dropped.
    """
    if not ADVISOR_SWEEP_JSON.is_file():
        raise SystemExit(
            f"missing {rel(ADVISOR_SWEEP_JSON)} — run "
            "scripts/sweep_advisor_budget.py first")
    payload = json.loads(ADVISOR_SWEEP_JSON.read_text())
    records = payload["records"]
    if not records:
        raise SystemExit(f"{rel(ADVISOR_SWEEP_JSON)}: no records")

    parts = sorted({r["part"] for r in records})
    budgets = sorted({r["max_dof"] for r in records if r["max_dof"] > 0})
    # x positions: log-spaced budgets, with the uncapped run one step past
    # the loosest budget so it sits on the same axis.
    x_of_budget = {b: i for i, b in enumerate(budgets)}
    x_uncapped = len(budgets)

    t = fs.theme()
    n_runs = len(records)
    n_refusals = sum(1 for r in records if r["budget_refusal"])
    over = [
        r for r in records
        if not r["vetoed"] and r["max_dof"] > 0
        and isinstance(r["predicted_dof"], (int, float))
        and r["predicted_dof"] > r["max_dof"]
    ]
    # The verdict is computed from the records, not asserted in a string.
    if over:
        verdict = f"{len(over)} of {n_runs} capped runs picked OVER budget"
    else:
        verdict = (f"every capped pick inside its DOF budget "
                   f"({n_runs} runs, {n_refusals} honest refusal"
                   f"{'s' if n_refusals != 1 else ''})")
    title = f"Learned mesh advisor under a DOF budget — {verdict}"
    subtitle = (
        "Per-case relative-error score (lower is better) of the action the "
        "advisor picks at each cap. Labels name the action where it changes; "
        "a refusal means no candidate action fit the budget."
    )
    footer = fs.footer_source(
        ADVISOR_SWEEP_JSON, n=n_runs,
        note="one CLI solve per point (polymesh solve --advisor "
             "--advisor-max-dof N); predictions by the shipped ONNX model",
    )
    fs.assert_glyphs(title, subtitle, footer, *parts)

    fig, axes = fs.figure(
        title, subtitle=subtitle, footer=footer, size="hero", nrows=1,
        ncols=len(parts),
    )

    for ax, part in zip(axes[0], parts):
        # Uncapped (max_dof == 0) sorts last so the polyline walks the ladder
        # left-to-right and ends at "no cap".
        runs = sorted((r for r in records if r["part"] == part),
                      key=lambda r: (r["max_dof"] == 0, r["max_dof"]))
        xs, ys = [], []
        last_action = None
        for r in runs:
            x = x_uncapped if r["max_dof"] == 0 else x_of_budget[r["max_dof"]]
            if r["budget_refusal"]:
                # Honest marker below the data band: the advisor declined
                # rather than return an over-budget action.
                ax.scatter([x], [0.02], marker="x", s=90, color=t.bad,
                           linewidth=2.2, zorder=5,
                           transform=ax.get_xaxis_transform())
                ax.annotate("refused", (x, 0.06), xycoords=("data", "axes fraction"),
                            ha="center", fontsize=fs.FONT_PT["annot"] - 1,
                            color=t.bad)
                continue
            if not isinstance(r["predicted_rel_err_rel"], (int, float)):
                continue
            xs.append(x)
            ys.append(r["predicted_rel_err_rel"])
            action = f"{r['mesher']} p{r['order']}"
            if action != last_action:
                ax.annotate(action, (x, r["predicted_rel_err_rel"]),
                            xytext=(0, 7), textcoords="offset points",
                            ha="center", fontsize=fs.FONT_PT["annot"] - 1,
                            color=t.muted)
                last_action = action
        ax.plot(xs, ys, color=t.accent, linewidth=1.8, zorder=3)
        ax.scatter(xs, ys, s=34, color=t.accent, edgecolor=t.ink, linewidth=0.7,
                   zorder=4)
        fs.panel_title(ax, part)
        ax.grid(axis="x", visible=False)

    labels = [f"{b // 1000}k" for b in budgets] + ["no\ncap"]
    for ax in axes[0]:
        ax.set_xticks(list(x_of_budget.values()) + [x_uncapped], labels)
    axes[0][0].set_ylabel("predicted rel_err_rel (lower is better)")
    axes[0][1].set_xlabel("DOF budget (--advisor-max-dof)", labelpad=10)

    out = outdir / "bench_advisor_budget.png"
    return fs.finish(fig, out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render PolyMesh benchmark charts.")
    ap.add_argument(
        "--outdir", type=Path, default=REPO / "docs/assets/showcase",
        help="output directory for the PNGs",
    )
    ap.add_argument(
        "--only", action="append", default=[],
        help="render only these charts (dof_time|tier1|mms|advisor_budget); repeatable",
    )
    args = ap.parse_args(argv)

    fs.use("light")
    args.outdir.mkdir(parents=True, exist_ok=True)

    charts = {"dof_time": plot_dof_time, "tier1": plot_tier1, "mms": plot_mms,
              "advisor_budget": plot_advisor_budget}
    wanted = args.only or list(charts)
    unknown = [w for w in wanted if w not in charts]
    if unknown:
        raise SystemExit(f"unknown chart(s): {', '.join(unknown)}")
    for key in wanted:
        charts[key](args.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
