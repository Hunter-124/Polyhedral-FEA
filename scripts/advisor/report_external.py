# SPDX-License-Identifier: BSD-3-Clause
"""``external_comparison.png``: external mesh sources against native PolyMesh."""
from __future__ import annotations

import math
import textwrap
from pathlib import Path
from typing import Any, Sequence

import numpy as np
from matplotlib.lines import Line2D
from matplotlib.patches import Patch, Rectangle

import figstyle as fs

from .paths import REFERENCE_DIR
from .report_common import OK_STATUS, family_of, load_json, save, tint, to_float

CORPUS_REFERENCE_DIR = REFERENCE_DIR / "corpus"

#: Four mesh sources now, and the fourth is the point: ``uniform-p2`` promotes
#: every element, so it is the only native variant that is order-for-order
#: comparable with Gmsh's uniformly quadratic tet10 meshes.
EXTERNAL_SOURCES = ["gmsh-mesh+polymesh-solver", "polymesh-native",
                    "polymesh-native-graded", "polymesh-native-uniform-p2"]
#: slot 2 (bluish green) is free in the mesher palette; pinning it here keeps
#: the new variant the same colour+marker+dash wherever it is drawn next.
fs.register_series("polymesh-native-uniform-p2", 2,
                   label="native, uniformly quadratic")


def _external_tolerance(case_id: str, metric_name: str) -> float | None:
    reference = load_json(CORPUS_REFERENCE_DIR / f"{case_id}.json")
    if not isinstance(reference, dict):
        return None
    base_name = metric_name.removesuffix("_rel_err")
    for metric in reference.get("metrics", []):
        if metric.get("name") != base_name:
            continue
        tolerance = to_float(metric.get("tol"))
        return tolerance if tolerance > 0.0 else None
    return None


#: Outcome states for the per-panel strip, in legend order. A refusal is the
#: mesher declining an h it cannot represent — an engineering answer, and the
#: plurality outcome in this matrix — so it is drawn as its own informative
#: state: never a gap in a line, never a failure. ``failed`` stays visually
#: separate because that distinction is real upstream. Each state carries a
#: hatch as well as a hue, so the strip survives greyscale printing.
OUTCOME_ORDER = ["measured", "refused", "failed", "timeout"]
OUTCOME_LABELS = {
    "measured": "measured",
    "refused": "refused — cell size declined",
    "failed": "failed — real error",
    "timeout": "ran out of time",
}


def _outcome_cell(state: str) -> tuple[str, str, str]:
    """(face, edge, hatch) for one outcome cell, from the active theme."""
    t = fs.theme()
    return {
        "measured": (tint(t.ink, 0.22), t.ink, ""),
        "refused": (tint(t.warn, 0.62), t.warn, "///"),
        "failed": (tint(t.bad, 0.35), t.bad, "xxx"),
        "timeout": (tint(t.muted, 0.72), t.muted, "..."),
    }[state]


def _external_rows(payload: list[Any]) -> list[dict[str, Any]]:
    """Every peer row, classified — refusals and failures kept, not dropped.

    The old filter required a finite ``accuracy.value``, which silently threw
    away 166 of 336 rows and made a refusal indistinguishable from a run that
    never happened.
    """
    out: list[dict[str, Any]] = []
    for row in payload:
        if not isinstance(row, dict) or row.get("solver") not in EXTERNAL_SOURCES:
            continue
        case_id = row.get("case_id")
        order = row.get("order")
        h_rel = to_float(row.get("h_rel"))
        if (not isinstance(case_id, str) or not isinstance(order, int)
                or not math.isfinite(h_rel) or h_rel <= 0):
            continue
        status = str(row.get("status", ""))
        dofs = to_float(row.get("dofs"))
        error = to_float((row.get("accuracy") or {}).get("value"))
        metric = str((row.get("accuracy") or {}).get("name", ""))
        tolerance = _external_tolerance(case_id, metric)
        measured = (status == OK_STATUS and math.isfinite(dofs) and dofs > 0
                    and math.isfinite(error) and error >= 0.0)
        if measured:
            state = "measured"
        elif status == "refused":
            state = "refused"
        elif status == "timeout":
            state = "timeout"
        else:
            # Anything else produced no number without the engine declining,
            # which is an error — including an ``ok`` row whose metric is
            # missing. Calling that a refusal would launder a defect.
            state = "failed"
        refusal = row.get("refusal")
        refusal = refusal if isinstance(refusal, dict) else {}
        diagnosis = row.get("diagnosis")
        diagnosis = diagnosis if isinstance(diagnosis, dict) else {}
        promotion = row.get("promotion")
        promotion = promotion if isinstance(promotion, dict) else {}
        out.append({
            "solver": row["solver"],
            "case_id": case_id,
            "family": family_of(case_id),
            "order": order,
            "h_rel": round(h_rel, 4),
            "state": state,
            "dofs": dofs,
            "error": error,
            "metric": metric,
            "tolerance": tolerance,
            # error in units of the case's own tolerance: 1.0 is the pass line
            "ratio": (error / tolerance) if (measured and tolerance) else math.nan,
            "refusal_kind": str(refusal.get("kind", "")),
            "recommended_h": to_float(refusal.get("recommended_h_m")),
            "diagnosis_kind": str(diagnosis.get("kind", "")),
            "order_pairing": str(promotion.get("order_pairing") or ""),
        })
    return out


