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


#: Tier-1 metric names and mesh kinds reach this module as phrase fragments
#: parsed out of markdown columns -- "SCF at hole equator", "u_r at inner
#: wall", "hex20 annulus, exact field BC" -- and not as dataset column or
#: model head names, so a ``figstyle.QUANTITY_LABELS`` lookup can never match
#: one; the keys there are identifiers. The report itself stays untouched: it
#: is a committed artifact and its column headings are provenance. So the
#: abbreviations those fragments carry are spelled out here, at the places
#: they are drawn, and nothing else in the fragment is rewritten. Do not
#: "simplify" these into QUANTITY_LABELS -- they would silently stop matching.
METRIC_WORDS = {
    # radial displacement at the inner wall of the Lamé cylinder
    "u_r": "radial displacement",
    # stress concentration factor at the hole / cavity equator
    "SCF": "peak stress factor",
}

#: Element names, as they are spelled in both the Tier-1 mesh-kind column and
#: the Tier-2 element rows. The node count is the part a reader can act on: it
#: says how much the element can bend between its corners.
ELEMENT_WORDS = {
    "tet4": "4-node tets",
    "tet10": "10-node tets",
    "hex8": "8-node hex bricks",
    "hex20": "20-node hex bricks",
}


def spell_out(name: str, table: dict[str, str]) -> str:
    """Replace whole tokens from ``table`` in a parsed phrase fragment."""
    for token, words in table.items():
        name = re.sub(rf"\b{re.escape(token)}\b", words, name)
    return name


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
    energy_ratio = grad["accuracy"]["value"] / base["accuracy"]["value"]

    # DOF, seconds and percent share no unit, so each pair is drawn as a
    # fraction of its own baseline: one zero-anchored axis carries all three,
    # the reduction IS the bar length, and the energy pair lands on the same
    # scale as the two it paid for. That pair is one-sided in the source --
    # 0.0888 % graded against 0.0854 % uniform, i.e. 1.04x the baseline
    # deficit, and the JSON records no energy pass flag -- so the row states
    # "graded higher" rather than an "accuracy held" verdict, and both parsed
    # values stay on the face as the per-bar labels.
    rows = [
        {
            "name": "Unknowns the solver has to solve for "
                    f"({fs.quantity_label('n_dof')})",
            "vals": (base["dofs"], grad["dofs"]),
            "fmt": lambda v: f"{int(v):,} unknowns",
            "stat": f"{dof_ratio:.2f}\u00d7 fewer",
        },
        {
            "name": "Time to build the mesh and solve it",
            "vals": (base["wall_time_s"]["total"], grad["wall_time_s"]["total"]),
            "fmt": lambda v: f"{v:.3f} s",
            "stat": f"{time_ratio:.1f}\u00d7 faster",
        },
        {
            "name": "Energy shortfall against the reference answer "
                    "(lower is better)",
            "vals": (base["accuracy"]["value"], grad["accuracy"]["value"]),
            "fmt": lambda v: f"{v:.4f} %",
            "stat": f"{energy_ratio:.2f}\u00d7 the shortfall, graded higher",
        },
    ]

    t = fs.theme()
    base_st = fs.series("baseline", "same cell size everywhere (frozen baseline)")
    grad_st = fs.series("graded_tet", "smaller cells toward the sharp inside corner")
    note = repeat_note(raw)
    # The ratios are now the headline stat on each row, so the prose carries
    # what the marks cannot: what is held fixed, whose baseline this is, and
    # how the timings were sampled.
    subtitle = (
        "Same solver, same 10-node tetrahedron elements, same part \u2014 the "
        "only difference is where the mesh puts its small cells. Every bar is "
        "PolyMesh measured against its own frozen baseline, never against "
        "another program. "
        + note.split(" \u2014 ")[0].capitalize() + "."
    )
    footer = fs.footer_source(
        D6_JSON, n=len(raw),
        note=f"baseline vs graded cases, label {base['label']} \u00b7 {note}",
    )
    fs.assert_glyphs(subtitle, footer, base_st.label, grad_st.label)

    fig, axes = fs.figure(
        "D6 \u00b7 L-domain: a mesh graded to the features vs PolyMesh's own frozen baseline",
        subtitle=subtitle, footer=footer, size="full",
    )
    ax = axes[0][0]

    # Row geometry in data units, y running downward. Measured on the 1800 px
    # render: the axes is 644 px tall, one data unit is 160 px, so the
    # 0.40-unit bars are 64 px thick and cover 60% of the axes height. A row
    # heading clears its own pair by 10 px and the pair above it by 36 px; that
    # 3.7:1 gap is what makes each heading read as belonging to the bars under
    # it instead of floating between two rows.
    PITCH = 1.26
    BAR_H = 0.40
    Y_BASE, Y_GRAD = 0.24, 0.66
    HEAD_DY = 0.02
    # 1.04 (the longest bar) of 1.20 = 87% of the axis width is data; the rest
    # is exactly the room the "0.0888 %" end labels need at 9 pt.
    XMAX = 1.20

    for i, row in enumerate(rows):
        top = i * PITCH
        ref = row["vals"][0]
        for y_off, val, st in ((Y_BASE, row["vals"][0], base_st),
                               (Y_GRAD, row["vals"][1], grad_st)):
            frac = val / ref
            ax.barh(top + y_off, frac, height=BAR_H, color=st.color,
                    edgecolor="none", zorder=3)
            label = row["fmt"](val)
            fs.assert_glyphs(label)
            ax.annotate(
                label, (frac, top + y_off), xytext=(6, 0),
                textcoords="offset points", ha="left", va="center",
                fontsize=fs.FONT_PT["annot"], color=t.ink, zorder=5,
            )
        fs.assert_glyphs(row["name"], row["stat"])
        # Name left, stat right, both on the free line above the pair: nothing
        # else is drawn at that height, so neither can collide with a bar label.
        # The 6 pt inset keeps the stat off the panel edge it was flush with.
        ax.annotate(row["name"], (0.0, top - HEAD_DY), xytext=(0, 0),
                    textcoords="offset points", ha="left", va="bottom",
                    fontsize=fs.FONT_PT["label"], color=t.muted, zorder=5)
        ax.annotate(row["stat"], (XMAX, top - HEAD_DY), xytext=(-6, 0),
                    textcoords="offset points", ha="right", va="bottom",
                    fontsize=fs.FONT_PT["panel"], weight="bold", color=t.ink,
                    zorder=5)

    ax.set_xlim(0, XMAX)
    ax.set_ylim(2 * PITCH + Y_GRAD + BAR_H / 2 + 0.14, -0.50)
    ax.set_yticks([])
    ax.spines["left"].set_visible(False)
    ax.set_xticks([0.0, 0.25, 0.5, 0.75, 1.0],
                  ["0", "25%", "50%", "75%", "100%"])
    ax.grid(axis="x", visible=True, color=t.grid, lw=0.6, zorder=0)
    ax.grid(axis="y", visible=False)
    # Drawn under the bars so the 100% mark reads in the row gaps without
    # putting an outline on the baseline bars that end on it.
    ax.axvline(1.0, color=t.rule, lw=0.8, zorder=1)
    ax.set_xlabel("share of the frozen baseline mesh (baseline = 100%)")

    handles = [fs.plt.Rectangle((0, 0), 1, 1, color=st.color, label=st.label)
               for st in (base_st, grad_st)]
    ax.legend(handles=handles, loc="upper left", ncols=2, handlelength=1.2,
              handleheight=0.9, borderpad=0.0, columnspacing=1.8,
              borderaxespad=0.2)

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
    ratios = [r["err_pct"] / r["tol_pct"] for r in rows]

    # Two-line y tick labels carrying the mesh kind cost a 565 px left gutter
    # on the 1800 px canvas (31% of the width). Split "Lamé cylinder (hex20
    # sector)" into a short head drawn over the bar and the mesh kind as a
    # muted line at the right of the same row, so the axis needs no gutter.
    heads: list[str] = []
    meshes: list[str] = []
    for r in rows:
        head, _, mesh = r["case"].partition(" (")
        heads.append(f"{head} \u00b7 {spell_out(r['metric'], METRIC_WORDS)}")
        meshes.append(spell_out(mesh.rstrip(")"), ELEMENT_WORDS))
    values = [f"{r['err_pct']:.3g}% of {r['tol_pct']:g}%" for r in rows]

    # The verdict is computed from the parsed data, never asserted in a string.
    outside = [r for r in rows if r["err_pct"] > r["tol_pct"]]
    n = len(rows)
    if outside:
        # The failing cases are named over their own bars; the title states the
        # count so it stays readable however many cases fail.
        verdict = f"{len(outside)} of {n} cases OUTSIDE the error allowed"
    else:
        verdict = f"all {n} cases inside the error allowed"
    title = f"Tier-1 checks against exact textbook answers \u2014 {verdict}"
    # The mesh kind of every case is now drawn on its own row, so the prose
    # keeps the fact without re-listing them.
    subtitle = (
        "Each bar is one case: the filled part is the error PolyMesh made and "
        "the whole track is the error allowed for that case. Measured on "
        "regular, purpose-built test meshes \u2014 not on the grid-fill meshes "
        "the product ships."
    )
    footer = fs.footer_source(
        GATE1_JSON, GATE1_MD, n=n,
        note=f"{checked}/{len(json_recs)} JSON records cross-checked",
    )

    # Colour encodes how much of each case's own tolerance it spends. The
    # parsed ratios are 0.007, 0.34, 0.37, 0.50 and 0.59, so a split at half
    # the budget separates the three cases with an order of margin from the two
    # that spend most of theirs; the third band only appears if a case fails.
    # The Timoshenko case is exactly 1.5/3 = 0.500, so the safe band is spelled
    # "half ... or less", not "under half": at the boundary the looser wording
    # would state a bound the data does not support.
    band_color_of = ("ok", "warn", "bad")
    band_label = (
        "used half the error allowed, or less",
        "used more than half the error allowed",
        "outside the error allowed",
    )
    bands = [2 if r > 1.0 else (1 if r > 0.5 else 0) for r in ratios]
    limit_label = "the limit"
    xlabel = "measured error as a share of the error allowed for that case"
    fs.assert_glyphs(title, subtitle, footer, limit_label, xlabel,
                     *heads, *meshes, *values, *band_label)

    t = fs.theme()
    fig, axes = fs.figure(title, subtitle=subtitle, footer=footer, size="full")
    ax = axes[0][0]
    ypos = np.arange(n)[::-1]

    # Bullet rows: a full-width track from 0 to each case's own tolerance, with
    # the measured error filling part of it. The unfilled tail *is* the
    # remaining margin, so margin is legible without a second number. The
    # track is drawn in the rule tone, not the grid tone: at 180 dpi the grid
    # tone over the panel left the empty tail barely separable from the panel.
    bar_h = 0.40
    ax.barh(ypos, 1.0, height=bar_h, color=t.rule, edgecolor="none", zorder=2)
    ax.barh(ypos, ratios, height=bar_h,
            color=[getattr(t, band_color_of[b]) for b in bands],
            edgecolor="none", zorder=3)

    # The limit is the point of the chart, so it is a solid rule over the bars
    # in ink -- not one of the grid lines -- and it is named on the value axis
    # directly beneath itself. Naming it in the band above the panel collided
    # with the legend as soon as a failing case widened both.
    ax.axvline(1.0, color=t.ink, linewidth=1.6, zorder=5)

    # 10 pt text is 0.18 row-units tall at this panel height, so a baseline
    # 0.26 above the row centre clears the 0.20 bar top and still stops
    # 0.36 short of the next row's bar.
    label_dy = 0.26
    for y, ratio, head, mesh, value in zip(ypos, ratios, heads, meshes,
                                           values):
        ax.text(0.006, y + label_dy, head, ha="left", va="bottom",
                fontsize=fs.FONT_PT["label"], color=t.ink, zorder=6)
        if mesh:
            ax.text(0.985, y + label_dy, mesh, ha="right", va="bottom",
                    fontsize=fs.FONT_PT["annot"] - 0.5, color=t.muted,
                    zorder=6)
        ax.text(ratio + 0.012, y, value, ha="left", va="center",
                fontsize=fs.FONT_PT["annot"], color=t.ink, zorder=6)

    # Room for the widest value label (13 characters ~ 0.09 of the span) past
    # the longest bar, and never less than a little past the limit rule.
    xmax = max(1.05, max(ratios) + 0.14)
    # 0.25 steps put ~420 px between ticks here; past 1.6 spans they crowd the
    # two-line limit tick, so the step doubles.
    step = 0.25 if xmax <= 1.6 else 0.5
    ticks = [round(v, 2) for v in np.arange(0.0, xmax + 1e-9, step)]
    ax.set_xlim(0.0, xmax)
    ax.set_xticks(ticks)
    ax.set_xticklabels([f"1.0\n{limit_label}" if v == 1.0 else f"{v:g}"
                        for v in ticks])
    for tick_label, v in zip(ax.get_xticklabels(), ticks):
        # Only the limit reads in ink; the rest of the scale stays muted so the
        # limit is the value the eye lands on.
        tick_label.set_color(t.ink if v == 1.0 else t.muted)
    ax.set_xlabel(xlabel)
    ax.set_ylim(-0.5, n - 1 + 0.62)
    ax.set_yticks([])
    ax.spines["left"].set_visible(False)
    ax.grid(axis="x", color=t.grid, linewidth=0.6, zorder=0)
    ax.grid(axis="y", visible=False)

    # Function-local: the module's import block is owned elsewhere, and this is
    # the only figure here that needs a swatch handle.
    from matplotlib.patches import Patch

    handles = [Patch(facecolor=getattr(t, band_color_of[b]), edgecolor="none",
                     label=band_label[b]) for b in sorted(set(bands))]
    ax.legend(handles=handles, loc="lower left", bbox_to_anchor=(0.0, 1.005),
              ncol=len(handles), frameon=False, borderaxespad=0.0,
              handlelength=1.1, handleheight=0.9, columnspacing=1.6)

    out = outdir / "bench_tier1.png"
    return fs.finish(fig, out)


