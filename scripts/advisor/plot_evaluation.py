#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Honest evaluation of the action advisor -- docs/advisor/figures/.

Reads a leave-one-group-out cross-validation record (default
``bench/advisor/crossval_final.json``) and draws one four-panel figure:

  advisor_evaluation.png
      budget sweep     macro-mean regret against budget level for every
                       chooser, oracle drawn as the zero floor rather than as a
                       competitor, fold_std error bars, and any chooser with
                       hindsight greyed out and labelled as such
      matched cost     the same choosers inside the matched-cost bands, where
                       spend allocation is taken away and only per-case
                       judgement is left
      paired tests     the *precomputed* pooled sign tests from the file,
                       wins/ties/losses with the p-value, significance marked
                       by text and hatch, never by colour alone
      pick failures    mean pick_failure_rate per chooser; a rule that never
                       picks a failing action is ranking on measured outcomes,
                       i.e. it has hindsight and is not deployable

Nothing is hardcoded about the level set, the chooser set, the pair set or the
fold count: it is all discovered from the file, because the sweep is still
getting finer and new choosers (``advisor_gated_*``) are still arriving.
Regret is in log10 units, 0 = picked the best feasible action; the right-hand
axis of the sweep restates it as "x worse than oracle".

Missing inputs print a "no data yet" note and exit 0, so this is safe to run
mid-campaign. Every number that lands in the figure is also printed.

Run from anywhere:

    python scripts/advisor/plot_evaluation.py
    python scripts/advisor/plot_evaluation.py \
        --crossval bench/advisor/crossval_geofeat.json \
        --out-dir docs/advisor/figures
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import textwrap
from datetime import datetime
from pathlib import Path
from typing import Any

import numpy as np
from matplotlib.lines import Line2D

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import figstyle as fs  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ADVISOR_DIR = ROOT / "bench" / "advisor"
#: The canonical record. Anything else is picked up only if this is absent.
CANONICAL_CROSSVAL = ADVISOR_DIR / "crossval_final.json"
#: The shipped chooser's gate threshold is read from here, so the figure
#: cannot disagree with the binary (src/advisor/src/advisor.cpp reads the same
#: key, falling back to veto_threshold when gate_threshold is absent).
CLAMPS = ADVISOR_DIR / "clamps.json"
#: Never auto-selected: deliberate ablations, plottable only via --crossval.
ABLATION_SUFFIXES = ("_geofeat", "_nogeo")
FIGURES_DIR = ROOT / "docs" / "advisor" / "figures"

#: Chooser groups. Membership decides the palette slot, so colour+marker+dash
#: stay stable; unknown names (the ``advisor_gated_*`` sweep) are sorted into
#: the learned group by prefix.
GROUPS: dict[str, list[str]] = {
    "oracle floor": ["oracle"],
    "learned advisor": ["advisor_policy", "advisor_argmin", "advisor_efficiency"],
    "trivial / heuristic": ["default", "constant_config", "family_lookup",
                            "finest_action", "spend_budget", "random"],
}
CHOOSERS: list[str] = [c for names in GROUPS.values() for c in names]

LABELS = {
    "oracle": "oracle (best feasible action)",
    "advisor_policy": "advisor_policy (policy head, previously shipped)",
    "advisor_argmin": "advisor_argmin (ranking only, no gate — ablation)",
    "advisor_efficiency": "advisor_efficiency",
    "default": "default (shipped default action)",
    "constant_config": "constant_config",
    "family_lookup": "family_lookup",
    "finest_action": "finest_action (smallest h_rel, can pick failures)",
    "spend_budget": "spend_budget (priciest feasible, measured dof)",
    "random": "random",
}
#: Palette slots. Learned rules get the three strong slots plus yellow, and a
#: threshold sweep shares one slot across its members because it is drawn as
#: one line plus an envelope. Slot 6 (yellow) is last in each list because it
#: is the weakest on white.
LEARNED_SLOTS = [0, 1, 2, 6]
TRIVIAL_SLOTS = [3, 4, 5, 7, 6]
ALPHA = 0.05
HINDSIGHT_TAG = "oracle-flavoured (has hindsight)"
#: What the SHIPPED chooser is called on the figure. The threshold is filled in
#: from clamps.json, never typed here.
SHIPPED_TAG = "SHIPPED in C++: gated enumeration over measured candidates"