def _rung_note(cells: list[dict[str, Any]]) -> list[str]:
    """Two label lines for one h_rel group: how many declined, and the h to use.

    Always two lines, empty when nothing declined, so every strip's tick
    labels are the same height and the row gaps stay even.
    """
    refused = [row for row in cells if row["state"] == "refused"]
    if not refused:
        return ["none declined", ""]
    recommended = [row["recommended_h"] for row in refused
                   if math.isfinite(row["recommended_h"]) and row["recommended_h"] > 0]
    silent = len(refused) - len(recommended)
    head = f"{len(refused)} declined" + (f" ({silent} no size)" if silent else "")
    if not recommended:
        # the fill-stage guard refuses without naming a size; saying so beats
        # inventing one.
        return [head, "no size stated"]
    return [head, f"cell size ≤ {min(recommended):.3g} m"]


def _outcome_strip(ax: Any, panel: list[dict[str, Any]], variants: list[str],
                   cases: list[str], rungs: list[float],
                   label_variants: bool) -> list[str]:
    """One cell per (mesh source, case, cell size): measured / refused / failed.

    Drawn as its own thin axes under the convergence panel rather than as
    markers inside it: the convergence panel's x-axis is DOF, and a refusal
    has no DOF at all, so it cannot honestly be placed there.
    """
    t = fs.theme()
    width = len(rungs) * len(cases)
    by_key = {(row["solver"], row["case_id"], row["h_rel"]): row for row in panel}
    for column, (rung_index, case_index) in enumerate(
            (r, c) for r in range(len(rungs)) for c in range(len(cases))):
        for variant_index, variant in enumerate(variants):
            row = by_key.get((variant, cases[case_index], rungs[rung_index]))
            y = len(variants) - 1 - variant_index
            if row is None:
                ax.add_patch(Rectangle((column + 0.08, y + 0.12), 0.84, 0.76,
                                       facecolor="none", edgecolor=t.grid,
                                       linewidth=0.6, zorder=2))
                continue
            face, edge, hatch = _outcome_cell(row["state"])
            ax.add_patch(Rectangle((column + 0.08, y + 0.12), 0.84, 0.76,
                                   facecolor=face, edgecolor=edge, hatch=hatch,
                                   linewidth=0.7, zorder=2))
    for rung_index in range(1, len(rungs)):
        ax.axvline(rung_index * len(cases), color=t.rule, linewidth=0.9,
                   zorder=3)
    ax.set_xlim(0, width)
    ax.set_ylim(0, len(variants))
    ax.set_xticks([(index + 0.5) * len(cases) for index in range(len(rungs))])
    notes = []
    labels = []
    for rung in rungs:
        cells = [row for row in panel if row["h_rel"] == rung]
        note = _rung_note(cells)
        notes.append(f"h_rel {rung:g}: "
                     + ", ".join(line for line in note if line))
        labels.append("\n".join([f"cell size {rung:g} of the part", *note]))
    ax.set_xticklabels(labels, fontsize=fs.FONT_PT["annot"] - 2.5,
                       color=fs.theme().muted, linespacing=1.4)
    ax.tick_params(axis="x", length=0)
    ax.set_yticks([index + 0.5 for index in range(len(variants))])
    if label_variants:
        ax.set_yticklabels([fs.series(variant).label
                            for variant in reversed(variants)],
                           fontsize=fs.FONT_PT["annot"] - 1.0)
    else:
        ax.set_yticklabels([])
    ax.tick_params(axis="y", length=0)
    for side in ax.spines.values():
        side.set_visible(False)
    ax.set_axisbelow(True)
    return notes


