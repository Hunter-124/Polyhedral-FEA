# SPDX-License-Identifier: BSD-3-Clause
"""Drawing side of ``plot_evaluation.py``: palette assignment and the four panels."""
from __future__ import annotations

import math
from typing import Any

import numpy as np
from matplotlib.lines import Line2D

import figstyle as fs

from .plot_evaluation_data import GROUPS, is_learned

LABELS = {
    "oracle": "oracle (the best choice in hindsight)",
    "advisor_policy": "advisor_policy (the advisor's own pick, previously "
                      "shipped)",
    "advisor_argmin": "advisor_argmin (ranking only — the same rule with the "
                      "gate removed)",
    "advisor_efficiency": "advisor_efficiency",
    "default": "default (shipped default action)",
    "constant_config": "constant_config",
    "family_lookup": "family_lookup",
    "finest_action": "finest_action (smallest cell size, can pick failures)",
    "spend_budget": "spend_budget (priciest feasible, by measured "
                    "degrees of freedom)",
    "random": "random",
}
#: Palette slots. Learned rules get the three strong slots plus yellow, and a
#: threshold sweep shares one slot across its members because it is drawn as
#: one line plus an envelope. Slot 6 (yellow) is last in each list because it
#: is the weakest on white.
LEARNED_SLOTS = [0, 1, 2, 6]
TRIVIAL_SLOTS = [3, 4, 5, 7, 6]
ALPHA = 0.05


def register_choosers(present: list[str], twin: tuple[str, str] | None,
                      shipped: str | None = None,
                      hindsight: set[str] | None = None) -> None:
    """Pin each chooser to a stable palette slot, grouped by kind.

    There are eight palette slots and more choosers than that, so slots are
    handed out over the choosers this file actually draws. When family_lookup
    coincides with constant_config it shares that slot instead of consuming
    one, because it is drawn as a single line.
    """
    fs.register_series("oracle", neutral=True, label=LABELS["oracle"])
    learned = [c for c in present if is_learned(c)]
    # the shipped chooser takes the first (strongest) slot, ahead of the heads
    # it replaced; everything else keeps its declared order
    learned.sort(key=lambda c: (c != shipped, c not in GROUPS["learned advisor"],
                                GROUPS["learned advisor"].index(c)
                                if c in GROUPS["learned advisor"] else 0, c))
    # a threshold sweep is one drawn line, so its members share one slot and
    # the remaining learned rules keep distinct colours
    slots: dict[str, int] = {}
    for name in learned:
        stem, _, tail = name.rpartition("_")
        try:
            float(tail)
        except ValueError:
            stem = name
        slot = slots.setdefault(
            stem, LEARNED_SLOTS[len(slots) % len(LEARNED_SLOTS)])
        fs.register_series(name, slot, label=LABELS.get(name, name))
    trivial = [c for c in present if c != "oracle" and not is_learned(c)]
    # greyed-out (hindsight) rules go last: their slot colour is never drawn,
    # so they must not consume one a visible series needs
    trivial.sort(key=lambda c: (c in (hindsight or set()),
                                GROUPS["trivial / heuristic"].index(c)
                                if c in GROUPS["trivial / heuristic"] else 99, c))
    if twin and twin[1] in trivial:
        trivial.remove(twin[1])
    for offset, name in enumerate(trivial):
        fs.register_series(name, TRIVIAL_SLOTS[offset % len(TRIVIAL_SLOTS)],
                           label=LABELS.get(name, name))
    if twin:
        fs.register_series(twin[1], fs.series(twin[0]).slot,
                           label=LABELS.get(twin[1], twin[1]))


def level_label(level: str, q: dict[str, float], bands: set[str]) -> str:
    if level == "unconstrained":
        return "no cap"
    if level in bands:
        return level[4:] if level.startswith("band") else level
    value = q.get(level, math.inf)
    return f"p{value * 100:g}" if math.isfinite(value) else level


def style_of(chooser: str, hindsight: set[str]) -> tuple[Any, str]:
    """Series style plus the colour to draw it in (grey if it has hindsight)."""
    st = fs.series(chooser)
    return st, (fs.theme().muted if chooser in hindsight else st.color)


