# SPDX-License-Identifier: BSD-3-Clause
"""What the mesh search bought: ``mesh_progress``, ``accuracy_vs_cost`` and
``fidelity_vs_h`` figures, drawn from the advisor dataset rows."""
from __future__ import annotations

import math
from pathlib import Path
from typing import Any

import numpy as np

import figstyle as fs

from .paths import ADVISOR_DIR
from .report_common import OK_STATUS, corpus_rows, fmt, save, to_float

#: Geometric-fidelity residuals are distances in model units. On most surfaces
#: the boundary residual is legitimately at machine precision (~1e-15 of the
#: part diagonal), so a raw log axis spends sixteen decades on noise and
#: flattens every real trend. Values at or below this floor are pinned to the
#: floor line and labelled as machine precision — never dropped.
PRECISION_FLOOR = 1e-12

#: mesh_progress starts its window once this share of cases has a first
#: result, so the anytime median is taken over a fixed set of cases
START_COVERAGE_PCT = 85.0


# --- figure 2: mesh_progress.png --------------------------------------------


def _best_so_far(values: np.ndarray) -> np.ndarray:
    """Running minimum that ignores NaN and stays NaN until the first value."""
    out = np.full(values.shape, np.nan)
    best = math.inf
    for index, value in enumerate(values):
        if math.isfinite(value) and value < best:
            best = value
        if math.isfinite(best):
            out[index] = best
    return out


def _step_sample(times: np.ndarray, values: np.ndarray,
                 grid: np.ndarray) -> np.ndarray:
    """Right-continuous step lookup: value in force at each grid time."""
    index = np.searchsorted(times, grid, side="right") - 1
    out = np.full(grid.shape, np.nan)
    valid = index >= 0
    out[valid] = values[index[valid]]
    return out