def gate_threshold() -> tuple[float | None, str | None]:
    """The gate threshold the C++ chooser uses, and the key it came from.

    ``src/advisor/src/advisor.cpp`` reads ``gate_threshold`` STRICTLY and
    throws when it is absent or non-numeric — the product refuses to run on a
    clamps file that does not state it. There is deliberately no fallback to
    ``veto_threshold`` here either: inferring a threshold the binary would
    reject would let this figure label a chooser "shipped" for a
    configuration that cannot ship.
    """
    try:
        clamps = json.loads(CLAMPS.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None, None
    if not isinstance(clamps, dict):
        return None, None
    value = clamps.get("gate_threshold")
    if isinstance(value, (int, float)) and not isinstance(value, bool) \
            and math.isfinite(value):
        return float(value), "gate_threshold"
    return None, None


def shipped_chooser(present: list[str], threshold: float | None) -> str | None:
    """The ``advisor_gated_<threshold>`` series matching the binary's gate.

    Matched numerically, so ``0.5``/``0.50`` name the same series, and only if
    the record actually scored that threshold -- a figure must not label a
    series shipped when the shipped setting was never measured.
    """
    if threshold is None:
        return None
    for name in present:
        stem, _, tail = name.rpartition("_")
        if stem != "advisor_gated":
            continue
        try:
            if math.isclose(float(tail), threshold, rel_tol=0.0, abs_tol=1e-9):
                return name
        except ValueError:
            continue
    return None


def is_learned(name: str) -> bool:
    return name in GROUPS["learned advisor"] or name.startswith("advisor_")


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


def decades_to_factor(decades: float) -> float:
    """``0.30`` decades -> ``2.0x`` worse. NaN passes through.

    Mirrors :func:`scripts.advisor.regret.decades_to_factor` so this script
    stays importable without the advisor package on sys.path.
    """
    return float("nan") if not math.isfinite(decades) else float(10.0 ** decades)


# --------------------------------------------------------------------------- #
# reading
# --------------------------------------------------------------------------- #
def load(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict) or not data.get("summary"):
        return None
    return data


def band_levels(data: dict[str, Any]) -> set[str]:
    """Matched-cost bands, recognised by their floor_quantile in the runs."""
    bands = set()
    for run in data.get("runs", []):
        for level, block in (run.get("levels") or {}).items():
            floor = (block or {}).get("floor_quantile")
            if isinstance(floor, (int, float)) and math.isfinite(floor):
                bands.add(level)
    for level in (data["summary"].get("levels") or {}):
        if level.startswith("band"):
            bands.add(level)
    return bands


def level_quantiles(data: dict[str, Any]) -> dict[str, float]:
    """Sort key per level, discovered from the runs (never a fixed list)."""
    order: dict[str, float] = {}
    for run in data.get("runs", []):
        for level, block in (run.get("levels") or {}).items():
            q = (block or {}).get("quantile")
            if isinstance(q, (int, float)) and math.isfinite(q):
                order.setdefault(level, float(q))
    for level in (data["summary"].get("levels") or {}):
        if level in order:
            continue
        if level == "unconstrained":
            order[level] = math.inf
        elif level.startswith("band"):
            parts = [p for p in level[4:].replace("-", " ").split() if p]
            try:
                nums = [float(p) for p in parts]
                order[level] = sum(nums) / len(nums)
            except ValueError:
                order[level] = math.inf
        elif level.startswith("q"):
            try:
                order[level] = float(level[1:])
            except ValueError:
                order[level] = math.inf
        else:
            order[level] = math.inf
    return order


def sorted_levels(data: dict[str, Any]) -> list[str]:
    q = level_quantiles(data)
    levels = list(data["summary"].get("levels") or {})
    return sorted(levels, key=lambda lv: (q.get(lv, math.inf), lv))


def head_of(data: dict[str, Any], levels: list[str]) -> str | None:
    """The head the file was scored on: its own ``objective`` if present."""
    available: list[str] = []
    for lv in levels:
        available += list((data["summary"]["levels"].get(lv) or {}))
    objective = data.get("objective")
    if objective in available:
        return str(objective)
    return available[0] if available else None


def table(data: dict[str, Any], levels: list[str], head: str
          ) -> dict[str, dict[str, dict[str, float]]]:
    """``{chooser: {level: {macro_mean_regret, fold_std, ...}}}``."""
    out: dict[str, dict[str, dict[str, float]]] = {}
    for lv in levels:
        block = (data["summary"]["levels"].get(lv) or {}).get(head) or {}
        for chooser, stats in block.items():
            if not isinstance(stats, dict):
                continue
            out.setdefault(chooser, {})[lv] = {
                "macro_mean_regret": float(stats.get("macro_mean_regret", float("nan"))),
                "fold_std": float(stats.get("fold_std", 0.0) or 0.0),
                "mean_seed_std": float(stats.get("mean_seed_std", 0.0) or 0.0),
                "n_folds": float(stats.get("n_folds", 0) or 0),
            }
    return out


def ordered_choosers(tab: dict[str, Any]) -> list[str]:
    known = [c for c in CHOOSERS if c in tab]
    extra = sorted(c for c in tab if c not in CHOOSERS)
    return known + extra


def primary_level(levels: list[str], q: dict[str, float]) -> str:
    """The representative budget.

    The median budget quantile (p50) when the sweep contains it -- that is the
    level the producer runs its paired tests at, so the figure and the write-up
    quote the same number. Otherwise the middle finite quantile present.
    """
    finite = [lv for lv in levels if math.isfinite(q.get(lv, math.inf))]
    half = [lv for lv in finite if math.isclose(q[lv], 0.5, abs_tol=1e-9)]
    if half:
        return half[0]
    if finite:
        return finite[(len(finite) - 1) // 2]
    return levels[-1]


def zero_case_folds(data: dict[str, Any]) -> list[tuple[Any, list[str]]]:
    """Folds where no level scored a single case -- they cannot be averaged."""
    seen: dict[Any, list[str]] = {}
    scored: set[Any] = set()
    for run in data.get("runs", []):
        fold = run.get("fold")
        counts = [int((b or {}).get("n_cases", 0) or 0)
                  for b in (run.get("levels") or {}).values()]
        if counts and max(counts) > 0:
            scored.add(fold)
        else:
            seen.setdefault(fold, [str(g) for g in (run.get("held_out_groups") or [])])
    return [(f, g) for f, g in sorted(seen.items(), key=lambda kv: str(kv[0]))
            if f not in scored]


def lookup_hit_rates(data: dict[str, Any]) -> list[float]:
    vals = []
    for run in data.get("runs", []):
        v = run.get("family_lookup_hit_rate")
        if isinstance(v, (int, float)) and math.isfinite(v):
            vals.append(float(v))
    return vals


def coincident(tab: dict[str, Any], a: str, b: str, levels: list[str]) -> bool:
    if a not in tab or b not in tab:
        return False
    for lv in levels:
        va, vb = tab[a].get(lv), tab[b].get(lv)
        if va is None or vb is None:
            return False
        if not math.isclose(va["macro_mean_regret"], vb["macro_mean_regret"],
                            rel_tol=1e-9, abs_tol=1e-12):
            return False
    return True


def failure_rates(data: dict[str, Any]) -> dict[str, dict[str, float]]:
    """``{chooser: {mean, max, n}}`` over every scored (run, level)."""
    acc: dict[str, list[float]] = {}
    for run in data.get("runs", []):
        for block in (run.get("levels") or {}).values():
            if not int((block or {}).get("n_cases", 0) or 0):
                continue
            for chooser, rate in ((block or {}).get("pick_failure_rate") or {}).items():
                if isinstance(rate, (int, float)) and math.isfinite(rate):
                    acc.setdefault(chooser, []).append(float(rate))
    return {c: {"mean": float(np.mean(v)), "max": float(np.max(v)), "n": len(v)}
            for c, v in acc.items() if v}


def hindsight_choosers(rates: dict[str, dict[str, float]]) -> list[str]:
    """Choosers that never pick a failing action while others do.

    A rule that cannot select an action that failed is ranking candidates on
    *measured* outcomes, which no deployable rule can do. Derived, not listed.
    """
    if not rates or max(v["max"] for v in rates.values()) <= 0.0:
        return []
    return sorted(c for c, v in rates.items()
                  if v["max"] == 0.0 and c != "oracle")


def paired_rows(data: dict[str, Any]) -> list[dict[str, Any]]:
    pooled = data["summary"].get("paired_pooled") or {}
    rows = []
    for key, stats in pooled.items():
        if not isinstance(stats, dict):
            continue
        challenger, _, reference = str(key).partition("_vs_")
        rows.append({
            "key": key,
            "challenger": challenger,
            "reference": reference or "?",
            "wins": int(stats.get("wins", 0) or 0),
            "losses": int(stats.get("losses", 0) or 0),
            "ties": int(stats.get("ties", 0) or 0),
            "n_paired": int(stats.get("n_paired", 0) or 0),
            "p_value": float(stats.get("p_value", float("nan"))),
        })
    rows.sort(key=lambda r: (r["challenger"], r["reference"]))
    return rows


def collapse_families(choosers: list[str], tab: dict[str, Any], level: str,
                      prefer: str | None = None
                      ) -> tuple[dict[str, list[str]], dict[str, str]]:
    """Fold threshold sweeps such as ``advisor_gated_0.2`` into one series.

    Returns ``({representative: [members]}, {member: representative})``. The
    representative is ``prefer`` when it belongs to the family -- the shipped
    setting, which is the one a reader must take away -- otherwise the best
    member at ``level``. The other settings survive as the envelope, so the
    figure shows how little the threshold matters instead of a dozen
    near-identical competing lines. Families are found by stripping a trailing
    ``_<number>``, never by a hardcoded list.
    """
    families: dict[str, list[str]] = {}
    for chooser in choosers:
        stem, _, tail = chooser.rpartition("_")
        try:
            float(tail)
        except ValueError:
            continue
        if stem:
            families.setdefault(stem, []).append(chooser)
    groups: dict[str, list[str]] = {}
    member_of: dict[str, str] = {}
    for stem, members in families.items():
        if len(members) < 2:
            continue
        if prefer in members:
            rep = str(prefer)
        else:
            rep = min(members, key=lambda c: (
                not math.isfinite(
                    tab[c].get(level, {}).get("macro_mean_regret", math.nan)),
                tab[c].get(level, {}).get("macro_mean_regret", math.inf)))
        groups[rep] = sorted(members)
        for member in members:
            member_of[member] = rep
    return groups, member_of

# --------------------------------------------------------------------------- #
# drawing
# --------------------------------------------------------------------------- #
def _fit(bits: list[str], font_pt: float, width_in: float,
         per_char: float = 0.505) -> str:
    """Join caption fragments, dropping trailing ones that will not fit.

    figstyle lays titles and subtitles out as a single unwrapped line, so an
    over-long caption is silently clipped at the figure edge. Budget characters
    from the figure width instead of guessing.
    """
    budget = int((width_in * 72.0 * 0.976) / (per_char * font_pt))
    text = ""
    for bit in bits:
        candidate = bit if not text else f"{text}; {bit}"
        if len(candidate) > budget and text:
            break
        text = candidate
    return text


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
    fs.panel_title(ax, "regret against budget — lower is better")

    values = {c: np.array([tab[c].get(lv, {}).get("macro_mean_regret", np.nan)
                           for lv in levels], dtype=float) for c in choosers}
    errs = {c: np.array([tab[c].get(lv, {}).get("fold_std", 0.0)
                         for lv in levels], dtype=float) for c in choosers}

    # oracle is the definition of zero, not a competitor: draw it as the floor
    ax.axhline(0.0, color=t.rule, linewidth=1.4, zorder=1)
    ax.text(x[-1] - 0.1, 0.01 * float(np.nanmax(list(values.values()))),
            "oracle floor = 0", ha="right", va="bottom", fontsize=8.0,
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
    # ("priciest feasible, measured dof", "smallest h_rel, can pick failures")
    # are what each chooser IS, and they belong in the panel note and the
    # cards; wrapped into nine legend rows they covered the curves they were
    # supposed to identify, whichever corner the key was put in.
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
    ax.set_xlabel(f"budget level (quantile of measured {budget_head})")
    ax.set_ylabel("macro-mean regret (log10 decades)")
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
    right.set_ylabel("x worse than oracle")


def draw_bands(ax: Any, tab: dict[str, Any], bands: list[str],
               choosers: list[str], twin: tuple[str, str] | None,
               hindsight: set[str], note: str,
               shipped: str | None = None) -> None:
    t = fs.theme()
    fs.panel_title(ax, "matched cost — spend allocation removed")
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
    ax.set_xlabel("matched-cost regret (log10 decades)")
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
    fs.panel_title(ax, "pooled paired tests — precomputed in the record")
    if not rows:
        ax.text(0.5, 0.5, "no paired tests in file", ha="center", va="center",
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
                + (f"p={p:.2g}" if sig else f"p={p:.2g} n.s."),
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
    ax.set_xlabel("share of paired cases — wins (solid) / ties (dotted) / losses (hatched)")
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
    ax.set_xlabel("mean pick_failure_rate over every scored fold x level")
    ax.grid(True, axis="x", color=t.grid, linewidth=0.7, zorder=0)
    ax.text(0.0, -1.0, note, transform=ax.get_yaxis_transform(),
            fontsize=7.0, color=t.muted, ha="left", va="top", linespacing=1.6)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)


# --------------------------------------------------------------------------- #
def advisor_evaluation(crossval: Path, out_dir: Path,
                       requested_level: str = "") -> bool:
    data = load(crossval)
    if data is None:
        print(f"no data yet — expected {crossval}; skipping advisor_evaluation.png")
        return False

    levels = sorted_levels(data)
    head = head_of(data, levels)
    if not levels or head is None:
        print(f"no data yet — {crossval} carries no scored levels; "
              "skipping advisor_evaluation.png")
        return False
    q = level_quantiles(data)
    bands = band_levels(data)
    band_list = [lv for lv in levels if lv in bands]
    budget_levels = [lv for lv in levels if lv not in bands]
    tab = table(data, levels, head)
    choosers = ordered_choosers(tab)
    if not choosers or not budget_levels:
        print(f"no data yet — {crossval} carries no scored choosers or budgets; "
              "skipping advisor_evaluation.png")
        return False

    primary = primary_level(budget_levels, q)
    if requested_level:
        if requested_level not in budget_levels:
            print(f"no data yet — level {requested_level!r} is not in "
                  f"{crossval.name} ({', '.join(budget_levels)}); "
                  "skipping advisor_evaluation.png")
            return False
        primary = requested_level
    budget_head = str(data.get("budget_head", "dof"))
    split_mode = str(data.get("split_mode", "?"))
    n_seeds = len(data.get("seeds") or [])
    zeros = zero_case_folds(data)
    hits = lookup_hit_rates(data)
    rates = failure_rates(data)
    hindsight = set(hindsight_choosers(rates))
    twin_pair = ("constant_config", "family_lookup")
    twin = twin_pair if coincident(tab, *twin_pair, levels) else None
    gate, gate_key = gate_threshold()
    shipped = shipped_chooser(choosers, gate)
    register_choosers(choosers, twin, shipped, hindsight)
    rows = paired_rows(data)
    missing = [c for c in CHOOSERS if c not in tab]
    n_folds_used = max((int(tab[c][lv]["n_folds"]) for c in choosers
                        for lv in tab[c]), default=0)
    groups, member_of = collapse_families(choosers, tab, primary, shipped)
    shown = [c for c in choosers if member_of.get(c, c) == c]
    sweep_labels = {}
    for rep, members in groups.items():
        stem, _, tail = rep.rpartition("_")
        if rep == shipped:
            sweep_labels[rep] = (f"{rep} — {SHIPPED_TAG} (gate {tail} from "
                                 f"clamps.json:{gate_key}; envelope = "
                                 f"{len(members) - 1} other thresholds)")
        else:
            sweep_labels[rep] = (f"{stem}_* ({len(members)} thresholds,\n"
                                 f"best {tail})")
        fs.register_series(rep, label=sweep_labels[rep])
        fs.assert_glyphs(sweep_labels[rep])

    mtime = datetime.fromtimestamp(crossval.stat().st_mtime).isoformat(
        timespec="seconds")
    print(f"\nadvisor_evaluation.png — {crossval.name}")
    print(f"  record                : {crossval.as_posix()} (mtime {mtime})")
    print(f"  split_mode            : {split_mode}, "
          f"{data.get('n_folds', '?')} folds, {n_seeds} seeds, "
          f"objective head '{head}', budget head '{budget_head}'")
    print(f"  budget levels         : {', '.join(budget_levels)}")
    print(f"  matched-cost bands    : {', '.join(band_list) or 'none in file'}")
    print(f"  primary budget level  : {primary}")
    print(f"  gate threshold        : "
          + (f"{gate:g} from {CLAMPS.as_posix()}:{gate_key} — read strictly, "
             "as src/advisor/src/advisor.cpp does; it throws when the key is "
             "absent" if gate is not None
             else f"MISSING from {CLAMPS.as_posix()} — the product refuses to "
                  "run without it, so no chooser is labelled shipped"))
    print(f"  shipped chooser       : "
          + (shipped if shipped else
             "no advisor_gated_* series matches the shipped gate"))
    for rep, members in groups.items():
        print(f"  collapsed sweep       : {rep.rpartition('_')[0]}_* -> {rep} "
              f"({'shipped threshold' if rep == shipped else 'best'} of "
              f"{len(members)} at {primary}: {', '.join(members)})")
    if missing:
        print(f"  choosers absent       : {', '.join(missing)}")

    def rank(level: str) -> list[tuple[str, dict[str, float]]]:
        return sorted(((c, tab[c].get(level, {})) for c in choosers),
                      key=lambda kv: (not math.isfinite(
                          kv[1].get("macro_mean_regret", math.nan)),
                          kv[1].get("macro_mean_regret", math.inf)))

    ranking = rank(primary)
    print(f"\n  ranking at '{primary}' (macro-mean regret, log10 decades; "
          "lower is better)")
    print("    rank chooser              regret   x worse  fold_std  seed_std  folds")
    for i, (chooser, stats) in enumerate(ranking, start=1):
        r = stats.get("macro_mean_regret", float("nan"))
        tag = ("  <- floor, 0 by definition" if chooser == "oracle"
               else "  <- has hindsight, not deployable" if chooser in hindsight
               else "")
        print(f"    {i:>4} {chooser:<20} {r:7.4f}  {decades_to_factor(r):7.3f}"
              f"  {stats.get('fold_std', 0.0):8.4f}"
              f"  {stats.get('mean_seed_std', 0.0):8.4f}"
              f"  {int(stats.get('n_folds', 0)):5d}{tag}")

    print("\n  full table (macro-mean regret by level)")
    print("    chooser             " + "".join(f"{lv:>14}" for lv in levels))
    for chooser in choosers:
        cells = "".join(
            f"{tab[chooser].get(lv, {}).get('macro_mean_regret', float('nan')):14.4f}"
            for lv in levels)
        print(f"    {chooser:<20}{cells}")

    print("\n  pooled paired tests (from summary.paired_pooled, precomputed)")
    print("    pair                                          wins loss ties     n"
          "   p_value  verdict")
    for row in rows:
        p = row["p_value"]
        verdict = "significant" if math.isfinite(p) and p < ALPHA else "n.s."
        print(f"    {row['challenger']:<20} vs {row['reference']:<20}"
              f"{row['wins']:5d}{row['losses']:5d}{row['ties']:5d}"
              f"{row['n_paired']:6d}  {p:9.3g}  {verdict}")

    print("\n  pick_failure_rate (mean over every scored fold x level)")
    for chooser, stats in sorted(rates.items(), key=lambda kv: kv[1]["mean"]):
        tag = ("  <- never picks a failure: ranks on measured outcomes, "
               "has hindsight" if chooser in hindsight else "")
        print(f"    {chooser:<20} {stats['mean'] * 100:6.2f}%  "
              f"max {stats['max'] * 100:6.2f}%  n={stats['n']}{tag}")

    for fold, held_out in zeros:
        print(f"\n  fold {fold} ({', '.join(held_out) or 'unnamed'}) contributed "
              "zero scorable cases — excluded from every macro mean")
    hit_note = ""
    if hits:
        print(f"  family_lookup hit rate: mean {np.mean(hits):.3f} over "
              f"{len(hits)} runs with a finite rate")
        if max(hits) == 0.0:
            hit_note = "family_lookup_hit_rate = 0.0 everywhere"
    if twin:
        print(f"  {twin[1]} is IDENTICAL to {twin[0]} at every level"
              + (f" ({hit_note})" if hit_note else "")
              + " — drawn as one line, not two")

    # ---- captions, every number computed ---------------------------------- #
    # The shipped rule is the gated enumeration at the threshold the binary
    # reads; if the record never scored that threshold there is no shipped
    # series to point at and the best deployable chooser carries the caption.
    order = [c for c, _ in ranking if member_of.get(c, c) == c]
    rivals = [c for c in order if c != "oracle" and c not in hindsight
              and not (twin and c == twin[1])]
    best = rivals[0] if rivals else order[0]
    best_r = tab[best].get(primary, {}).get("macro_mean_regret", float("nan"))
    focus = shipped if shipped in tab else best
    focus_r = tab[focus].get(primary, {}).get("macro_mean_regret", float("nan"))
    # Rank over DISTINCT choosers: the advisor_gated_* sweep is one idea at
    # several thresholds, and "last of 14" would read as fourteen ideas tested.
    distinct = [c for c in rivals if c not in member_of or c in groups]
    focus_rank = (distinct.index(focus) + 1) if focus in distinct else None
    n_distinct = len(distinct)

    def pair(challenger: str, reference: str) -> dict[str, Any] | None:
        return next((r for r in rows if r["challenger"] == challenger
                     and r["reference"] == reference), None)

    # the ranking-only sibling: the same predictions without the gate, so the
    # difference between the two is exactly what the gate buys
    ungated = next((c for c in rivals if is_learned(c) and c not in groups
                    and c not in member_of and c != focus
                    and c != "advisor_efficiency" and c != "advisor_policy"),
                   None)
    policy = "advisor_policy" if "advisor_policy" in tab else None
    ns_pairs = [r for r in rows if r["challenger"] == focus
                and not is_learned(r["reference"])
                and math.isfinite(r["p_value"]) and r["p_value"] >= ALPHA]
    # the headline is the shipped chooser against the toughest deployable
    # non-learned comparator it was actually tested against, chosen from the
    # record rather than named here
    candidates = [r for r in rows if r["challenger"] == focus
                  and not is_learned(r["reference"])
                  and r["reference"] not in hindsight
                  and r["n_paired"] > 0 and math.isfinite(r["p_value"])]
    headline = min(candidates, key=lambda r: r["wins"] - r["losses"],
                   default=None)
    if headline is None:
        headline = pair("advisor_argmin", "finest_action")

    band_rank: list[str] = []
    band_best = ""
    band_mean: dict[str, float] = {}
    if band_list:
        band_mean = {c: float(np.nanmean(
            [tab[c].get(b, {}).get("macro_mean_regret", np.nan) for b in band_list]))
            for c in shown if c != "oracle" and not (twin and c == twin[1])}
        band_rank = sorted(band_mean, key=lambda c: band_mean[c])
        band_best = band_rank[0]
        print("\n  matched-cost bands (" + ", ".join(band_list)
              + ") — mean regret across bands, best first")
        for i, chooser in enumerate(band_rank, start=1):
            print(f"    {i:>4} {chooser:<20} {band_mean[chooser]:7.4f}"
                  + ("  <- SHIPPED" if chooser == focus else "")
                  + ("  <- has hindsight" if chooser in hindsight else ""))

    band_deployable = [c for c in band_rank if c not in hindsight]
    focus_band_rank = (band_rank.index(focus) + 1) if focus in band_rank else None
    focus_band_worst = bool(band_rank) and band_rank[-1] == focus
    # the cheapest hindsight baseline: not deployable, but it is the number the
    # advisor was previously behind, so whether it is still ahead is the result
    oracleish = min((c for c in hindsight if c in tab),
                    key=lambda c: tab[c].get(primary, {}).get(
                        "macro_mean_regret", math.inf), default=None)
    oracleish_r = (tab[oracleish].get(primary, {}).get("macro_mean_regret",
                                                       float("nan"))
                   if oracleish else float("nan"))
    gate_row = pair(focus, ungated) if ungated else None
    rank_row = pair(ungated, headline["reference"]) if (
        ungated and headline) else None

    title_bits = []
    if headline:
        won = headline["wins"] > headline["losses"]
        sig = math.isfinite(headline["p_value"]) and headline["p_value"] < ALPHA
        title_bits.append(
            f"{headline['challenger']} {'beats' if won else 'loses to'}"
            f" {headline['reference']}"
            f" {headline['wins']}W-{headline['losses']}L"
            f" p={headline['p_value']:.2g}{'' if sig else ' n.s.'}")
    # what the gate itself buys over the same predictions ranked without it:
    # state it either way, from the paired test, never from expectation
    if gate_row is not None:
        g_sig = (math.isfinite(gate_row["p_value"])
                 and gate_row["p_value"] < ALPHA)
        g_won = gate_row["wins"] > gate_row["losses"]
        title_bits.append(
            f"gate vs {ungated} "
            + ("significant" if g_sig and g_won else "not significant")
            + f" ({gate_row['wins']}W-{gate_row['losses']}L p="
              f"{gate_row['p_value']:.2g})")
    title = _fit(["Advisor evaluation: " + (title_bits[0] if title_bits
                                            else "budget sweep and paired tests")]
                 + title_bits[1:], fs.FONT_PT["title"], 14.5,
                 per_char=0.595)

    shipped_word = "shipped" if focus == shipped else "best deployable"
    bits = [f"{head} at {primary}: {shipped_word} {focus} {focus_r:.3f}"
            + (f" ({focus_rank}/{n_distinct})" if focus_rank else "")
            + (", ranking-only "
               f"{tab[ungated].get(primary, {}).get('macro_mean_regret', float('nan')):.3f}"
               if ungated else "")
            + (", policy head "
               f"{tab[policy].get(primary, {}).get('macro_mean_regret', float('nan')):.3f}"
               if policy else "")]
    if oracleish and math.isfinite(oracleish_r) and math.isfinite(focus_r):
        hs_row = pair(focus, oracleish)
        ahead = focus_r < oracleish_r
        decided = bool(hs_row) and math.isfinite(hs_row["p_value"]) \
            and hs_row["p_value"] < ALPHA
        # A 0.001-decade lead that the paired test cannot separate is not a
        # win, and calling it one is the exact overstatement this figure
        # exists to avoid. Only the significance test gets to say "beats".
        if decided:
            verdict = "now beats" if ahead else "still loses to"
        else:
            verdict = "is level with"
        bits.append(
            f"{verdict} hindsight {oracleish} {oracleish_r:.3f}"
            + (f" ({hs_row['wins']}W-{hs_row['losses']}L p="
               f"{hs_row['p_value']:.2g}"
               + ("" if decided else " n.s., not distinguishable") + ")"
               if hs_row else ""))
    lost_pairs = [r for r in rows if r["challenger"] == focus
                  and not is_learned(r["reference"])
                  and r["reference"] not in hindsight
                  and r["losses"] > r["wins"]
                  and math.isfinite(r["p_value"]) and r["p_value"] < ALPHA]
    def _pairs(label: str, group: list[dict[str, Any]], show: int = 2) -> str:
        ranked = sorted(group, key=lambda r: r["p_value"])
        named = ", ".join(f"{r['reference']} p={r['p_value']:.2g}"
                          for r in ranked[:show])
        extra = f" (+{len(ranked) - show} more)" if len(ranked) > show else ""
        return f"{label} {named}{extra}"

    if lost_pairs:
        bits.append(_pairs("significantly worse than", lost_pairs, show=1))
    if focus_band_rank:
        bits.append(f"at matched cost {focus_band_rank}/{len(band_rank)}"
                    + (", the worst chooser" if focus_band_worst
                       else f", behind {band_best}"))
    if ns_pairs:
        bits.append(_pairs("n.s. vs", ns_pairs))
    subtitle = _fit(bits, fs.FONT_PT["subtitle"], 14.5)

    zero_note = "; ".join(
        f"fold {f} ({', '.join(g) or 'unnamed'}) scored 0 cases and is excluded "
        "from every macro mean" for f, g in zeros)
    prov = data.get("provenance") or {}
    stamp = ""
    stale = ""
    if isinstance(prov, dict):
        rev = str(prov.get("git_revision", ""))[:12]
        sha = str(prov.get("dataset_sha256", ""))
        parts = [f"rev {rev}" if rev else "",
                 f"dataset sha256 {sha[:12]}" if sha else ""]
        stamp = " | " + ", ".join(p for p in parts if p) if any(parts) else ""
        # The record names the dataset it was computed on; compare it with the
        # dataset that exists now. A crossval record left over from a previous
        # corpus draws a figure indistinguishable from a current one, and this
        # repository has three truth regimes' worth of leftovers.
        stale = fs.stale_against(sha, ADVISOR_DIR / "dataset.csv")
        if stale:
            print(f"  {stale}")
    footer = fs.footer_source(
        crossval, n=len(data.get("runs") or []),
        note=f"leave-one-{split_mode}-out, {n_folds_used} scorable folds x "
             f"{n_seeds} seeds, objective '{head}'"
             + (f" | {zero_note}" if zero_note else "") + stamp
             + (f"\n{stale}" if stale else ""))

    seed_max = max((tab[c][lv]["mean_seed_std"] for c in choosers for lv in tab[c]),
                   default=0.0)
    sweep_notes = [
        f"error bars = fold_std over {n_folds_used} folds; "
        + (f"largest mean_seed_std {seed_max:.3f} over {n_seeds} seeds"
           if seed_max > 0 else "mean_seed_std = 0 everywhere")]
    twin_note = ""
    if twin:
        twin_note = (f"{twin[1]} coincides exactly with {twin[0]}"
                     + (f" — {hit_note}, so the held-out family always falls "
                        "back" if hit_note else ""))
    hindsight_note = ""
    if hindsight:
        hindsight_note = (", ".join(sorted(hindsight)) + " is greyed out: "
                          f"{HINDSIGHT_TAG} — it ranks candidates by measured "
                          "cost, so it can never pick a failing action")
    for rep, members in groups.items():
        spread = [tab[m].get(primary, {}).get("macro_mean_regret", float("nan"))
                  for m in members]
        others = [m for m in members if m != rep]
        lead = (f"{rep} drawn heavy: shipped gate, read from "
                f"clamps.json:{gate_key}"
                if rep == shipped else
                f"{rep} is the best of the sweep at {primary}; the shipped "
                "threshold is not in this record")
        sweep_notes.append(
            f"{lead}. Shaded envelope = the other {len(others)} thresholds; "
            f"the sweep spans {min(spread):.3f}-{max(spread):.3f} decades at "
            f"{primary}, so the threshold is worth "
            f"{max(spread) - min(spread):.3f} decades and is not the result")
    if missing:
        sweep_notes.append("absent from this file: " + ", ".join(missing))
    # 44 columns, not 52: the key occupies the right of the same empty band,
    # and a wider note ran underneath it.
    sweep_note = "\n".join(textwrap.fill(line, 44, subsequent_indent="   ")
                           for line in sweep_notes)
    paired_note = (f"n = paired cases with a decided comparison; "
                   f"* = p < {ALPHA:g}, = : not significant")
    if rank_row is not None:
        r_sig = (math.isfinite(rank_row["p_value"])
                 and rank_row["p_value"] < ALPHA
                 and rank_row["wins"] > rank_row["losses"])
        paired_note += "\n" + textwrap.fill(
            f"ranking alone ({ungated}, no gate) "
            + ("also clears" if r_sig else "does not clear")
            + f" {rank_row['reference']} "
              f"({rank_row['wins']}W-{rank_row['losses']}L "
              f"p={rank_row['p_value']:.2g})"
            + (f", and {focus} vs {ungated} is "
               f"{gate_row['wins']}W-{gate_row['losses']}L-"
               f"{gate_row['ties']}T p={gate_row['p_value']:.2g}"
               + (": the gate no longer separates from ranking alone"
                  if not (math.isfinite(gate_row["p_value"])
                          and gate_row["p_value"] < ALPHA
                          and gate_row["wins"] > gate_row["losses"])
                  else ": the gate still separates from ranking alone")
               if gate_row is not None else ""), 96)

    band_note = ""
    if focus_band_rank and band_rank:
        dep_rank = (band_deployable.index(focus) + 1
                    if focus in band_deployable else None)
        ahead_of = (band_rank[focus_band_rank - 2]
                    if focus_band_rank >= 2 else None)
        band_note = textwrap.fill(
            f"budget matched, {focus} ranks {focus_band_rank}/{len(band_rank)}"
            + (" — worst of all" if focus_band_worst else "")
            + (f" ({dep_rank}/{len(band_deployable)} deployable)"
               if dep_rank else "")
            + (f", behind {ahead_of}" if ahead_of else "")
            + ": most of the win is spend allocation, not per-case judgement",
            58)
    gate_notes = []
    for rep in groups:
        base = ungated or "advisor_argmin"
        if rep in rates and base in rates:
            gate_notes.append(
                f"{rep} cuts the pick-failure rate from "
                f"{rates[base]['mean'] * 100:.1f}% ({base}) to "
                f"{rates[rep]['mean'] * 100:.1f}% (mean over all budget "
                f"levels; tight budgets leave fewer feasible actions, so a "
                f"single level reads 2-3 points lower), and regret at "
                f"{primary} from "
                f"{tab[base].get(primary, {}).get('macro_mean_regret', float('nan')):.3f}"
                f" to {tab[rep].get(primary, {}).get('macro_mean_regret', float('nan')):.3f}"
                " decades")
    failure_note = "\n".join(textwrap.fill(line, 62) for line in [
        "a deployable rule must sometimes pick an action that turns out to "
        "fail; a 0% rate means the rule ranked candidates on measured "
        "outcomes it could not have known"]
        + gate_notes
        + ([hindsight_note] if hindsight_note else [])
        + ([twin_note] if twin_note else []))

    fs.assert_glyphs(title, subtitle, footer, paired_note, band_note,
                     failure_note, sweep_note, HINDSIGHT_TAG,
                     *(LABELS[c] for c in choosers if c in LABELS))

    out_dir.mkdir(parents=True, exist_ok=True)
    fig, axes = fs.figure(
        title, subtitle=subtitle, footer=footer, size=(14.5, 10.0),
        nrows=2, ncols=2,
        share_y_axis="the four panels are regret, regret in bands, a share of "
                     "paired cases and a failure rate",
        gridspec_kw={"width_ratios": [1.62, 1.0]})
    draw_sweep(axes[0][0], tab, budget_levels, shown, q, bands, budget_head,
               twin, hindsight, groups, shipped, sweep_note)
    if band_list:
        draw_bands(axes[0][1], tab, band_list, shown, twin, hindsight,
                   band_note, shipped)
    else:
        axes[0][1].text(0.5, 0.5, "no matched-cost bands in this file",
                        ha="center", va="center", color=fs.theme().muted,
                        fontsize=9)
        fs.axes_off(axes[0][1])
    drawn_rows = [
        r for r in rows
        if member_of.get(r["challenger"], r["challenger"]) == r["challenger"]
        and member_of.get(r["reference"], r["reference"]) == r["reference"]
        and (not is_learned(r["reference"])
             or (r["challenger"] in groups and r["reference"] == ungated))
        and r["n_paired"] > 0 and math.isfinite(r["p_value"])]
    hidden_rows = len(rows) - len(drawn_rows)
    if hidden_rows:
        paired_note += (f"\n{hidden_rows} further pairs (advisor-vs-advisor and "
                        "the collapsed threshold sweep) are printed by the "
                        "generator")
        fs.assert_glyphs(paired_note)
    draw_paired(axes[1][0], drawn_rows, paired_note)
    if rates:
        draw_failures(axes[1][1], rates, shown, twin, hindsight, failure_note)
    else:
        axes[1][1].text(0.5, 0.5, "no pick_failure_rate in this file",
                        ha="center", va="center", color=fs.theme().muted,
                        fontsize=9)
        fs.axes_off(axes[1][1])
    fig.subplots_adjust(wspace=0.55, hspace=0.42)
    path = fs.finish(fig, out_dir / "advisor_evaluation.png")
    print(f"\n  best deployable at '{primary}': {best} ({best_r:.4f} decades, "
          f"{decades_to_factor(best_r):.3f}x oracle)")
    print(f"  shipped at '{primary}'        : {focus} ({focus_r:.4f} decades, "
          f"{decades_to_factor(focus_r):.3f}x oracle)"
          + (f", rank {focus_rank}/{n_distinct} distinct" if focus_rank else ""))
    if gate_row is not None:
        print(f"  gate vs ranking only     : {focus} vs {ungated} "
              f"{gate_row['wins']}W-{gate_row['losses']}L-{gate_row['ties']}T "
              f"p={gate_row['p_value']:.3g} "
              + ("significant" if gate_row["p_value"] < ALPHA
                 and gate_row["wins"] > gate_row["losses"] else "NOT significant"))
    if rank_row is not None:
        print(f"  ranking only vs {rank_row['reference']:<12}: "
              f"{rank_row['wins']}W-{rank_row['losses']}L-{rank_row['ties']}T "
              f"p={rank_row['p_value']:.3g} "
              + ("significant" if rank_row["p_value"] < ALPHA
                 and rank_row["wins"] > rank_row["losses"] else "NOT significant"))
    if oracleish:
        print(f"  vs hindsight {oracleish:<15}: {focus_r:.4f} vs "
              f"{oracleish_r:.4f} decades — "
              + ("advisor ahead" if focus_r < oracleish_r else "baseline ahead"))
    if focus_band_rank:
        print(f"  matched-cost rank        : {focus} {focus_band_rank}/"
              f"{len(band_rank)} overall, "
              + (f"{band_deployable.index(focus) + 1}/{len(band_deployable)} "
                 "among deployable" if focus in band_deployable else "n/a")
              + ("; WORST chooser" if focus_band_worst else ""))
    print(f"  subtitle: {subtitle}")
    print(f"  title: {title}")
    print(f"  wrote {path}")
    return True


def default_crossval() -> Path:
    """The canonical cross-validation record, or the newest stand-in.

    The producer renames artifacts as the study moves, so a fixed name list
    would silently plot a stale file; mtime only breaks the tie between
    non-canonical candidates. Ablations (``*_geofeat.json``) are excluded --
    they are deliberately worse and must never become the default.
    """
    if CANONICAL_CROSSVAL.is_file():
        return CANONICAL_CROSSVAL
    candidates = [p for p in ADVISOR_DIR.glob("crossval*.json")
                  if not any(p.stem.endswith(sfx) for sfx in ABLATION_SUFFIXES)]
    if not candidates:
        return CANONICAL_CROSSVAL
    return max(candidates, key=lambda p: p.stat().st_mtime)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("--crossval", type=Path, default=None,
                   help="cross-validation json (default: "
                        f"{CANONICAL_CROSSVAL.name}, else the newest "
                        "bench/advisor/crossval*.json that is not an ablation)")
    p.add_argument("--primary-level", default="",
                   help="budget level to quote in the captions, e.g. q0.5 "
                        "(default: the p50 budget when the file has one)")
    p.add_argument("--out-dir", type=Path, default=FIGURES_DIR,
                   help="output directory (default docs/advisor/figures)")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    fs.use("dark")
    written = int(advisor_evaluation(args.crossval or default_crossval(),
                                     args.out_dir, args.primary_level))
    print(f"\n{written}/1 figures written to {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