# ---------------------------------------------------------------------------
# (c) bench_mms.png
# ---------------------------------------------------------------------------
def plot_mms(outdir: Path) -> Path:
    """Measured convergence order against theory, and the signed gap.

    Paired bars cannot carry this figure's own claim: at bar scale 3.98 against
    4 and 0.997 against 1 are the same picture. So the absolute orders sit on a
    dot-against-reference-tick row with both numbers printed, and the row below
    magnifies the same pair as a signed percent deviation -- the only encoding
    here in which a 0.3% gap is legible.
    """
    groups = [
        {
            "title": "Frozen baseline elements (tets and bricks)",
            "sub": "4- and 8-node elements are linear,\n"
                   "10- and 20-node are quadratic\n"
                   "cell size halved: 4 \u2192 8 cells per side",
            "rows": parse_mms_elements(),
            "src": GATE1_MD,
        },
        {
            "title": "Higher-order elements (hierarchical p-basis)",
            "sub": "element order raised from 1 to 4\n"
                   "integrated-Legendre hierarchical basis",
            "rows": parse_mms_hierarchical(),
            "src": PROGRESS_MD,
        },
    ]

    all_rows = [r for g in groups for r in g["rows"]]
    for r in all_rows:
        r["dev_pct"] = (r["observed"] / r["theory"] - 1.0) * 100.0
    devs = [r["dev_pct"] for r in all_rows]
    # Same computed quantity as the bar version -- the worst shortfall of
    # measured order against theory -- but stated one-sided, because one parsed
    # case (p=1, 1.02) is 2.0% ABOVE theory: a two-sided "within 0.7%" would be
    # contradicted by the deviation row printed directly beneath it.
    shortfall = max(0.0, -min(devs))
    excess = max(0.0, max(devs))
    if shortfall <= 5.0:
        verdict = (f"no measured order is more than {shortfall:.1f}% below "
                   f"theory ({len(all_rows)} cases)")
    else:
        verdict = (f"worst measured order {shortfall:.0f}% below theory "
                   f"({len(all_rows)} cases)")
    title = f"Convergence check \u2014 {verdict}"
    footer = fs.footer_source(
        *[g["src"] for g in groups], n=len(all_rows),
        note="theory order and measured order both read from the committed reports",
    )
    subtitle = (
        "Order is how fast the error shrinks as the cells get smaller: at "
        "order 2, halving the cell size cuts the error about fourfold, so "
        "higher is better. Every case here is solved against an answer known "
        "exactly up front (the method of manufactured solutions, MMS). Top "
        "row: the measured order (dot) against the order theory predicts "
        "(grey tick), both printed. Bottom row: the same pair as a signed gap "
        "from theory, on one shared scale across both groups."
    )

    def dev_label(value: float) -> str:
        # "+0.0%" would imply a signed miss where the report records an exact
        # hit (tet10 and hex20 both measure 2.000 against theory 2). The 0.05
        # cut is half the printed resolution, so nothing else rounds into it.
        return "0.0%" if abs(value) < 0.05 else f"{value:+.1f}%"

    order_label = "convergence order\n(energy-norm error)"
    dev_axis_label = "gap from theory (%)"
    theory_labels = [f"theory {r['theory']:g}" for r in all_rows]
    fs.assert_glyphs(title, subtitle, footer, order_label, dev_axis_label,
                     *[spell_out(r["label"], ELEMENT_WORDS)
                       for r in all_rows],
                     *[r["observed_text"] for r in all_rows], *theory_labels,
                     *[dev_label(r["dev_pct"]) for r in all_rows])

    t = fs.theme()
    theory_st = fs.series("theory", "theory order")
    meas_st = fs.series("measured", "measured order")
    fig, axes = fs.figure(
        title, subtitle=subtitle, footer=footer, size=(10.0, 6.9),
        nrows=2, ncols=2,
        share_y_axis="the two rows carry different quantities (absolute order "
                     "vs percent deviation), and the two order panels cover "
                     "different ranges (isoparametric p=1..2 vs hierarchical "
                     "p=1..4); the deviation row is pinned to one explicit "
                     "shared scale instead",
        sharex="col",
        gridspec_kw={"width_ratios": [len(g["rows"]) for g in groups],
                     "height_ratios": [1.0, 0.92]},
    )

    # One deviation scale for both groups: the element cases sit within 0.3% of
    # theory and the p-basis cases within 2.0%, and re-scaling each panel to its
    # own extremes would draw the two as equally far off. 30% of the pooled
    # span on each side is what the outermost value label needs at 180 dpi.
    dev_lo, dev_hi = min(devs), max(devs)
    dev_pad = (dev_hi - dev_lo) * 0.30
    for col, g in enumerate(groups):
        rows = g["rows"]
        ax_abs, ax_dev = axes[0][col], axes[1][col]
        x = np.arange(len(rows), dtype=float)
        theory = np.array([r["theory"] for r in rows], dtype=float)
        obs = np.array([r["observed"] for r in rows], dtype=float)
        dev = np.array([r["dev_pct"] for r in rows], dtype=float)

        # Theory is a reference tick, not a bar: a bar from 0 to 4 hides a 0.02
        # difference inside its own outline. Marker size is in points and one x
        # slot is ~75 pt wide at this panel width, so 34 pt spans about 45% of
        # a slot -- wide enough to read as a level, clear of its neighbours.
        ax_abs.plot(x, theory, ls="none", marker="_", ms=34, mew=2.4,
                    color=theory_st.color, label=theory_st.label, zorder=3)
        ax_abs.plot(x, obs, ls="none", marker="o", ms=9.5, color=meas_st.color,
                    mec=t.bg, mew=0.8, label=meas_st.label, zorder=4)

        # Each number sits on the far side of its own mark, so the pair stays
        # 18 pt apart even where the marks are 0.003 apart (tet4: 0.997 vs 1).
        for xi, th, ob, otext in zip(x, theory, obs,
                                     [r["observed_text"] for r in rows]):
            under = ob <= th
            ax_abs.annotate(otext, (xi, ob), xytext=(0, -9 if under else 9),
                            textcoords="offset points", ha="center",
                            va="top" if under else "bottom",
                            fontsize=fs.FONT_PT["annot"], color=t.ink, zorder=5)
            ax_abs.annotate(f"theory {th:g}", (xi, th),
                            xytext=(0, 9 if under else -9),
                            textcoords="offset points", ha="center",
                            va="bottom" if under else "top",
                            fontsize=fs.FONT_PT["annot"] - 1.0, color=t.muted,
                            zorder=5)

        # Dots encode position, not length, so this panel need not reach 0.
        # 22% of the data span on each side leaves the marks spread over ~70%
        # of the panel height and still clears the labels above and below them.
        low = min(theory.min(), obs.min())
        high = max(theory.max(), obs.max())
        ax_abs.set_ylim(low - 0.22 * (high - low), high + 0.22 * (high - low))
        # The theory orders are integers, so the gridlines *are* theory levels.
        ax_abs.set_yticks(np.unique(theory))
        ax_abs.grid(axis="y", color=t.grid, linewidth=0.6, zorder=0)
        ax_abs.grid(axis="x", visible=False)
        fs.panel_title(ax_abs, g["title"])

        # Zero is theory. A stem from it gives the sub-percent misses a length
        # to read; the two exact hits (2.000 vs 2) correctly have no stem.
        ax_dev.axhline(0.0, color=t.rule, linewidth=1.4, zorder=2)
        ax_dev.vlines(x, 0.0, dev, color=meas_st.color, linewidth=2.4, zorder=3)
        ax_dev.plot(x, dev, ls="none", marker="o", ms=8.0, color=meas_st.color,
                    mec=t.bg, mew=0.8, zorder=4)
        for xi, dv in zip(x, dev):
            ax_dev.annotate(dev_label(dv), (xi, dv),
                            xytext=(0, 9 if dv >= 0.0 else -9),
                            textcoords="offset points", ha="center",
                            va="bottom" if dv >= 0.0 else "top",
                            fontsize=fs.FONT_PT["annot"], color=t.ink, zorder=5)
        ax_dev.set_ylim(dev_lo - dev_pad, dev_hi + dev_pad)
        ax_dev.grid(axis="y", color=t.grid, linewidth=0.6, zorder=0)
        ax_dev.grid(axis="x", visible=False)
        # Half a slot of margin: the outer ticks are 34 pt wide and the outer
        # value labels are centred on them, so both clear the spines.
        ax_dev.set_xlim(-0.62, len(rows) - 1 + 0.62)
        # One x slot is ~190 px here and "20-node hex bricks" needs ~210, so
        # the tick breaks after the node count. The p-basis labels carry no
        # element token and pass through both steps unchanged.
        ax_dev.set_xticks(x, [spell_out(r["label"], ELEMENT_WORDS)
                              .replace(" ", "\n", 1) for r in rows])
        ax_dev.set_xlabel(g["sub"])

        if col == 0:
            ax_abs.set_ylabel(order_label)
            ax_dev.set_ylabel(dev_axis_label)
            leg = ax_abs.legend(loc="upper left", fontsize=fs.FONT_PT["legend"],
                                framealpha=0.0, borderaxespad=0.6,
                                handlelength=1.9, handletextpad=0.7,
                                labelspacing=0.45)
            # In the panel the reference tick is 34 pt wide; reused at that
            # size in the legend the dash ran straight through its own label,
            # so the legend copy is cut to the 17 pt the handle box has
            # (handlelength 1.9 x the 9 pt legend font).
            for handle in leg.legend_handles:
                if handle.get_marker() == "_":
                    handle.set_markersize(17)
        else:
            # Named once, on the panel whose right margin near zero is empty:
            # the same tag on the left panel would sit on hex20's 0.0% label.
            ax_dev.text(0.99, 0.0, "theory", ha="right", va="bottom",
                        transform=ax_dev.get_yaxis_transform(),
                        fontsize=fs.FONT_PT["annot"] - 0.5, color=t.muted,
                        zorder=5)

    # The element deviations are all at or just below zero, so the top half of
    # that panel is free: the pooled extremes go there rather than into prose.
    fs.annotate_n(axes[1][0], len(all_rows), what="convergence cases",
                  loc="upper left",
                  extra=f"furthest above theory +{excess:.1f}%\n"
                        f"furthest below theory -{shortfall:.1f}%")

    out = outdir / "bench_mms.png"
    return fs.finish(fig, out)