def draw_sweep(ax: Any, tab: dict[str, Any], levels: list[str],
               choosers: list[str], q: dict[str, float], bands: set[str],
               budget_head: str, twin: tuple[str, str] | None,
               hindsight: set[str],
               groups: dict[str, list[str]] | None = None,
               shipped: str | None = None,
               note: str = "") -> None:
    t = fs.theme()
    x = np.arange(len(levels), dtype=float)
    fs.panel_title(ax, "how much worse than the best choice, by budget — "
                       "lower is better")

    values = {c: np.array([tab[c].get(lv, {}).get("macro_mean_regret", np.nan)
                           for lv in levels], dtype=float) for c in choosers}
    errs = {c: np.array([tab[c].get(lv, {}).get("fold_std", 0.0)
                         for lv in levels], dtype=float) for c in choosers}

    # oracle is the definition of zero, not a competitor: draw it as the floor
    ax.axhline(0.0, color=t.rule, linewidth=1.4, zorder=1)
    ax.text(x[-1] - 0.1, 0.01 * float(np.nanmax(list(values.values()))),
            "best choice in hindsight = 0", ha="right", va="bottom",
            fontsize=8.0,
            color=t.muted, zorder=3)

    drawn = [c for c in choosers if c != "oracle"
             and not (twin and c == twin[1])]
    for chooser in drawn:
        st, color = style_of(chooser, hindsight)
        is_shipped = chooser == shipped
        ax.errorbar(x, values[chooser], yerr=errs[chooser], color=color,
                    ecolor=color, elinewidth=0.9, capsize=2.0,
                    alpha=0.55 if chooser in hindsight else 0.95,
                    linestyle=st.dash, marker=st.marker,
                    markersize=5.6 if is_shipped else 4.2,
                    linewidth=3.0 if is_shipped else 1.6,
                    zorder=6 if is_shipped else 4)

    # a collapsed threshold sweep keeps its envelope, so the reader can see
    # how little the threshold changes and that one line stands for many
    for rep, members in (groups or {}).items():
        if rep not in drawn:
            continue
        stack = np.array([[tab[m].get(lv, {}).get("macro_mean_regret", np.nan)
                           for lv in levels] for m in members], dtype=float)
        _, color = style_of(rep, hindsight)
        ax.fill_between(x, np.nanmin(stack, axis=0), np.nanmax(stack, axis=0),
                        color=color, alpha=0.18, linewidth=0, zorder=2)

    # The key sits inside the panel: an outside legend is not seen by
    # tight_layout and pushed the right-hand column off the canvas. The upper
    # left is free because every curve starts near the oracle floor.
    handles = []
    # the shipped rule leads the key; the rest keep the visual order of the
    # curves at the widest budget so the legend reads top-down like the panel
    #
    # Labels are NAMES plus a tag, not sentences. The full explanations
    # ("priciest feasible, by measured degrees of freedom", "smallest cell
    # size, can pick failures") are what each chooser IS, and they belong in
    # the panel note and the cards; wrapped into nine legend rows they covered
    # the curves they were supposed to identify, whichever corner the key was
    # put in.
    for chooser in sorted(drawn, key=lambda c: (c != shipped, -float(
            np.nan_to_num(values[c][-1], nan=-1.0)))):
        st, color = style_of(chooser, hindsight)
        text = chooser
        if twin and chooser == twin[0]:
            text = f"{text} = {twin[1]}"
        if chooser == shipped:
            text = f"{text}  [SHIPPED]"
        elif chooser in hindsight:
            text = f"{text}  [hindsight]"
        fs.assert_glyphs(text)
        handles.append(Line2D([], [], color=color, linestyle=st.dash,
                              marker=st.marker,
                              markersize=5.6 if chooser == shipped else 4.2,
                              linewidth=3.0 if chooser == shipped else 1.6,
                              alpha=0.55 if chooser in hindsight else 0.95,
                              label=text))
    legend = ax.legend(handles=handles, loc="upper right",
                       bbox_to_anchor=(1.0, 0.34), fontsize=6.6,
                       frameon=False, labelspacing=0.4, handlelength=2.2,
                       borderaxespad=0.0, ncol=2, columnspacing=1.2)

    ax.set_xticks(x)
    ax.set_xticklabels([level_label(lv, q, bands) for lv in levels],
                       fontsize=7.4, rotation=45, ha="right",
                       rotation_mode="anchor")
    ax.set_xlabel("budget cap, as a percentile of measured "
                  f"{fs.quantity_label(budget_head)}")
    ax.set_ylabel(f"{fs.quantity_label('macro_mean_regret')}\n"
                  f"({fs.DECADES_NOTE})")
    ax.set_xlim(x[0] - 0.4, x[-1] + 0.4)
    hi = max(float(np.nanmax(v)) for v in values.values())
    n_note = note.count("\n") + 1 if note else 0
    # Headroom below the oracle floor for the note AND the key, which sit side
    # by side: whichever is taller sets the reserve. 0.10 per row, measured on
    # the six-line note this figure produces once the gate provenance wraps;
    # at 0.085 the last line printed over the rotated budget-level ticks.
    key_rows = (len(handles) + 1) // 2
    reserve = 0.10 * max(n_note, key_rows * 1.5)
    ax.set_ylim(-hi * (0.10 + reserve) if note or handles else -0.02 * hi,
                hi * (1.42 if len(drawn) > 4 else 1.12))
    if note:
        ax.text(x[0] - 0.3, -0.045 * hi, note, fontsize=6.8, color=t.muted,
                ha="left", va="top", linespacing=1.55, zorder=5)
    ax.grid(True, axis="y", color=t.grid, linewidth=0.7, zorder=0)

    right = ax.secondary_yaxis(
        "right", functions=(lambda d: np.power(10.0, d),
                            lambda f: np.log10(np.maximum(f, 1e-12))))
    right.set_ylabel("times worse than the best choice")


