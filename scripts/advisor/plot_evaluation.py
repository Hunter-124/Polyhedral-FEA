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
axis of the sweep restates it as "times worse than the best choice".

Missing inputs print a "no data yet" note and exit 0, so this is safe to run
mid-campaign. Every number that lands in the figure is also printed.

Run from anywhere:

    python scripts/advisor/plot_evaluation.py
    python scripts/advisor/plot_evaluation.py \
        --crossval bench/advisor/crossval.json \
        --out-dir docs/advisor/figures
"""
from __future__ import annotations

import argparse
import math
import sys
import textwrap
from datetime import datetime
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import figstyle as fs  # noqa: E402
from advisor.paths import ADVISOR_DIR, REPO_ROOT  # noqa: E402
from advisor.plot_evaluation_data import (  # noqa: E402
    CHOOSERS, CLAMPS, band_levels, coincident, collapse_families, failure_rates,
    gate_threshold, head_of, hindsight_choosers, is_learned, level_quantiles, load,
    lookup_hit_rates, ordered_choosers, paired_rows, primary_level, shipped_chooser,
    sorted_levels, table, zero_case_folds,
)
from advisor.plot_evaluation_draw import (  # noqa: E402
    ALPHA, LABELS, draw_bands, draw_failures, draw_paired, draw_sweep, register_choosers,
)
from advisor.regret import decades_to_factor  # noqa: E402

#: The canonical record. Anything else is picked up only if this is absent.
CANONICAL_CROSSVAL = ADVISOR_DIR / "crossval_final.json"
#: Never auto-selected: deliberate ablations, plottable only via --crossval.
ABLATION_SUFFIXES = ("_geofeat", "_nogeo")
FIGURES_DIR = REPO_ROOT / "docs" / "advisor" / "figures"
HINDSIGHT_TAG = "it had hindsight"
#: What the SHIPPED chooser is called on the figure. The threshold is filled in
#: from clamps.json, never typed here.
SHIPPED_TAG = ("SHIPPED in C++: checks every measured candidate and blocks "
               "the risky ones")


def _fit(bits: list[str], font_pt: float, width_in: float,
         max_lines: int = 1) -> str:
    """Join caption fragments, dropping trailing ones that will not fit.

    figstyle wraps a title or subtitle to the figure width -- ``fs.figure``
    fills each paragraph at ``width_in * 72 / (0.55 * font_pt)`` columns -- so
    the real limit is a line count, not a character count. Measure with the
    same wrapper at the same width over ``max_lines`` of them, rather than a
    second hand-tuned constant: budgeting one unwrapped line silently threw
    away findings that had room to wrap, and dropping an honest result to a
    stale character count is itself a claim the figure did not earn. The cap
    stays, though, because an unbounded caption grows down into the axes.
    """
    columns = max(24, int(width_in * 72.0 / (0.55 * font_pt)))
    text = ""
    for bit in bits:
        candidate = bit if not text else f"{text}; {bit}"
        if len(textwrap.wrap(candidate, columns)) > max_lines and text:
            break
        text = candidate
    return text


def budget_phrase(level: str, q: dict[str, float]) -> str:
    """The words for one budget level: ``q0.5`` -> ``the median budget``.

    The level ids stay spelled as the record spells them in the console
    tables; prose gets the percentile a reader can picture instead.
    """
    if level == "unconstrained":
        return "no budget cap at all"
    value = q.get(level, math.inf)
    if not math.isfinite(value):
        return f"the {level} budget"
    if math.isclose(value, 0.5, abs_tol=1e-9):
        return "the median budget"
    return f"the {value * 100:g}th-percentile budget"


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
             "as src/advisor/src/advisor_artifacts.cpp does; it throws when the key is "
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
            hit_note = "it never once found the held-out family"
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
            f" {headline['wins']} to {headline['losses']},"
            f" p={headline['p_value']:.2g}"
            + ("" if sig else ", too close to call"))
    # what the gate itself buys over the same predictions ranked without it:
    # state it either way, from the paired test, never from expectation
    if gate_row is not None:
        g_sig = (math.isfinite(gate_row["p_value"])
                 and gate_row["p_value"] < ALPHA)
        g_won = gate_row["wins"] > gate_row["losses"]
        title_bits.append(
            f"gate vs {ungated}: "
            + ("a real difference" if g_sig and g_won else "too close to call")
            + f" ({gate_row['wins']} to {gate_row['losses']}, p="
              f"{gate_row['p_value']:.2g})")
    title = _fit(["Advisor evaluation: " + (title_bits[0] if title_bits
                                            else "budget sweep and paired tests")]
                 + title_bits[1:], fs.FONT_PT["title"], 14.5)

    shipped_word = "shipped" if focus == shipped else "best that can ship"
    primary_words = budget_phrase(primary, q)
    bits = [f"{fs.quantity_label(head)} at {primary_words}: {shipped_word} "
            f"{focus} {focus_r:.3f} ({fs.times_off(focus_r)} off the best "
            "choice)"
            + (f", {focus_rank} of {n_distinct}" if focus_rank else "")
            + (", ranking only "
               f"{tab[ungated].get(primary, {}).get('macro_mean_regret', float('nan')):.3f}"
               if ungated else "")
            + (", its own pick "
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
            + (f" ({hs_row['wins']} to {hs_row['losses']}, p="
               f"{hs_row['p_value']:.2g}"
               + ("" if decided else ", too close to call") + ")"
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
        bits.append(_pairs("clearly worse than", lost_pairs, show=1))
    if focus_band_rank:
        bits.append(f"with spending matched, {focus_band_rank} of "
                    f"{len(band_rank)}"
                    + (", the worst chooser" if focus_band_worst
                       else f", behind {band_best}"))
    if ns_pairs:
        bits.append(_pairs("too close to call against", ns_pairs))
    subtitle = _fit(bits, fs.FONT_PT["subtitle"], 14.5, max_lines=3)

    zero_note = "; ".join(
        f"cross-validation group {f} ({', '.join(g) or 'unnamed'}) scored 0 "
        "cases and is excluded from every average" for f, g in zeros)
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
        note=f"leave-one-{split_mode}-out, {n_folds_used} scorable "
             f"cross-validation groups x {n_seeds} random starts, scored on "
             f"{fs.quantity_label(head)} ('{head}')"
             + (f" | {zero_note}" if zero_note else "") + stamp
             + (f"\n{stale}" if stale else ""))

    seed_max = max((tab[c][lv]["mean_seed_std"] for c in choosers for lv in tab[c]),
                   default=0.0)
    sweep_notes = [
        f"bars = spread over the {n_folds_used} groups"
        + (f"; random starts move a point {seed_max:.3f} "
           f"({fs.times_off(seed_max)})" if seed_max > 0
           else "; every random start gives the same numbers")]
    twin_note = ""
    if twin:
        twin_note = (f"{twin[1]} coincides exactly with {twin[0]}"
                     + (f" — {hit_note}, so it always falls back"
                        if hit_note else ""))
    hindsight_note = ""
    if hindsight:
        hindsight_note = (", ".join(sorted(hindsight)) + " is grayed out: "
                          f"{HINDSIGHT_TAG} — it ranked on cost measured "
                          "afterwards, so it can never pick a failure")
    for rep, members in groups.items():
        spread = [tab[m].get(primary, {}).get("macro_mean_regret", float("nan"))
                  for m in members]
        others = [m for m in members if m != rep]
        lead = (f"{rep} drawn thick: shipped gate "
                f"(clamps.json:{gate_key})"
                if rep == shipped else
                f"{rep} is the best of the sweep at {primary_words}; the "
                "shipped threshold is not in this record")
        sweep_notes.append(
            f"{lead}. The band = the other {len(others)}, all within "
            f"{max(spread) - min(spread):.3f} "
            f"({fs.times_off(max(spread) - min(spread))}) at {primary_words}, "
            "so the threshold is not the result")
    if missing:
        sweep_notes.append("absent from this file: " + ", ".join(missing))
    # 39 columns, not 44: the key occupies the right of the same empty band,
    # and at 44 the widest lines ran under its handles once the record grew to
    # nine choosers. Measured off the rendered PNG, not guessed.
    sweep_note = "\n".join(textwrap.fill(line, 39, subsequent_indent="   ")
                           for line in sweep_notes)
    paired_note = ("n = cases where one rule beat the other; a star marks a "
                   f"real difference (p below {ALPHA:g})")
    if rank_row is not None:
        r_sig = (math.isfinite(rank_row["p_value"])
                 and rank_row["p_value"] < ALPHA
                 and rank_row["wins"] > rank_row["losses"])
        paired_note += "\n" + textwrap.fill(
            f"ranking alone ({ungated}) "
            + ("also beats" if r_sig else "does not beat")
            + f" {rank_row['reference']} "
              f"({rank_row['wins']} to {rank_row['losses']}, "
              f"p={rank_row['p_value']:.2g})"
            + (f", and {focus} vs {ungated} is "
               f"{gate_row['wins']} to {gate_row['losses']} with "
               f"{gate_row['ties']} ties (p={gate_row['p_value']:.2g})"
               + (": the gate adds nothing over ranking alone"
                  if not (math.isfinite(gate_row["p_value"])
                          and gate_row["p_value"] < ALPHA
                          and gate_row["wins"] > gate_row["losses"])
                  else ": the gate still adds something over ranking alone")
               if gate_row is not None else ""), 96)

    band_note = ""
    if focus_band_rank and band_rank:
        dep_rank = (band_deployable.index(focus) + 1
                    if focus in band_deployable else None)
        ahead_of = (band_rank[focus_band_rank - 2]
                    if focus_band_rank >= 2 else None)
        band_note = textwrap.fill(
            f"with spending matched, {focus} ranks {focus_band_rank} of "
            f"{len(band_rank)}"
            + (" — worst of all" if focus_band_worst else "")
            + (f" ({dep_rank} of {len(band_deployable)} that can ship)"
               if dep_rank else "")
            + (f", behind {ahead_of}" if ahead_of else "")
            + ": most of the win is choosing how much to spend, not the "
              "per-case choice",
            58)
    gate_notes = []
    for rep in groups:
        base = ungated or "advisor_argmin"
        if rep in rates and base in rates:
            base_r = tab[base].get(primary, {}).get("macro_mean_regret",
                                                    float("nan"))
            rep_r = tab[rep].get(primary, {}).get("macro_mean_regret",
                                                  float("nan"))
            gate_notes.append(
                f"{rep} cuts failing picks from "
                f"{rates[base]['mean'] * 100:.1f}% ({base}) to "
                f"{rates[rep]['mean'] * 100:.1f}%, averaged over budgets "
                "— a tight budget leaves fewer choices, so one alone "
                f"reads 2-3 points lower — and at {primary_words} from "
                f"{base_r:.3f} to {rep_r:.3f} ({fs.times_off(rep_r)})")
    failure_note = "\n".join(textwrap.fill(line, 62) for line in [
        "a rule you could ship must sometimes pick an action that fails; "
        "0% means it ranked on results it could not have known"]
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
        axes[0][1].text(0.5, 0.5, "no matched-spending bands in this file",
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
        axes[1][1].text(0.5, 0.5,
                        f"no {fs.quantity_label('pick_failure_rate')} "
                        "in this file",
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