def mesh_progress(rows: list[dict[str, str]], out_dir: Path) -> bool:
    corpus = corpus_rows(rows)
    if not corpus:
        print("no data yet — no advisor-row-v3 rows in dataset.csv; "
              "skipping mesh_progress.png")
        return False

    by_case: dict[str, list[dict[str, str]]] = {}
    for row in corpus:
        by_case.setdefault(row["part"], []).append(row)

    metrics = [
        ("accuracy_rel_err",
         f"Best {fs.quantity_label('accuracy_rel_err')} found so far",
         f"best {fs.quantity_label('accuracy_rel_err')} so far\n"
         "(lower is better)", "hybrid_zoo"),
        ("geo_fidelity_dist_p99",
         f"Best {fs.quantity_label('geo_p99')} found so far",
         f"best {fs.quantity_label('geo_p99')} so far\n"
         "(model units, lower is better)",
         "graded_tet"),
    ]
    curves: dict[str, list[tuple[np.ndarray, np.ndarray]]] = {
        k: [] for k, _, _, _ in metrics}
    horizons: list[float] = []
    for case, case_rows in sorted(by_case.items()):
        cost = np.array([to_float(r["mesh_ms"]) + to_float(r["solve_ms"])
                         for r in case_rows])
        cost = np.nan_to_num(cost, nan=0.0)
        times = np.cumsum(cost) / 1000.0  # seconds of solver wall time
        if times[-1] <= 0:
            continue
        horizons.append(float(times[-1]))
        for key, _, _, _ in metrics:
            raw = np.array([to_float(r[key]) if r["status"] == OK_STATUS else math.nan
                            for r in case_rows])
            best = _best_so_far(raw)
            if np.isfinite(best).any():
                curves[key].append((times, best))

    if not horizons or not any(curves.values()):
        print("no data yet — corpus rows carry no timing/metric values; "
              "skipping mesh_progress.png")
        return False

    horizon = float(np.median(horizons))
    print("\nmesh_progress.png — anytime curve over "
          f"{len(by_case)} cases, {len(corpus)} corpus rows")
    print(f"  cumulative solver time per case: median {horizon:.0f} s, "
          f"min {min(horizons):.0f} s, max {max(horizons):.0f} s")
    print("  a case's curve is held flat once it runs out of actions, so the "
          "population is fixed across the window")

    # Reduce first, draw second: the subtitle states the measured gain, so it
    # has to be computed from the data before the figure exists.
    reduced: dict[str, dict[str, Any]] = {}
    for key, label, _short, name in metrics:
        case_curves = curves[key]
        if not case_curves:
            continue
        # The window starts once most cases have produced a first result. A
        # case that starts later is dropped rather than allowed to join
        # mid-curve: a changing population would make the median rise even
        # though every individual curve only ever falls.
        firsts = np.array([float(t[np.isfinite(v)][0]) for t, v in case_curves])
        t_lo = float(np.percentile(firsts, START_COVERAGE_PCT))
        kept = [curve for curve, first in zip(case_curves, firsts) if first <= t_lo]
        dropped = len(case_curves) - len(kept)
        t_hi = max(float(t[-1]) for t, _ in kept)
        grid = np.geomspace(t_lo, t_hi, 240)
        stack = np.vstack([_step_sample(t, v, grid) for t, v in kept])
        median = np.median(stack, axis=0)
        reduced[key] = {
            "label": label, "series": name, "grid": grid, "median": median,
            "lo": np.percentile(stack, 25, axis=0),
            "hi": np.percentile(stack, 75, axis=0),
            "kept": len(kept), "dropped": dropped, "t_lo": t_lo, "t_hi": t_hi,
            "gain": (median[0] / median[-1]) if median[-1] > 0 else math.nan,
        }

    gains = " · ".join(
        f"{fs.quantity_label(key)} improves {reduced[key]['gain']:.2f}×"
        for key, _, _, _ in metrics if key in reduced
        and math.isfinite(reduced[key]["gain"]))
    fig, axes = fs.figure(
        "Mesh search over the campaign — best result found so far against "
        "solver time spent",
        subtitle=(f"Over the search window the typical case improves: {gains}. "
                  f"The shaded band covers the middle half of the cases."),
        footer=fs.footer_source(ADVISOR_DIR / "dataset.csv", n=len(corpus),
                                note=f"{len(by_case)} cases"),
        size="wide", ncols=2,
        share_y_axis="the two panels measure different things (a relative "
                     "error against a distance in model units)")

    for ax, (key, label, short, name) in zip(axes[0], metrics):
        stats = reduced.get(key)
        st = fs.series(name)
        if stats is None:
            ax.text(0.5, 0.5, f"no {fs.quantity_label(key)} data yet",
                    ha="center", va="center",
                    transform=ax.transAxes, color=fs.theme().muted)
            fs.axes_off(ax)
            continue
        grid, median = stats["grid"], stats["median"]
        ax.fill_between(grid, stats["lo"], stats["hi"], color=st.color,
                        alpha=0.16, linewidth=0,
                        label="middle half of the cases")
        ax.plot(grid, median, color=st.color, linestyle=st.dash, linewidth=2.2,
                label=f"median over {stats['kept']} cases")
        ax.axvline(horizon, color=fs.theme().rule, linewidth=1.1,
                   linestyle=(0, (1, 2)),
                   label=f"time the median case got ({horizon:.0f} s)")
        ax.set_xscale("log")
        info = fs.loglim(ax, np.concatenate([stats["lo"], stats["hi"]]))
        ax.set_xlabel("total solver time spent on this case  (seconds)")
        ax.set_ylabel(short)
        fs.panel_title(ax, label)
        ax.legend(loc="lower left")
        fs.annotate_n(ax, stats["kept"], excluded=stats["dropped"],
                      what="cases", extra=info.note())
        print(f"  {key}: median {fmt(float(median[0]))} at {stats['t_lo']:.1f} s -> "
              f"{fmt(float(median[-1]))} at {stats['t_hi']:.0f} s  "
              f"({stats['gain']:.2f}x better); "
              f"start IQR [{fmt(float(stats['lo'][0]))}, {fmt(float(stats['hi'][0]))}] -> "
              f"final IQR [{fmt(float(stats['lo'][-1]))}, {fmt(float(stats['hi'][-1]))}]; "
              f"{stats['kept']} cases in the window, {stats['dropped']} started too late")

    save(fig, out_dir, "mesh_progress.png")
    return True


# --- figure 3: accuracy_vs_cost.png -----------------------------------------