def _place_rung_labels(fig: Any, panels: Sequence[tuple[Any, Sequence[
        tuple[float, float, str, str]]]], fontsize: float) -> int:
    """Label each point with its cell size, choosing offsets that do not collide.

    Several mesh variants land on nearly the same (unknowns, error as a
    multiple of the allowance) in these panels, so a fixed offset rule always
    buried one label under another. Each label is instead tried against the
    ones already placed, in axes-fraction space, and takes the first free
    spot. Returns how many labels found no free spot, so the figure never
    claims a placement it did not get.
    """
    # tight_layout here only to read the near-final axes geometry; figstyle's
    # finish() lays the figure out again with its own rect before saving.
    try:
        fig.tight_layout()
    except Exception:
        pass
    candidates = [(5.0, 4.0), (-6.0, 4.0), (5.0, -11.0), (-6.0, -11.0),
                  (5.0, 15.0), (-6.0, 15.0), (5.0, -22.0), (-6.0, -22.0),
                  (5.0, 26.0), (-6.0, 26.0), (5.0, -33.0), (-6.0, -33.0)]
    crowded = 0
    for ax, entries in panels:
        box = ax.get_position()
        width_pt = box.width * fig.get_size_inches()[0] * 72.0
        height_pt = box.height * fig.get_size_inches()[1] * 72.0
        x_lo, x_hi = (math.log10(value) for value in ax.get_xlim())
        y_lo, y_hi = (math.log10(value) for value in ax.get_ylim())
        placed: list[tuple[float, float, float, float]] = []
        for x, y, text, color in entries:
            # 0.58 em per character is the measured average for this font at
            # label sizes; exact metrics need a renderer we do not have yet.
            w = 0.58 * fontsize * len(text) / width_pt
            h = 1.25 * fontsize / height_pt
            fx = (math.log10(x) - x_lo) / (x_hi - x_lo)
            fy = (math.log10(y) - y_lo) / (y_hi - y_lo)
            chosen = None
            for dx, dy in candidates:
                left = fx + (dx / width_pt if dx > 0 else (dx / width_pt) - w)
                bottom = fy + (dy / height_pt if dy > 0
                               else (dy / height_pt) - h)
                if left < 0.01 or left + w > 0.99:
                    continue
                if bottom < 0.01 or bottom + h > 0.99:
                    continue
                if any(left < px + pw and px < left + w
                       and bottom < py + ph and py < bottom + h
                       for px, py, pw, ph in placed):
                    continue
                chosen = (dx, dy, left, bottom)
                break
            if chosen is None:
                crowded += 1
                continue
            dx, dy, left, bottom = chosen
            placed.append((left, bottom, w, h))
            ax.annotate(text, (x, y), xytext=(dx, dy),
                        textcoords="offset points",
                        ha="left" if dx > 0 else "right",
                        fontsize=fontsize, color=color, zorder=5)
    return crowded


def _order_tally(measured: list[dict[str, Any]], families: list[str],
                 order: int) -> list[tuple[str, str, float, float]]:
    """(family, best native variant, its median relative error, Gmsh's median)
    per family."""
    out = []
    for family in families:
        panel = [row for row in measured
                 if row["family"] == family and row["order"] == order]
        peer = [row["error"] for row in panel
                if row["solver"] == "gmsh-mesh+polymesh-solver"]
        best: tuple[str, float] | None = None
        for solver in EXTERNAL_SOURCES:
            if solver == "gmsh-mesh+polymesh-solver":
                continue
            errors = [row["error"] for row in panel if row["solver"] == solver]
            if not errors:
                continue
            median = float(np.median(errors))
            if best is None or median < best[1]:
                best = (solver, median)
        if not peer or best is None:
            continue
        out.append((family, best[0], best[1], float(np.median(peer))))
    return out