def draw_bands(ax: Any, tab: dict[str, Any], bands: list[str],
               choosers: list[str], twin: tuple[str, str] | None,
               hindsight: set[str], note: str,
               shipped: str | None = None) -> None:
    t = fs.theme()
    fs.panel_title(ax, "same spending — just the per-case choice")
    shown = [c for c in choosers if c != "oracle" and not (twin and c == twin[1])]
    mean_of = {c: float(np.nanmean([tab[c].get(b, {}).get("macro_mean_regret", np.nan)
                                    for b in bands])) for c in shown}
    shown.sort(key=lambda c: mean_of[c])
    y = np.arange(len(shown), dtype=float)[::-1]
    hatches = ["", "//", "..", "xx"]
    height = 0.78 / max(1, len(bands))

    for j, band in enumerate(bands):
        offset = (j - (len(bands) - 1) / 2.0) * height
        for chooser, yy in zip(shown, y):
            stats = tab[chooser].get(band, {})
            st, color = style_of(chooser, hindsight)
            ax.barh(yy + offset, stats.get("macro_mean_regret", np.nan),
                    height=height * 0.92, color=color,
                    alpha=0.55 if chooser in hindsight else 0.95,
                    edgecolor=t.ink, linewidth=1.5 if chooser == shipped else 0.5,
                    hatch=hatches[j % len(hatches)], zorder=3,
                    xerr=stats.get("fold_std", 0.0),
                    error_kw=dict(ecolor=t.ink, elinewidth=0.7, capsize=1.8))

    labels = []
    for c in shown:
        text = c + (f" = {twin[1]}" if twin and c == twin[0] else "")
        labels.append(text + ("  [hindsight]" if c in hindsight else "")
                      + ("  [SHIPPED]" if c == shipped else ""))
    ax.set_yticks(y)
    ax.set_yticklabels(labels, fontsize=7.8)
    # headroom for the wrapped note, which is drawn inside the panel
    ax.set_ylim(-1.9, len(shown) - 0.3 + 0.95 * (note.count("\n") + 1))
    ax.set_xlabel(f"{fs.quantity_label('macro_mean_regret')}\n"
                  f"({fs.DECADES_NOTE})")
    handles = [ax.barh(0, 0, color=t.panel, edgecolor=t.ink, linewidth=0.5,
                       hatch=hatches[j % len(hatches)],
                       label=f"band {b[4:] if b.startswith('band') else b}")
               for j, b in enumerate(bands)]
    ax.legend(handles=handles, fontsize=7.6, loc="lower right", frameon=False)
    ax.grid(True, axis="x", color=t.grid, linewidth=0.7, zorder=0)
    ax.text(0.99, 0.985, note, transform=ax.transAxes, fontsize=7.4,
            color=t.ink, ha="right", va="top", linespacing=1.5, zorder=5)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)