# ---------------------------------------------------------------------------
# (d) bench_advisor_budget.png
# ---------------------------------------------------------------------------
def plot_advisor_budget(outdir: Path) -> Path:
    """What the learned advisor picks as the DOF cap tightens (ADR-0034).

    Every point is a real CLI run recorded in the sweep JSON: the chosen
    action's predicted per-case relative error (rel_err_rel — the ranking
    score the chooser optimizes) against the budget it was given. All three
    cases sit on one score axis and every run of a constant action is drawn
    as one coloured step, so which action wins at which cap reads before any
    number does. Runs the advisor refused (every candidate over budget) are
    drawn as refusal marks, never silently dropped.
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
    # x positions: one column per budget, with the uncapped run one step past
    # the loosest budget so it sits on the same axis.
    x_of_budget = {b: i for i, b in enumerate(budgets)}
    x_uncapped = len(budgets)
    cap_labels = [f"{b // 1000}k" for b in budgets] + ["no cap"]

    t = fs.theme()
    n_runs = len(records)
    n_refusals = sum(1 for r in records if r["budget_refusal"])
    # The sweep invoked one CLI solve per point and the JSON records each exit
    # code. The plotted score is the model's prediction either way, so the
    # marks stand -- but "one CLI solve per point" on its own reads as if all
    # of them ran, so the count comes off the cli_exit field and onto the
    # figure: into the footer, and as a ring on every point it applies to.
    # Indexed, not .get(): a sweep file that never recorded exit codes must
    # fail loudly here, not quietly report "0 of 21 exited nonzero".
    failed = [r for r in records if r["cli_exit"] != 0]
    # Every capped run whose predicted DOF can be compared against its cap --
    # the population the over-budget count is drawn from, so the verdict
    # states a count against its own denominator instead of a bound.
    checkable = [
        r for r in records
        if not r["vetoed"] and r["max_dof"] > 0
        and isinstance(r["predicted_dof"], (int, float))
    ]
    over = [r for r in checkable if r["predicted_dof"] > r["max_dof"]]
    # The verdict is computed from the records, not asserted in a string, and
    # it prints both numbers: the data is one-sided, so it says how many of
    # how many rather than claiming a bound over runs nobody measured.
    verdict = (f"{len(over)} of {len(checkable)} capped picks over budget "
               f"({n_runs} runs, {n_refusals} honest refusal"
               f"{'s' if n_refusals != 1 else ''})")
    title = f"Learned mesh advisor under a budget — {verdict}"
    subtitle = (
        f"The advisor is handed a {fs.quantity_label('max_dof')} — a cap on "
        "the unknowns the solver has to solve for — and picks a mesh under it. "
        "Each coloured step is one pick, held until a looser cap buys a better "
        "one; the colour says which mesher, and cell size is a fraction of the "
        "part. All three parts share one score axis: the height is the model's "
        f"predicted {fs.quantity_label('rel_err_rel')}, so lower is better. A "
        "refusal means no candidate mesh fit the budget."
    )
    footer = fs.footer_source(
        ADVISOR_SWEEP_JSON, n=n_runs,
        note=f"one CLI solve invoked per point (polymesh solve --advisor "
             f"--advisor-max-dof N), of which {len(failed)} of {n_runs} "
             f"failed to mesh or solve — the picked action never came back; "
             f"every plotted score is the shipped ONNX model's prediction, "
             f"not a measured outcome",
    )
    fs.assert_glyphs(title, subtitle, footer, *parts)

    def action_key(r: dict) -> tuple:
        """Everything that makes the pick a different action.

        The mesher and order alone are not the action: stepped_shaft_s0 stays
        on hybrid_zoo p1 at every cap but moves h_rel 0.1 -> 0.08 with one
        adapt pass at 4k, and its predicted score moves with it. Folding on
        the mesher alone would draw that change as no change.
        """
        return (r["mesher"], int(r["order"]), round(float(r["h_rel"]), 4),
                int(r["adapt_passes"]))

    # Fold each case into runs of one constant action, so six equal picks are
    # one step carrying one label instead of six repeated labels.
    cases: list[dict] = []
    for part in parts:
        # Uncapped (max_dof == 0) sorts last so the walk goes left-to-right
        # and ends at "no cap".
        runs = sorted((r for r in records if r["part"] == part),
                      key=lambda r: (r["max_dof"] == 0, r["max_dof"]))
        steps: list[dict] = []
        refused: list[int] = []
        for r in runs:
            x = x_uncapped if r["max_dof"] == 0 else x_of_budget[r["max_dof"]]
            score = r["predicted_rel_err_rel"]
            if r["budget_refusal"] or not isinstance(score, (int, float)):
                # A refusal has no action and therefore no score: the step
                # breaks here rather than bridging over the gap.
                if r["budget_refusal"]:
                    refused.append(x)
                continue
            score = float(score)
            key = action_key(r)
            if (steps and steps[-1]["key"] == key and steps[-1]["x1"] == x - 1
                    and abs(steps[-1]["y"] - score) < 1e-12):
                steps[-1]["x1"] = x
                steps[-1]["xs"].append(x)
            else:
                steps.append({"key": key, "y": score, "x0": x, "x1": x,
                              "xs": [x], "bad": []})
            if r["cli_exit"] != 0:
                # This point's action never came back from the CLI, so the
                # score on it is a prediction with no measured outcome behind
                # it. Marked per record: the exit codes are not uniform
                # within an action in general.
                steps[-1]["bad"].append(x)
        if steps or refused:
            cases.append({"part": part, "steps": steps, "refused": refused})

    scores = [s["y"] for c in cases for s in c["steps"]]
    lo, hi = min(scores), max(scores)
    span = (hi - lo) or 1.0

    # One axes carries all three cases, so the scores stay on a single shared
    # scale *and* the data fills the panel: the same numbers split over three
    # panels left two of them with ~89% of their height empty.
    fig, axes = fs.figure(
        title, subtitle=subtitle, footer=footer, size="hero", nrows=1, ncols=1,
    )
    ax = axes[0][0]

    # Headroom: a two-line annotation is 6.6% of the y range (9 pt lines at
    # 22 px on the 675 px tall hero axes) and stands 8 pt off its step, so 17%
    # below and 14% above the data clears the labels hanging off the outermost
    # steps. The data still keeps 76% of the axis height.
    bottom, top = lo - 0.17 * span, hi + 0.14 * span
    # Case names sit in a gutter left of the first cap. Measured: bold 10 pt
    # "stepped_shaft_s0" renders 207 px wide, 6.8% of a column per character
    # on this canvas; the 0.22-column margin keeps it off the tick labels.
    gutter = 0.068 * max(len(p) for p in parts) + 0.22
    left, right = -0.5 - gutter, x_uncapped + 0.5
    ax.set_xlim(left, right)
    ax.set_ylim(bottom, top)
    x_range, y_range = right - left, top - bottom
    # Same canvas, 9 pt: one character is 0.61% of the x range and one text
    # line 3.3% of the y range. The stand-off is 8 pt = 2.5% of the y range,
    # not 4 pt: a nonzero-exit ring is 11 pt across, so its 12 px radius would
    # otherwise reach into the first line of the label above its marker.
    char_x = 0.0061 * x_range
    line_y = 0.033 * y_range
    pad_y = 0.025 * y_range

    ax.grid(axis="x", visible=False)
    ax.grid(axis="y", color=t.grid, linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    ax.set_xticks(range(x_uncapped + 1), cap_labels)
    ax.set_xlabel(f"{fs.quantity_label('max_dof')} the advisor was given "
                  "(--advisor-max-dof)")
    # The score is a log10 distance from the case's own median action, so the
    # axis says what a step of 0.30 on it actually means.
    ax.set_ylabel("predicted error of the pick (lower is better)\n"
                  "measured against this part's middle pick\n"
                  f"({fs.DECADES_NOTE})")

    # Past the last action change nothing moves; shading that stretch says so
    # without asserting anything the records do not carry.
    last_change = max((s["x0"] for c in cases for s in c["steps"][1:]),
                      default=-1)
    settled_note = ""
    if 0 < last_change < x_uncapped:
        ax.axvspan(last_change - 0.5, right, color=t.grid, alpha=0.45,
                   zorder=0, linewidth=0)
        settled_note = (f"nothing changes from {cap_labels[last_change]} "
                        f"up to no cap")

    # Placed label boxes: a label goes to the side that has room instead of
    # landing on a marker, on a step, or on another label.
    placed: list[tuple[float, float, float, float]] = []

    def fits(box: tuple[float, float, float, float]) -> bool:
        bx0, bx1, by0, by1 = box
        if by0 < bottom or by1 > top:
            return False
        for px0, px1, py0, py1 in placed:
            if bx0 < px1 and px0 < bx1 and by0 < py1 and py0 < by1:
                return False
        for other in cases:
            for s in other["steps"]:
                if (bx0 < s["x1"] + 0.5 and s["x0"] - 0.5 < bx1
                        and by0 <= s["y"] <= by1):
                    return False
        return True

    def clearance(x0: float, x1: float, y: float, sign: int) -> float:
        """Room above (sign +1) or below a level before another case's step.

        The window is widened by 0.6 of a column: a label that stops just
        short of a neighbouring step still reads as if it belonged to it, and
        that ambiguity is what put "hybrid_vem p2  -2.12" in the lane between
        the two hybrid_vem levels 24 px apart.
        """
        room = (top - y) if sign > 0 else (y - bottom)
        for other in cases:
            for s in other["steps"]:
                if x0 - 0.6 > s["x1"] + 0.5 or s["x0"] - 0.5 > x1 + 0.6:
                    continue
                gap = (s["y"] - y) * sign
                if gap > 0:
                    room = min(room, gap)
        return room

    def put(text: str, x: float, y: float) -> None:
        lines = text.split("\n")
        w = char_x * max(len(ln) for ln in lines)
        h = line_y * len(lines)
        above = (x, x + w, y + pad_y, y + pad_y + h)
        below = (x, x + w, y - pad_y - h, y - pad_y)
        need = h + pad_y
        # Only sides that hit nothing are candidates; among those, prefer one
        # with a label's worth of clearance, then the roomier one. A side that
        # collides is never chosen, so a short-clearance placement (a label
        # rising past a step that ends before it starts) is the worst case
        # rather than a label lying on a step.
        options = []
        if fits(above):
            options.append((clearance(x, x + w, y, +1), above, "bottom", 8.0))
        if fits(below):
            options.append((clearance(x, x + w, y, -1), below, "top", -8.0))
        if options:
            _, box, va, dy = max(options, key=lambda o: (o[0] >= need, o[0]))
        else:
            box, va, dy = above, "bottom", 8.0
        placed.append(box)
        ax.annotate(text, (x, y), xytext=(0, dy), textcoords="offset points",
                    ha="left", va=va, fontsize=fs.FONT_PT["annot"],
                    color=t.ink, zorder=6)

    # Reserve the fixed furniture first -- case names in the gutter and the
    # settled-region note -- so the per-step labels route around them.
    for case in cases:
        if not case["steps"]:
            continue
        first = case["steps"][0]
        ax.annotate(case["part"], (-0.62, first["y"]), ha="right",
                    va="center", fontsize=fs.FONT_PT["label"], weight="bold",
                    color=t.ink, zorder=6)
        half = 0.5 * line_y * fs.FONT_PT["label"] / fs.FONT_PT["annot"]
        placed.append((left, -0.62, first["y"] - half, first["y"] + half))
    if settled_note:
        ax.annotate(settled_note, (last_change - 0.35, top - pad_y),
                    ha="left", va="top", fontsize=fs.FONT_PT["annot"],
                    color=t.muted, zorder=6)
        placed.append((last_change - 0.35,
                       last_change - 0.35 + char_x * len(settled_note),
                       top - pad_y - line_y, top - pad_y))
    if failed:
        # The key exists only to say what a ring means, so it carries the
        # count and nothing else: the footer is the provenance line and it
        # already states that the score is the model's prediction and not a
        # measured outcome. One line, centred in the widest gap between two
        # levels -- here 1.16 score units against the 0.10 one line needs --
        # so it never lands on a step.
        levels = sorted({s["y"] for c in cases for s in c["steps"]})
        gap, gap_lo = max(((b - a, a) for a, b in zip(levels, levels[1:])),
                          default=(y_range, bottom))
        x_key = (last_change - 0.25) if settled_note else 0.6
        y_mid = gap_lo + 0.5 * gap
        head = (f"ringed: {len(failed)} of {n_runs} picks failed to mesh "
                f"or solve")
        fs.assert_glyphs(head)
        ax.scatter([x_key - 0.22], [y_mid], marker="o", s=120,
                   facecolors="none", edgecolors=t.bad, linewidths=1.4,
                   zorder=6)
        ax.annotate(head, (x_key, y_mid), ha="left", va="center",
                    fontsize=fs.FONT_PT["annot"], color=t.ink, zorder=6)
        placed.append((x_key - 0.35, x_key + char_x * len(head),
                       y_mid - 0.5 * line_y, y_mid + 0.5 * line_y))
    # The mesher is named once, in the legend, instead of on every step. The
    # plain names run to 26 characters, and repeating one on each of the seven
    # step labels put three of them on top of each other; the colour already
    # carries the identity, so the legend is the cheaper place to spell it.
    mesher_names: list[str] = []
    for case in cases:
        for s in case["steps"]:
            if s["key"][0] not in mesher_names:
                mesher_names.append(s["key"][0])
    mesher_styles = [fs.series(name) for name in mesher_names]
    fs.assert_glyphs(*[st.label for st in mesher_styles])
    # Solid, like the steps themselves: the shared dash cycle would show a
    # dashed key beside a solid line and read as a different series.
    mesher_handles = [
        fs.plt.Line2D([0], [0], color=st.color, linestyle="-", linewidth=3.0,
                      marker=st.marker, markersize=6, markeredgecolor=t.bg,
                      markeredgewidth=0.8, label=st.label)
        for st in mesher_styles
    ]
    legend_cols = 2 if len(mesher_handles) > 2 else 1
    ax.legend(handles=mesher_handles, loc="lower right", ncol=legend_cols,
              frameon=False, fontsize=fs.FONT_PT["legend"], handlelength=1.6,
              columnspacing=1.6, borderaxespad=0.6, labelspacing=0.35)
    # Reserved before any step label is placed, so the placement search treats
    # the legend as occupied space rather than drawing a label under it.
    legend_rows = -(-len(mesher_handles) // legend_cols)
    widest = max(len(st.label) for st in mesher_styles)
    placed.append((right - char_x * (widest + 10) * legend_cols, right,
                   bottom, bottom + line_y * (legend_rows + 0.6)))

    label_texts: list[str] = []
    for ci, case in enumerate(cases):
        prev = None
        for s in case["steps"]:
            st = fs.series(s["key"][0])
            # The step spans the caps it wins at, half a column past each end:
            # the sweep sampled only these caps, so the change is drawn
            # between the two caps that bracket it, not on top of one.
            ax.plot([s["x0"] - 0.5, s["x1"] + 0.5], [s["y"], s["y"]],
                    color=st.color, linewidth=3.0, solid_capstyle="butt",
                    zorder=3)
            if prev is not None and prev["x1"] + 1 == s["x0"]:
                ax.plot([s["x0"] - 0.5] * 2, [prev["y"], s["y"]],
                        color=st.color, linewidth=1.6, zorder=3)
            # One marker per record, so the run count in the title is
            # countable on the face of the chart.
            ax.scatter(s["xs"], [s["y"]] * len(s["xs"]), marker=st.marker,
                       s=38, color=st.color, edgecolor=t.bg, linewidth=0.8,
                       zorder=4)
            if s["bad"]:
                # A ring, not a recolour: the marker keeps its mesher
                # identity and the ring says the CLI run behind that point
                # exited nonzero. 120 pt^2 is an 11 pt ring around the 6 pt
                # marker, so it reads as an overlay and not as a series.
                ax.scatter(s["bad"], [s["y"]] * len(s["bad"]), marker="o",
                           s=120, facecolors="none", edgecolors=t.bad,
                           linewidths=1.4, zorder=5)
            _, order, h_rel, passes = s["key"]  # mesher is already `st` above
            # Plain words for the pick, and only what the colour cannot say:
            # the mesher is named once in the legend. One fact per line keeps
            # every label under a column wide, which is what the placement
            # search needs here -- the bottom three levels sit 0.11 apart on a
            # 2.2 span, so a two-column-wide label has nowhere to go and lands
            # on its neighbour.
            order_words = fs.quantity_label(f"policy_order_logit_{order}")
            lines = [order_words, f"cells {h_rel:g}"]
            if passes:
                lines.append(f"{passes} pass{'' if passes == 1 else 'es'} "
                             f"of refinement")
            # The score is a log10 distance from this part's median pick, so
            # the label prints the plain factor beside it: a bare -2.02 reads
            # as a small number when it stands for a hundredfold difference.
            factor = fs.times_off(abs(s["y"]))
            way = "lower" if s["y"] < 0.0 else "higher"
            lines.append(f"{s['y']:+.2f} \u2014 {factor} {way}")
            text = "\n".join(lines)
            label_texts.append(text)
            put(text, s["x0"] - 0.42, s["y"])
            prev = s
        for k, x in enumerate(case["refused"]):
            # No action was chosen, so there is no score to draw: the refusal
            # gets its own lane at the foot of the panel. The mark stays short
            # -- this lane is not routed around the step labels, and the
            # subtitle already says what a refusal is.
            y_frac = 0.03 + 0.06 * ci
            ax.scatter([x], [y_frac], marker="x", s=70, color=t.bad,
                       linewidth=2.0, zorder=6,
                       transform=ax.get_xaxis_transform())
            if k == 0:
                ax.annotate(f"refused  {case['part']}", (x, y_frac),
                            xycoords=("data", "axes fraction"), xytext=(9, 0),
                            textcoords="offset points", ha="left",
                            va="center", fontsize=fs.FONT_PT["annot"],
                            color=t.bad, zorder=6)
    fs.assert_glyphs(*label_texts, settled_note or "ok")

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

    # One visual identity for the whole figure set. The 3D renders, the
    # comparison grids, gui_studio.png and architecture.png are all drawn on the
    # dark stage; these charts were the only light-background artefacts in
    # docs/assets/showcase, so a README that shows them together read as two
    # unrelated documents. figstyle's DARK theme is the same Okabe-Ito palette
    # against #0E1116, so nothing about the encoding changes with the swap.
    fs.use("dark")
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