def _cost_tally(measured: list[dict[str, Any]], families: list[str],
                order: int) -> list[tuple[str, float, float]]:
    """(family, best native median relative error x degrees of freedom, Gmsh's
    median) per family.

    The accuracy tally answers "whose mesh is more accurate". It does not
    answer "at what cost", and on this matrix the two answers differ: the
    native wins are bought with 4-10x the degrees of freedom. Charging for
    them is one multiplication, so there is no excuse for showing only the
    flattering basis.
    """
    out = []
    for family in families:
        panel = [row for row in measured
                 if row["family"] == family and row["order"] == order
                 and row.get("dofs")]
        peer = [row["error"] * row["dofs"] for row in panel
                if row["solver"] == "gmsh-mesh+polymesh-solver"]
        native = [row["error"] * row["dofs"] for row in panel
                  if row["solver"] != "gmsh-mesh+polymesh-solver"]
        best: float | None = None
        for solver in EXTERNAL_SOURCES:
            if solver == "gmsh-mesh+polymesh-solver":
                continue
            costs = [row["error"] * row["dofs"] for row in panel
                     if row["solver"] == solver]
            if not costs:
                continue
            median = float(np.median(costs))
            if best is None or median < best:
                best = median
        if not peer or best is None or not native:
            continue
        out.append((family, best, float(np.median(peer))))
    return out