def draw_paired(ax: Any, rows: list[dict[str, Any]], note: str) -> None:
    t = fs.theme()
    fs.panel_title(ax, "head to head on the same cases — counted in the record")
    if not rows:
        ax.text(0.5, 0.5, "no head-to-head comparisons in this file",
                ha="center", va="center",
                color=t.muted, fontsize=9)
        fs.axes_off(ax)
        return

    y = np.arange(len(rows), dtype=float)[::-1]
    parts = [("wins", t.ok, ""), ("ties", t.muted, ".."), ("losses", t.bad, "//")]
    for row, yy in zip(rows, y):
        total = max(1, row["wins"] + row["losses"] + row["ties"])
        left = 0.0
        for key, color, hatch in parts:
            width = row[key] / total
            if width <= 0:
                continue
            ax.barh(yy, width, left=left, height=0.62, color=color,
                    edgecolor=t.ink, linewidth=0.5, hatch=hatch, zorder=3)
            if width > 0.10:
                ax.text(left + width / 2, yy, f"{row[key]}", ha="center",
                        va="center", fontsize=7.2, weight="bold", zorder=4,
                        color=t.ink if key == "ties" else t.bg)
            left += width
        p = row["p_value"]
        sig = math.isfinite(p) and p < ALPHA
        ax.text(1.03, yy, ("*  " if sig else "=  ")
                + (f"p={p:.2g}" if sig else f"p={p:.2g}, too close to call"),
                ha="left", va="center", fontsize=7.4, zorder=4,
                color=t.ink if sig else t.muted,
                weight="bold" if sig else "normal")

    ax.set_yticks(y)
    ax.set_yticklabels([f"{r['challenger']} vs {r['reference']} (n={r['n_paired']})"
                        for r in rows], fontsize=7.4)
    ax.set_xlim(0.0, 1.62)
    ax.set_ylim(-(0.5 + 1.15 * (note.count("\n") + 1)), len(rows) - 0.3)
    ax.set_xticks([0.0, 0.5, 1.0])
    ax.set_xticklabels(["0%", "50%", "100%"])
    ax.set_xlabel("share of cases both rules scored — wins (solid), "
                  "ties (dotted), losses (hatched)")
    ax.text(0.0, -0.62, note, fontsize=7.2, color=t.muted, ha="left",
            va="top", linespacing=1.6)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)


def draw_failures(ax: Any, rates: dict[str, dict[str, float]],
                  choosers: list[str], twin: tuple[str, str] | None,
                  hindsight: set[str], note: str) -> None:
    t = fs.theme()
    fs.panel_title(ax, "how often each rule picks a failing action")
    shown = [c for c in choosers if c in rates and not (twin and c == twin[1])]
    shown.sort(key=lambda c: rates[c]["mean"])
    y = np.arange(len(shown), dtype=float)[::-1]
    for chooser, yy in zip(shown, y):
        st, color = style_of(chooser, hindsight)
        value = rates[chooser]["mean"]
        ax.barh(yy, value, height=0.62, color=color,
                alpha=0.55 if chooser in hindsight else 0.95,
                edgecolor=t.ink, linewidth=0.5, zorder=3,
                hatch="xx" if (chooser in hindsight or chooser == "oracle") else "")
        tag = ("  — impossible without hindsight"
               if chooser in hindsight else
               "  — 0 by definition" if chooser == "oracle" else "")
        ax.text(value + 0.008, yy, f"{value * 100:.1f}%{tag}", ha="left",
                va="center", fontsize=7.4, zorder=4,
                color=t.ink if not tag else t.muted)
    labels = [c + (f" = {twin[1]}" if twin and c == twin[0] else "") for c in shown]
    ax.set_yticks(y)
    ax.set_yticklabels(labels, fontsize=7.8)
    ax.set_ylim(-(0.9 + 1.25 * (note.count("\n") + 1)), len(shown) - 0.3)
    top = max((rates[c]["mean"] for c in shown), default=0.1)
    ax.set_xlim(0.0, max(0.05, top) * 1.9)
    ax.xaxis.set_major_formatter(lambda v, _pos: f"{v * 100:g}%")
    ax.set_xlabel(f"{fs.quantity_label('pick_failure_rate')}\n"
                  "(every cross-validation group and budget)")
    ax.grid(True, axis="x", color=t.grid, linewidth=0.7, zorder=0)
    ax.text(0.0, -1.0, note, transform=ax.get_yaxis_transform(),
            fontsize=7.0, color=t.muted, ha="left", va="top", linespacing=1.6)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
