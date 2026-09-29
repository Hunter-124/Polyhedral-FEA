# SPDX-License-Identifier: BSD-3-Clause
"""Reading side of ``plot_evaluation.py``: the cross-validation record, queried.

Nothing here draws. Every level, chooser and pair set is discovered from the
record, never assumed, because the sweep keeps growing.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

import numpy as np

from .paths import ADVISOR_DIR

#: The shipped chooser's gate threshold is read from here, so the figure
#: cannot disagree with the binary (src/advisor/src/advisor_artifacts.cpp reads
#: the same key and throws when it is absent).
CLAMPS = ADVISOR_DIR / "clamps.json"

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


def gate_threshold() -> tuple[float | None, str | None]:
    """The gate threshold the C++ chooser uses, and the key it came from.

    ``src/advisor/src/advisor_artifacts.cpp`` reads ``gate_threshold`` STRICTLY and
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