def _pareto(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    """Indices of the lower-left Pareto front: minimize both axes."""
    keep: list[int] = []
    best = math.inf
    for index in np.lexsort((y, x)):
        if y[index] < best:
            best = float(y[index])
            keep.append(int(index))
    return np.array(keep, dtype=int)


def accuracy_vs_cost(rows: list[dict[str, str]], out_dir: Path) -> bool:
    every = corpus_rows(rows)
    corpus = [r for r in every if r["status"] == OK_STATUS]
    usable = [r for r in corpus if math.isfinite(to_float(r["accuracy_rel_err"]))]
    if not usable:
        print("no data yet — no successful corpus rows with accuracy_rel_err; "
              "skipping accuracy_vs_cost.png")
        return False
    failed = len(every) - len(corpus)
    no_metric = len(corpus) - len(usable)

    accuracy = np.array([to_float(r["accuracy_rel_err"]) for r in usable])
    meshers = [r["mesher"] for r in usable]
    panels = [("n_dof", f"{fs.quantity_label('n_dof')}  (the unknowns the "
                        "solver has to solve for)"),
              ("solve_ms", f"{fs.quantity_label('solve_ms')}  (milliseconds)")]

    print(f"\naccuracy_vs_cost.png — {len(usable)} successful corpus rows "
          f"({failed} failed meshes and {no_metric} rows without a metric "
          f"excluded from {len(every)})")
    fig, axes = fs.figure(
        "Accuracy against cost for every mesh in the data set",
        subtitle="Every dot is one mesh from the campaign. The heavy line is "
                 "each mesher's median within a band of cost, so the crowd of "
                 "dots cannot hide which mesher owns which region.",
        footer=fs.footer_source(ADVISOR_DIR / "dataset.csv", n=len(every)),
        size="wide", ncols=2)

    for ax, (key, xlabel) in zip(axes[0], panels):
        cost = np.array([to_float(r[key]) for r in usable])
        keep = np.isfinite(cost) & (cost > 0) & (accuracy > 0)
        for mesher in sorted(set(meshers)):
            sel = keep & np.array([m == mesher for m in meshers])
            if not sel.any():
                continue
            st = fs.series(mesher)
            # A thin, small marker cloud plus a binned median: 2,000 points at
            # alpha on top of each other hid which mesher owned which region.
            ax.scatter(cost[sel], accuracy[sel], s=7, alpha=0.22,
                       marker=st.marker, color=st.color, edgecolors="none",
                       zorder=2)
            edges = np.geomspace(cost[sel].min(), cost[sel].max(), 9)
            centres, medians = [], []
            for lo_edge, hi_edge in zip(edges[:-1], edges[1:]):
                bucket = accuracy[sel & (cost >= lo_edge) & (cost < hi_edge)]
                if bucket.size >= 3:
                    centres.append(math.sqrt(lo_edge * hi_edge))
                    medians.append(float(np.median(bucket)))
            if centres:
                ax.plot(centres, medians, color=st.color, linestyle=st.dash,
                        marker=st.marker, markersize=5.5, linewidth=2.2,
                        markeredgecolor=fs.theme().bg, markeredgewidth=0.6,
                        zorder=4,
                        label=f"{st.label}  median  (n={int(sel.sum()):,})")
            else:
                ax.plot([], [], color=st.color, linestyle=st.dash,
                        marker=st.marker, label=f"{st.label}  (n={int(sel.sum()):,})")

        front = _pareto(cost[keep], accuracy[keep])
        kept_rows = [row for row, flag in zip(usable, keep) if flag]
        px, py = cost[keep][front], accuracy[keep][front]
        owners = [kept_rows[index]["part"] for index in front]
        ax.plot(px, py, color=fs.theme().ink, linewidth=1.5,
                drawstyle="steps-post", zorder=5,
                label=f"best trade-offs available  ({len(px)} "
                      f"mesh{'' if len(px) == 1 else 'es'})")
        ax.scatter(px, py, s=26, facecolors="none", edgecolors=fs.theme().ink,
                   linewidths=1.1, zorder=6)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel(xlabel)
        # The front is a staircase along the lower-left, exactly where an
        # unframed key would sit on top of it. Give the key an opaque card so
        # the front reads as passing behind it rather than through it.
        legend = ax.legend(loc="lower left", frameon=True, framealpha=0.94)
        legend.get_frame().set_facecolor(fs.theme().panel)
        legend.get_frame().set_edgecolor(fs.theme().rule)
        # The key now carries the spelled-out mesher names, so the card is
        # wider than the identifiers made it and reaches the front's markers.
        # Lift it over them, which is what "passing behind it" already claimed.
        legend.set_zorder(7)
        fs.annotate_n(ax, int(keep.sum()), excluded=failed + no_metric,
                      what="meshes")

        print(f"  vs {key}: {int(keep.sum())} points, "
              f"{key} {fmt(float(cost[keep].min()))} .. {fmt(float(cost[keep].max()))}, "
              f"rel_err {fmt(float(accuracy[keep].min()))} .. "
              f"{fmt(float(accuracy[keep].max()))}")
        print(f"    Pareto front ({len(px)} point{'' if len(px) == 1 else 's'}, "
              f"{len(set(owners))} distinct case"
              f"{'' if len(set(owners)) == 1 else 's'}): "
              + ", ".join(f"({fmt(a, 3)}, {fmt(b, 3)}) {part}"
                          for a, b, part in zip(px, py, owners)))
        # rel_err carries a per-case reference-truth offset, so a single lucky
        # case can dominate the pooled cloud outright. Say so on the figure
        # rather than letting a one-point front read as a plotting bug.
        if len(set(owners)) <= 2:
            note = (f"every point on this line comes from "
                    f"{', '.join(sorted(set(owners)))} —\n"
                    f"{fs.quantity_label('accuracy_rel_err')} cannot be "
                    "compared between cases,\nwhich is why the network also "
                    f"learns {fs.quantity_label('rel_err_rel')}")
            fs.assert_glyphs(note)
            ax.text(0.02, 0.02, note, transform=ax.transAxes, va="bottom",
                    ha="left", fontsize=fs.FONT_PT["annot"] - 0.5,
                    color=fs.theme().muted, style="italic")

    axes[0][0].set_ylabel(
        f"{fs.quantity_label('accuracy_rel_err')}  (lower is better)")
    save(fig, out_dir, "accuracy_vs_cost.png")
    return True


# --- figure 4: fidelity_vs_h.png --------------------------------------------


def fidelity_vs_h(rows: list[dict[str, str]], out_dir: Path) -> bool:
    usable = [r for r in corpus_rows(rows)
              if math.isfinite(to_float(r["h_rel"]))
              and (math.isfinite(to_float(r["geo_fidelity_chamfer_mean"]))
                   or math.isfinite(to_float(r["geo_fidelity_dist_p99"])))]
    if not usable:
        print("no data yet — no rows with h_rel and geometric fidelity; "
              "skipping fidelity_vs_h.png")
        return False

    metrics = [("geo_fidelity_chamfer_mean",
                fs.quantity_label("geo_fidelity_chamfer_mean")),
               ("geo_fidelity_dist_p99",
                fs.quantity_label("geo_fidelity_dist_p99"))]
    orders = sorted({int(to_float(r["order"])) for r in usable
                     if math.isfinite(to_float(r["order"]))})

    print(f"\nfidelity_vs_h.png — {len(usable)} rows with geometric fidelity")
    print(f"  precision floor {PRECISION_FLOOR:g} model units: on most "
          "surfaces the boundary residual is at machine precision, and a raw "
          "log axis over those values shows nothing")
    fig, axes = fs.figure(
        "How closely the mesh follows the CAD surface, against cell size, "
        "split by element order",
        subtitle="Each marker is the median at one cell size, and the bars "
                 "span the middle half of the runs. Values sitting on the "
                 "floor line are at the limit of what the arithmetic can "
                 "measure, not zero error.",
        footer=fs.footer_source(ADVISOR_DIR / "dataset.csv", n=len(usable)),
        size="wide", ncols=2,
        share_y_axis="the average distance and the worst 1% are two different "
                     "summaries of the same set of distances")

    for ax, (key, label) in zip(axes[0], metrics):
        all_levels = sorted({round(to_float(r["h_rel"]), 4) for r in usable
                             if math.isfinite(to_float(r[key]))})
        column = np.array([to_float(r[key]) for r in usable
                           if math.isfinite(to_float(r[key]))])
        # Floor first, then draw: the axis limits come from the floored data so
        # a handful of machine-precision residuals cannot stretch the panel
        # over sixteen empty decades and flatten every real trend.
        info = fs.loglim(ax, column, floor=PRECISION_FLOOR)
        rates: list[str] = []
        for index, order in enumerate(orders):
            sel = [r for r in usable
                   if math.isfinite(to_float(r["order"]))
                   and int(to_float(r["order"])) == order
                   and math.isfinite(to_float(r[key]))]
            if not sel:
                continue
            st = fs.series(f"order {order}")
            # nudge each order sideways: fidelity is a surface property, so
            # the orders land on top of each other without an offset.
            nudge = math.exp((index - (len(orders) - 1) / 2) * 0.02)
            hs = np.array([to_float(r["h_rel"]) for r in sel])
            vs = fs.clamp_to_floor([to_float(r[key]) for r in sel],
                                   info.floor)
            ax.scatter(hs * nudge, vs, s=9, alpha=0.18, marker=st.marker,
                       color=st.color, edgecolors="none", zorder=2)
            levels = sorted({round(float(h), 4) for h in hs})
            med, q25, q75 = [], [], []
            for level in levels:
                bucket = vs[np.isclose(hs, level, rtol=1e-3)]
                med.append(float(np.median(bucket)))
                q25.append(float(np.percentile(bucket, 25)))
                q75.append(float(np.percentile(bucket, 75)))
                at_floor = int((bucket <= info.floor).sum())
                print(f"  {key} order {order} h_rel {level:g}: n={bucket.size} "
                      f"median {fmt(med[-1])} IQR [{fmt(q25[-1])}, {fmt(q75[-1])}]"
                      + (f"  ({at_floor} at the precision floor)" if at_floor else ""))
            fit = fs.fit_loglog(levels, med)
            if fit.reportable:
                # The rate belongs with the other measured text, not in the
                # legend: as a legend suffix it doubled the key's width and
                # left no corner free for the precision note.
                rates.append(f"order {order}: rate cell size^{fit.slope:.2f} "
                             f"(fit quality {fit.residual:.2f})")
            ax.errorbar(np.array(levels) * nudge, med,
                        yerr=[np.array(med) - np.array(q25),
                              np.array(q75) - np.array(med)],
                        color=st.color, marker=st.marker, markersize=6,
                        linewidth=2.2, linestyle=st.dash, capsize=4, zorder=4,
                        markeredgecolor=fs.theme().bg, markeredgewidth=0.6,
                        label=f"order {order}  (n={len(sel):,})")
            if fit.reportable:
                print(f"    order {order}: measured rate h^{fit.slope:.2f} "
                      f"(r² = {fit.residual:.3f}) over {len(levels)} resolutions")
            elif len(levels) >= 2 and med[0] > 0:
                print(f"    order {order}: only {len(levels)} resolutions — "
                      "too few for a rate; coarsest "
                      f"{levels[-1]:g} -> finest {levels[0]:g} improves {key} "
                      f"by {med[-1] / med[0]:.2f}x")
        ax.set_xscale("log")
        ax.set_xticks(all_levels)
        ax.set_xticklabels([f"{level:g}" for level in all_levels])
        ax.minorticks_off()
        ax.set_xmargin(0.25)
        # Ascending h_rel, left to right. The old panel put decreasing numbers
        # under a "finer →" arrow, which reads as a reversed axis.
        ax.set_xlabel(f"{fs.quantity_label('h_rel')}"
                      "  —  finer on the left")
        ax.set_ylabel(f"{label}  (model units)")
        fs.panel_title(ax, f"{label} against cell size")
        ax.legend(loc="upper left")
        corner = rates + ([info.note("at machine precision")]
                          if info.clamped else [])
        if corner:
            ax.text(0.985, 0.97, "\n".join(corner),
                    transform=ax.transAxes, ha="right", va="top",
                    fontsize=fs.FONT_PT["annot"] - 1.0,
                    color=fs.theme().muted, linespacing=1.3)

    save(fig, out_dir, "fidelity_vs_h.png")
    return True