def external_comparison(result_path: Path, out_dir: Path) -> bool:
    payload = load_json(result_path)
    if not isinstance(payload, list):
        print(f"no data yet — expected a JSON row array at {result_path}; "
              "skipping external_comparison.png")
        return False

    rows = _external_rows(payload)
    measured = [row for row in rows if row["state"] == "measured"]
    plotted = [row for row in measured if math.isfinite(row["ratio"])]
    if not plotted:
        print(f"no data yet — no measured external-comparison rows in "
              f"{result_path}; skipping external_comparison.png")
        return False

    preferred = ("box_hole", "stepped_shaft")
    present_families = {row["family"] for row in rows}
    families = [family for family in preferred if family in present_families]
    families.extend(sorted(present_families - set(families)))
    orders = sorted({row["order"] for row in rows})
    rungs = sorted({row["h_rel"] for row in rows}, reverse=True)
    states = {state: sum(row["state"] == state for row in rows)
              for state in OUTCOME_ORDER}
    tolerances = sorted({row["tolerance"] for row in rows
                         if row["tolerance"] is not None})

    print(f"\nexternal_comparison.png — {len(rows)} peer rows from "
          f"{result_path}: " + ", ".join(f"{state} {count}"
                                         for state, count in states.items()))
    print(f"  reference tolerances span {min(tolerances):g}–"
          f"{max(tolerances):g} (median "
          f"{float(np.median(tolerances)):g}) across {len(tolerances)} "
          "distinct values")

    tallies = {order: _order_tally(measured, families, order)
               for order in orders}
    wins = {order: sum(1 for _, _, native, peer in tallies[order]
                       if native < peer) for order in orders}
    for order in orders:
        for family, solver, native, peer in tallies[order]:
            verdict = "ours lower" if native < peer else "Gmsh lower"
            print(f"  order {order} {family}: best native "
                  f"{fs.series(solver).label} median relative error "
                  f"{native:.4f} vs Gmsh mesh {peer:.4f} — {verdict}")
        print(f"  order {order} tally: ours lower in {wins[order]}/"
              f"{len(tallies[order])} families")

    order1 = orders[0]
    reversal = ("; ".join(f"{family} {native:.4f} vs {peer:.4f}"
                          for family, _, native, peer in tallies[order1]))

    cost = _cost_tally(measured, families, order1)
    cost_wins = sum(1 for _, native, peer in cost if native < peer)
    for family, native, peer in cost:
        print(f"  order {order1} {family} relative error x degrees of "
              f"freedom: ours {native:.0f} vs Gmsh {peer:.0f} — "
              + ("ours lower" if native < peer else "Gmsh lower"))
    print(f"  order {order1} cost tally: ours lower in {cost_wins}/"
          f"{len(cost)} families on relative error x degrees of freedom")
    cost_sentence = ""
    if cost:
        losers = ", ".join(family for family, native, peer in cost
                           if native >= peer)
        cost_sentence = (
            " Those wins are bought with extra unknowns to solve for: on the "
            f"median {fs.quantity_label('efficiency')} ours is lower in only "
            f"{cost_wins}/{len(cost)} families"
            + (f", with Gmsh more economical on {losers}" if losers else "")
            + " — more accurate per case is not the same as cheaper per case.")
    findings = REFERENCE_DIR / "external" / "external-truth-findings.json"
    # the parity vocabulary is read off promotion.order_pairing, so no caveat
    # in this figure can outlive the variant that fixed it.
    true_parity_labels = sorted({fs.series(row["solver"]).label for row in rows
                                 if "TRUE PARITY" in row["order_pairing"]})
    approx_labels = sorted({fs.series(row["solver"]).label for row in rows
                            if "APPROXIMATE" in row["order_pairing"]})
    parity_sentence = ""
    if true_parity_labels:
        parity_sentence = (
            " Each row states its own order pairing "
            f"(promotion.order_pairing): {', '.join(true_parity_labels)} is a "
            "TRUE PARITY match against Gmsh's uniformly quadratic 10-node "
            "tetrahedra (tet10), where every element is promoted.")
        if approx_labels:
            parity_sentence += (f" {', '.join(approx_labels)} is only "
                                "APPROXIMATE — promotion lifts a marked "
                                "subset of the elements, not all of them.")

    # error/tolerance, not raw rel_err: tolerances are now 0.02–0.087 while
    # errors run 0.004–0.8, so a shaded band on a linear axis is a hairline at
    # the axis floor. Dividing by each case's own tolerance puts the pass line
    # at exactly 1.0 in every panel and makes panels with different tolerances
    # directly comparable, which no shared linear y-axis can do. The axis stays
    # logarithmic because the ratios still span three decades.
    # A tolerance-normalised axis invites exactly one question — how many
    # configurations are actually inside tolerance — so answer it instead of
    # leaving the reader to count markers in the shaded band.
    ratios = [row["error"] / row["tolerance"] for row in rows
              if row.get("state") == "measured" and row.get("tolerance")]
    inside = sum(1 for r in ratios if r <= 1.0)
    pass_sentence = ""
    if ratios:
        best = min(ratios)
        pass_sentence = (
            f" {inside} of the {len(ratios)} measured runs are inside the "
            f"allowed error, and the closest is {best:.2f}× the allowance, so "
            "at these cell sizes neither mesh source reaches reference "
            "accuracy on most cases — the comparison is which source is "
            "closer, not which one passes.")
    subtitle = (
        "The height of each point is how far that mesh is from the reference "
        "answer, as a multiple of the error that case is allowed, so 1.0 is "
        "the pass line in every panel and cases with different allowances "
        f"(now {min(tolerances):g}–{max(tolerances):g}, once a hand-picked "
        "0.15) can be read side by side. The strip under each panel has one "
        "cell per mesh source, case and cell size: a refusal is the mesher "
        f"declining a cell size it cannot build ({states['refused']} of "
        f"{len(rows)} rows, the most common outcome), and it is shown as an "
        "outcome rather than as a gap; a failure is a real error "
        f"({states['failed']}) and stays separate."
        + parity_sentence + pass_sentence
        + f" At order {order1} our best native variant now has the lower "
        f"median relative error in {wins[order1]}/{len(tallies[order1])} "
        f"families ({reversal}), REVERSING the previous regeneration — those "
        "earlier numbers were measured on an engine that silently deleted the "
        "bore, so this is a corrected measurement and not a method "
        "improvement (see external-truth-findings.json and "
        "docs/validation/figures/hole_aliasing.png)."
        + cost_sentence)

    # two axes rows per element order: the convergence panel and its outcome
    # strip. Width grows with the family count; the height is generous because
    # four axes rows plus three-line strip labels need the room, and is held at
    # or above width / MAX_ASPECT so the grid can never letterbox.
    width = 4.7 * len(families)
    height = max(5.6 * len(orders), width / fs.MAX_ASPECT)
    fig, axes = fs.figure(
        "Mesh source against third-party reference answers — error as a "
        "multiple of the error each case is allowed",
        subtitle=subtitle,
        footer=fs.footer_source(result_path, CORPUS_REFERENCE_DIR, findings,
                                n=len(rows)),
        size=(width, height),
        nrows=2 * len(orders), ncols=len(families),
        share_y_axis="the outcome strips are pictures of categories and share "
                     "nothing with the panels above them; those panels are "
                     "put on one common axis explicitly below",
        gridspec_kw={"height_ratios": [3.0, 1.5] * len(orders)})

    shown_solvers: list[str] = []
    chart_axes = []
    deferred_labels: list[tuple[Any, list[tuple[float, float, str, str]]]] = []
    for order_index, order in enumerate(orders):
        for family_index, family in enumerate(families):
            ax = axes[2 * order_index][family_index]
            strip_ax = axes[2 * order_index + 1][family_index]
            panel = [row for row in rows
                     if row["family"] == family and row["order"] == order]
            if not panel:
                ax.set_visible(False)
                strip_ax.set_visible(False)
                continue
            chart_axes.append(ax)

            fs.tolerance_band(ax, 1.0, label="pass line = 1.0")

            rung_labels: list[tuple[float, float, str, str]] = []
            variants = [solver for solver in EXTERNAL_SOURCES
                        if any(row["solver"] == solver for row in panel)]
            for solver in variants:
                source_rows = [row for row in panel
                               if row["solver"] == solver
                               and row["state"] == "measured"
                               and math.isfinite(row["ratio"])]
                if not source_rows:
                    continue
                points = []
                for h_rel in sorted({row["h_rel"] for row in source_rows},
                                    reverse=True):
                    rung = [row for row in source_rows if row["h_rel"] == h_rel]
                    points.append((
                        float(np.median([row["dofs"] for row in rung])),
                        float(np.median([row["ratio"] for row in rung])),
                        h_rel,
                    ))
                points.sort(key=lambda point: point[0])
                xs = [point[0] for point in points]
                ys = [point[1] for point in points]
                st = fs.series(solver)
                fit = fs.fit_loglog(xs, ys)
                if fit.reportable:
                    ax.plot(xs, ys, color=st.color, linestyle=st.dash,
                            linewidth=2.0, zorder=2)
                ax.scatter(xs, ys, marker=st.marker, s=52, color=st.color,
                           edgecolor=fs.theme().bg, linewidth=0.7, zorder=3)
                merged: list[tuple[float, float, list[float]]] = []
                for x, y, h_rel in points:
                    if merged and math.isclose(merged[-1][0], x, rel_tol=0.02):
                        merged[-1][2].append(h_rel)
                    else:
                        merged.append((x, y, [h_rel]))
                for x, y, hs in merged:
                    rung_labels.append((x, y, "/".join(f"{h:g}"
                                                       for h in sorted(hs)),
                                        st.color))
                if solver not in shown_solvers:
                    shown_solvers.append(solver)
                rate = (f"; rate degrees of freedom^{fit.slope:.2f}"
                        if fit.reportable
                        else f"; {len(points)} resolution(s) — no rate stated")
                print(f"  {family} order {order} {st.label}: "
                      + ", ".join(
                          f"h={h_rel:g} median(dof={dofs:.0f}, "
                          f"err/tol={ratio:.3g})"
                          for dofs, ratio, h_rel in points) + rate)

            panel_ratios = [row["ratio"] for row in panel
                            if math.isfinite(row["ratio"])]
            if panel_ratios:
                fs.loglim(ax, panel_ratios + [1.0], draw_floor=False)
            # room on the right so the last point does not land on the
            # tolerance-line caption, which figstyle pins to the right edge
            ax.set_xscale("log")
            ax.autoscale_view()
            x_lo, x_hi = ax.get_xlim()
            ax.set_xlim(x_lo / 1.2, x_hi * 4.0)

            # the h_rel labels are placed after the shared y-range is fixed,
            # by _place_rung_labels: several variants land on nearly the same
            # (dof, ratio) in these panels, so the offsets have to be chosen
            # against the other labels rather than by a fixed rule.
            deferred_labels.append((ax, rung_labels))

            family_label = {
                "box_hole": "Box with a hole (stress concentration)",
                "stepped_shaft": "Stepped-shaft tip deflection",
            }.get(family, family.replace("_", " ").title())
            # the order-2 parity claim is read off promotion.order_pairing, so
            # the blanket "approx. native parity" caveat can no longer outlive
            # the variant that fixed it.
            parity = []
            true_parity = sorted({fs.series(row["solver"]).label
                                  for row in panel
                                  if "TRUE PARITY" in row["order_pairing"]})
            approximate = sorted({fs.series(row["solver"]).label
                                  for row in panel
                                  if "APPROXIMATE" in row["order_pairing"]})
            if true_parity:
                parity.append(f"true parity: {', '.join(true_parity)} "
                              "vs Gmsh's quadratic tetrahedra")
            if approximate:
                parity.append(f"approximate parity: {', '.join(approximate)}")
            # one title line in every panel: a multi-line left-aligned title
            # forces tight_layout to open the same gap between every axes row,
            # which pushed each outcome strip away from the panel it belongs
            # to. The parity statement therefore lives inside the axes.
            fs.panel_title(ax, f"{family_label} · order {order}")
            if parity:
                ax.text(0.985, 0.985, "\n".join(
                    line for bit in parity for line in textwrap.wrap(bit, 44)),
                    transform=ax.transAxes, ha="right", va="top",
                    fontsize=fs.FONT_PT["annot"] - 0.5, color=fs.theme().ink,
                    linespacing=1.35, zorder=6)
            ax.set_xlabel(f"active {fs.quantity_label('n_dof')}  (log scale)\n"
                          "the unknowns the solver has to solve for")
            if family_index == 0:
                ax.set_ylabel(
                    f"{fs.quantity_label('accuracy_rel_err')}, as a multiple "
                    "of\nthe error allowed (1 or less passes; log scale)")
            # lower right: inside the shaded pass region, which no series
            # reaches on its right-hand side in any panel.
            fs.annotate_n(ax, sum(row["state"] == "measured" for row in panel),
                          excluded=sum(row["state"] != "measured"
                                       for row in panel),
                          what="measured", loc="lower right",
                          extra="excluded rows are in the strip below")

            cases = sorted({row["case_id"] for row in panel})
            notes = _outcome_strip(strip_ax, panel, variants, cases,
                                   [rung for rung in rungs
                                    if any(row["h_rel"] == rung
                                           for row in panel)],
                                   label_variants=family_index == 0)
            print(f"  {family} order {order} outcomes: "
                  + " | ".join(notes))

    # every panel is now in the same unit (multiples of tolerance), so one
    # common y-range across the whole figure is the honest treatment.
    fs.share_y(chart_axes)

    crowded = _place_rung_labels(fig, deferred_labels,
                                 fs.FONT_PT["annot"] - 1.5)
    if crowded:
        print(f"  {crowded} cell-size label(s) had no free spot and were left "
              "off the figure; the printed medians above carry those sizes")

    handles = fs.series_handles(shown_solvers)
    handles.append(Line2D([0], [0], color=fs.theme().band, linewidth=1.2,
                          linestyle=(0, (4, 2)), label="pass line = 1.0"))
    for state in OUTCOME_ORDER:
        if not states[state]:
            continue
        face, edge, hatch = _outcome_cell(state)
        handles.append(Patch(facecolor=face, edgecolor=edge, hatch=hatch,
                             label=f"{OUTCOME_LABELS[state]} "
                                   f"({states[state]})"))
    # one row, in the free band between the last strip's labels and the
    # two-line provenance footer: two rows collided with both.
    fig.legend(handles=handles, ncol=len(handles), loc="lower center",
               bbox_to_anchor=(0.5, 0.040), frameon=False,
               fontsize=fs.FONT_PT["legend"] - 0.5)
    print(f"  h_rel rungs present: {', '.join(f'{r:g}' for r in rungs)}")
    save(fig, out_dir, "external_comparison.png")
    return True
