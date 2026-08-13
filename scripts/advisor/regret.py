#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Decision scoring for the mesh advisor: choosers, regret, budgets, tests.

Every other advisor metric scores a *prediction*. This module scores a
*decision*, which is the only thing the advisor exists to make, and it is
shared by :mod:`evaluate` (one checkpoint) and :mod:`crossval` (folds x seeds)
so the two can never drift into reporting different numbers.

Why the budget is the point
---------------------------
Under a pure-accuracy objective the oracle is not a chooser at all: accuracy is
very nearly monotone in resolution, so the best action is whichever candidate
spends the most, at *any* grid we could build. Measured on the v3 corpus, 51 of
63 cases had the finest offered ``h_rel`` as their accuracy oracle. Regret
against that oracle therefore rewards "spend more" and cannot distinguish a
model that learned mesh economics from one that learned to output the smallest
size it is allowed to.

The decision only becomes real when cost binds. So the primary metric here is
regret under a **budget**: every chooser, and the oracle it is scored against,
may only pick actions whose cost is within the budget, and the budget is swept.

The budget axis is ``dof`` by default and that default is deliberate. Wall time
is machine-, thread- and run-dependent, and a regret number whose *constraint*
is noisy is not reproducible evidence. Active DOF is deterministic, engine
independent, and it is the unit the adaptive-FEM literature reports.

Units
-----
All outcome columns arrive as ``log10`` values (see ``dataset.TARGET_SOURCES``),
so a regret of 0.30 means "0.30 decades = 2x worse than the best action this
case could have had". :func:`decades_to_factor` converts for reporting.
"""
from __future__ import annotations

import math
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Any, Callable, Sequence

import numpy as np

#: Outcome columns a chooser can be scored on. Lower is better for all of them.
#: ``efficiency`` is first because it is the only one of these that poses a real
#: decision; see :data:`DERIVED_HEADS`.
SCORED_HEADS: list[str] = ["efficiency", "rel_err", "geo_p99", "solve_ms"]

#: Outcomes computed from two log10 columns by addition, i.e. a product of the
#: underlying quantities.
#:
#: ``efficiency = log10(rel_err) + log10(dof) = log10(rel_err x DOF)`` is
#: accuracy per degree of freedom, the same quantity README.md:192 already scores
#: the external Gmsh comparison on.
#:
#: This exists because accuracy alone is not a decision. Accuracy is very nearly
#: monotone in resolution, so the accuracy-optimal action is whichever candidate
#: spends most: measured on the v3 corpus it is the finest offered ``h_rel`` in
#: 51 of 63 cases. Constraining the budget helps but does not fix it -- inside a
#: budget, "spend all of it" is still near-optimal. Under ``efficiency`` the
#: degeneracy disappears: the optimum spreads over every rung (27 cases at
#: h_rel 0.12, 22 at 0.20, 14 at 0.16) and coincides with "spend everything" in
#: exactly 1 of 63 cases.
DERIVED_HEADS: dict[str, tuple[str, str]] = {"efficiency": ("rel_err", "dof")}

#: Cost axes a budget can be expressed in. ``dof`` is primary; see module doc.
BUDGET_HEADS: list[str] = ["dof", "solve_ms"]

#: Budget levels, as a quantile of the case's own candidate cost distribution.
#: Case-relative rather than absolute because a 5,000-DOF budget is generous
#: for a stepped shaft and impossible for a sphere-in-box, and a single
#: absolute budget would silently score the two on different problems.
#:
#: Swept finely because the question is whether a learned per-case policy ever
#: overtakes "spend the whole budget", and a crossover can only be located by
#: looking across the range rather than at one level.
BUDGET_QUANTILES: tuple[float, ...] = (0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.75, 0.9, 1.0)

#: Relative-error targets for the "DOF to reach this accuracy" metric.
DOF_TARGETS: tuple[float, ...] = (0.1, 0.05, 0.01)

#: Fewest measured actions a case needs before its regret means anything.
MIN_ACTIONS = 3


def decades_to_factor(decades: float) -> float:
    """``0.30`` decades -> ``2.0x`` worse. NaN passes through."""
    return float("nan") if not math.isfinite(decades) else float(10.0 ** decades)


# --------------------------------------------------------------------------- #
# case tables
# --------------------------------------------------------------------------- #

@dataclass
class Case:
    """One held-out case and every campaign action measured for it.

    ``rows`` indexes into the split arrays. ``failed`` marks candidates the
    engine could not deliver at all. Whether those are candidates is a choice
    with consequences: excluding them (the default, and what every regret number
    in this project used to do) means a chooser is never charged for picking an
    action that fails, which flatters exactly the strategies that reach for the
    most expensive action. :func:`build_cases` can include them, and
    :func:`score` then reports ``pick_failure_rate`` alongside regret.
    """

    part: str
    group: str
    rows: list[int]
    outcomes: dict[str, np.ndarray] = field(default_factory=dict)
    valid: dict[str, np.ndarray] = field(default_factory=dict)
    failed: np.ndarray = field(default_factory=lambda: np.zeros(0, dtype=bool))

    @property
    def n_actions(self) -> int:
        return len(self.rows)

    def best(self, head: str, feasible: np.ndarray | None = None) -> float:
        """Best (lowest) measured value of ``head`` among feasible candidates."""
        mask = self.valid[head]
        if feasible is not None:
            mask = mask & feasible
        return float(self.outcomes[head][mask].min()) if mask.any() else float("nan")

    def worst(self, head: str, feasible: np.ndarray | None = None) -> float:
        """Worst measured value, used as the floor charged for a failed pick."""
        mask = self.valid[head]
        if feasible is not None:
            mask = mask & feasible
        return float(self.outcomes[head][mask].max()) if mask.any() else float("nan")


def build_cases(split: Any, group_of: Callable[[str], str],
                min_actions: int = MIN_ACTIONS,
                include_failures: bool = False) -> list[Case]:
    """Group a :class:`~advisor.dataset.Split` into per-case candidate tables.

    ``min_actions`` guards the degenerate case: with fewer than three measured
    actions "regret against the best" is nearly vacuous, and the resulting
    number would be averaged in beside cases carrying real evidence.

    ``include_failures`` admits actions the engine could not deliver. They carry
    no outcome, so they can never be the oracle's pick, but a chooser may select
    one and be charged for it.
    """
    by_part: dict[str, list[int]] = defaultdict(list)
    for i, part in enumerate(split.parts):
        by_part[part].append(i)

    failure = np.asarray(split.failure, dtype=np.float64)
    cases: list[Case] = []
    for part, rows in sorted(by_part.items()):
        scored = [i for i in rows if split.masks["rel_err"][i]]
        if len(scored) < min_actions:
            continue
        usable = rows if include_failures else scored
        case = Case(part=part, group=group_of(part), rows=list(usable),
                    failed=failure[np.asarray(usable, dtype=int)] > 0.0)
        for head, values in split.targets.items():
            case.outcomes[head] = np.asarray(values, dtype=np.float64)[usable]
            case.valid[head] = np.asarray(split.masks[head], dtype=bool)[usable]

        # Derived efficiency outcome: log10(rel_err) + log10(dof) = log10(rel_err
        # x DOF), the "accuracy per degree of freedom" the README already scores
        # the Gmsh comparison on (README.md:192). It is derived rather than
        # predicted, so no head has to be added, and it is the objective that
        # makes the choice non-degenerate: measured on the v3 corpus, the
        # accuracy optimum sits at the finest offered h_rel in 51 of 63 cases,
        # while the efficiency optimum is spread across every rung and coincides
        # with "spend everything" in 1 case of 63.
        for head, (a, b) in DERIVED_HEADS.items():
            if a in case.outcomes and b in case.outcomes:
                case.outcomes[head] = case.outcomes[a] + case.outcomes[b]
                case.valid[head] = case.valid[a] & case.valid[b]
        cases.append(case)
    return cases


# --------------------------------------------------------------------------- #
# feasibility
# --------------------------------------------------------------------------- #

def budget_levels(case: Case, budget_head: str,
                  quantiles: Sequence[float] = BUDGET_QUANTILES) -> list[float]:
    """Case-relative budget levels, as quantiles of its own candidate costs."""
    mask = case.valid[budget_head]
    if not mask.any():
        return []
    costs = case.outcomes[budget_head][mask]
    return [float(np.quantile(costs, q)) for q in quantiles]


def feasible_mask(case: Case, budget_head: str, budget: float | None,
                  floor: float | None = None,
                  allow_failed: bool = False) -> np.ndarray:
    """Candidates whose cost lies in ``(floor, budget]``.

    ``budget=None`` is unconstrained. Supplying ``floor`` makes the window
    two-sided, which is what a MATCHED-COST comparison needs: inside a narrow
    band every candidate costs about the same, so "spend the whole budget" stops
    being a strategy and what remains is judgement about which action -- mesher,
    order, grading -- is best at a given price. That is the question a user with
    a fixed compute budget actually faces.

    A candidate whose cost was never measured is infeasible, not free: we cannot
    certify it fits, and admitting it would let unmeasured rows dominate the
    tight budgets.

    ``allow_failed`` keeps actions the engine could not deliver in the candidate
    set. They have no measured cost, so no budget filter can apply to them --
    and that is the point: a real chooser cannot know an action will fail before
    running it, so excluding them from the offer would grade every policy on a
    decision it never actually faces. The oracle can still never pick one,
    because oracle selection additionally requires a valid outcome.
    """
    if budget is None and floor is None:
        return np.ones(case.n_actions, dtype=bool)
    mask = case.valid[budget_head]
    if budget is not None:
        mask = mask & (case.outcomes[budget_head] <= budget + 1e-12)
    if floor is not None:
        mask = mask & (case.outcomes[budget_head] >= floor - 1e-12)
    if allow_failed and case.failed.size == case.n_actions:
        mask = mask | case.failed
    return mask


# --------------------------------------------------------------------------- #
# choosers
# --------------------------------------------------------------------------- #

#: A chooser maps (case, feasible mask) to the index *within* ``case.rows`` it
#: picks, or ``None`` when it cannot choose (nothing feasible).
Chooser = Callable[[Case, np.ndarray], "int | None"]


def _restrict(case: Case, feasible: np.ndarray, head: str) -> np.ndarray:
    return feasible & case.valid[head]


def oracle_chooser(head: str) -> Chooser:
    """Picks the truly best feasible action. Regret 0 by definition."""

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        mask = _restrict(case, feasible, head)
        if not mask.any():
            return None
        values = np.where(mask, case.outcomes[head], np.inf)
        return int(np.argmin(values))

    return choose


def score_chooser(scores: dict[str, np.ndarray], lower_is_better: bool = True) -> Chooser:
    """Picks the feasible action optimising a per-row score, e.g. a prediction.

    ``scores`` is keyed by part so one dict serves every case of a split.
    """

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        if not feasible.any():
            return None
        value = np.asarray(scores[case.part], dtype=np.float64)
        value = value if lower_is_better else -value
        value = np.where(feasible & np.isfinite(value), value, np.inf)
        return None if not np.isfinite(value).any() else int(np.argmin(value))

    return choose

def gated_score_chooser(scores: dict[str, np.ndarray],
                        failure_probability: dict[str, np.ndarray],
                        threshold: float) -> Chooser:
    """Argmin of a predicted outcome, over candidates the model expects to survive.

    The advisor already has a trained feasibility head, and today it is spent on
    a single 0.5 veto applied *after* the action is chosen
    (``src/advisor/src/advisor.cpp:437``). That ordering cannot prevent a bad
    pick, only refuse it wholesale and fall back to defaults. Measured, the
    ungated argmin selects an action that fails outright 22.7 % of the time at a
    median DOF budget, against 3.3 % for the trivial "go finer" rule -- so the
    head is being wasted exactly where it could help.

    Filtering first and ranking second uses the same two heads in the order that
    can actually change the decision. If every candidate is predicted to fail the
    gate is dropped rather than returning nothing: refusing to choose is the
    veto's job, and conflating the two would make this chooser unmeasurable
    against the ungated one on the cases that matter most.
    """

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        if not feasible.any():
            return None
        risk = np.asarray(failure_probability[case.part], dtype=np.float64)
        survivors = feasible & (risk <= threshold)
        if not survivors.any():
            survivors = feasible
        value = np.asarray(scores[case.part], dtype=np.float64)
        value = np.where(survivors & np.isfinite(value), value, np.inf)
        return None if not np.isfinite(value).any() else int(np.argmin(value))

    return choose


def nearest_action_chooser(targets: dict[str, np.ndarray],
                           action_matrix: dict[str, np.ndarray]) -> Chooser:
    """Picks the feasible action closest to a requested action vector.

    This is what scores the *shipped* policy: the C++ advisor emits an action,
    not a ranking, and the campaign only measured a finite grid, so the
    faithful way to score it is "which measured action did it ask for".
    Distance is in standardized action space, so a dial with a wide range
    cannot dominate the match.
    """

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        if not feasible.any():
            return None
        want = np.asarray(targets[case.part], dtype=np.float64)
        have = np.asarray(action_matrix[case.part], dtype=np.float64)
        distance = np.linalg.norm(have - want[None, :], axis=1)
        distance = np.where(feasible, distance, np.inf)
        return int(np.argmin(distance))

    return choose


def finest_action_chooser(action_matrix: dict[str, np.ndarray],
                          h_rel_index: int) -> Chooser:
    """Picks the feasible action with the smallest ``h_rel``: "mesh as fine as allowed".

    The realistic form of "spend everything". ``spend_budget_chooser`` ranks by
    MEASURED cost, which no deployable rule could do -- you cannot know an
    action's DOF count before meshing, and you certainly cannot know that it
    will fail. This one ranks by the action itself, so it can and does select
    actions that turn out to fail, exactly as a real "just go finer" heuristic
    would. Comparing the two isolates how much of the trivial rule's apparent
    strength comes from hindsight.
    """

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        if not feasible.any():
            return None
        h = np.asarray(action_matrix[case.part], dtype=np.float64)[:, h_rel_index]
        value = np.where(feasible, h, np.inf)
        return None if not np.isfinite(value).any() else int(np.argmin(value))

    return choose

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        if not feasible.any():
            return None
        want = np.asarray(targets[case.part], dtype=np.float64)
        have = np.asarray(action_matrix[case.part], dtype=np.float64)
        distance = np.linalg.norm(have - want[None, :], axis=1)
        distance = np.where(feasible, distance, np.inf)
        return int(np.argmin(distance))

    return choose


def fixed_action_chooser(action: np.ndarray,
                         action_matrix: dict[str, np.ndarray]) -> Chooser:
    """A single constant configuration, applied to every case.

    The baseline a learned policy has to beat to have earned its existence: a
    zero-parameter chooser fitted once on the training split.
    """

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        if not feasible.any():
            return None
        have = np.asarray(action_matrix[case.part], dtype=np.float64)
        distance = np.linalg.norm(have - action[None, :], axis=1)
        distance = np.where(feasible, distance, np.inf)
        return int(np.argmin(distance))

    return choose


def spend_budget_chooser(budget_head: str = "dof") -> Chooser:
    """Picks the most expensive feasible action: "spend the whole budget".

    This is the baseline the corpus itself forces on us. Accuracy is very nearly
    monotone in resolution (measured ``rel_err ~ h^+0.80``), and the coarse
    rungs are also where the mesher refuses -- so the finest feasible action is
    usually both the most accurate and the only one left. That makes
    "spend everything you are allowed to" a one-line heuristic with no
    parameters, no features and no training.

    A learned advisor that cannot beat this under a budget has not learned mesh
    economics; it has learned to output the smallest size it is permitted. This
    chooser exists so that claim is tested rather than assumed.
    """

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        mask = _restrict(case, feasible, budget_head)
        if not mask.any():
            return None
        values = np.where(mask, case.outcomes[budget_head], -np.inf)
        return int(np.argmax(values))

    return choose


def random_chooser(seed: int) -> Chooser:
    """Uniform over feasible actions. Calibrates how much the action even matters."""
    generator = np.random.default_rng(seed)

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        index = np.flatnonzero(feasible)
        return None if index.size == 0 else int(generator.choice(index))

    return choose


# --------------------------------------------------------------------------- #
# fitting the constant / lookup baselines on the training split
# --------------------------------------------------------------------------- #

def fit_constant_action(cases: Sequence[Case], action_matrix: dict[str, np.ndarray],
                        head: str = "rel_err") -> tuple[np.ndarray | None, float]:
    """The single action with the lowest mean regret over ``cases``.

    Fitted on the *training* cases only, then applied unchanged to the held-out
    fold, so it is a real baseline and not an oracle in disguise.
    """
    candidates: dict[tuple[float, ...], list[float]] = defaultdict(list)
    for case in cases:
        mask = case.valid[head]
        if not mask.any():
            continue
        best = float(case.outcomes[head][mask].min())
        actions = np.asarray(action_matrix[case.part], dtype=np.float64)
        for i in np.flatnonzero(mask):
            candidates[tuple(actions[i])].append(float(case.outcomes[head][i]) - best)

    if not candidates:
        return None, float("nan")
    # Coverage matters: an action measured on two cases can look perfect by
    # accident, so require it on at least half the cases before it may win.
    floor = max(1, len(cases) // 2)
    eligible = {k: v for k, v in candidates.items() if len(v) >= floor}
    pool = eligible or candidates
    best_action = min(pool, key=lambda k: float(np.mean(pool[k])))
    return np.asarray(best_action, dtype=np.float64), float(np.mean(pool[best_action]))


def fit_group_actions(cases: Sequence[Case], action_matrix: dict[str, np.ndarray],
                      head: str = "rel_err") -> dict[str, np.ndarray]:
    """Best constant action *per group*, i.e. a lookup table keyed by family."""
    by_group: dict[str, list[Case]] = defaultdict(list)
    for case in cases:
        by_group[case.group].append(case)
    table: dict[str, np.ndarray] = {}
    for group, members in by_group.items():
        action, _ = fit_constant_action(members, action_matrix, head)
        if action is not None:
            table[group] = action
    return table


def group_lookup_chooser(table: dict[str, np.ndarray], fallback: np.ndarray | None,
                         action_matrix: dict[str, np.ndarray]) -> tuple[Chooser, Callable[[], float]]:
    """Per-group best action, falling back to the global constant when unseen.

    Under a leave-one-family-out split the held-out family is by construction
    absent from the table, so this baseline degenerates to the constant one and
    the returned hit-rate is 0. That is not a bug to paper over: it is the
    honest statement that a lookup table cannot address an unseen family, and
    the hit rate is reported so nobody reads the two rows as independent.
    """
    hits = [0, 0]

    def choose(case: Case, feasible: np.ndarray) -> int | None:
        if not feasible.any():
            return None
        hits[1] += 1
        action = table.get(case.group)
        if action is None:
            action = fallback
        else:
            hits[0] += 1
        if action is None:
            return None
        return fixed_action_chooser(action, action_matrix)(case, feasible)

    def hit_rate() -> float:
        return float(hits[0] / hits[1]) if hits[1] else float("nan")

    return choose, hit_rate


# --------------------------------------------------------------------------- #
# scoring
# --------------------------------------------------------------------------- #

def score(cases: Sequence[Case], choosers: dict[str, Chooser],
          heads: Sequence[str] = SCORED_HEADS,
          budget_head: str = "dof",
          budget_quantiles: Sequence[float] | None = BUDGET_QUANTILES,
          bands: Sequence[tuple[float, float]] | None = None,
          allow_failed: bool = False,
          charge_failures: bool = True) -> dict[str, Any]:
    """Regret of every chooser, unconstrained, per budget level, and per cost band.

    ``bands`` are ``(floor_quantile, ceiling_quantile)`` pairs producing
    MATCHED-COST windows, inside which every candidate costs about the same and
    only judgement separates them.

    ``allow_failed`` offers actions the engine could not deliver. When such an
    action is picked and ``charge_failures`` is set, the chooser is charged the
    worst measured outcome in that case -- a deliberate LOWER BOUND on the true
    cost, since a failed run actually yields nothing and forces a retry. The
    honest figure to read alongside it is ``pick_failure_rate``, which needs no
    imputation at all.
    """
    levels: list[tuple[str, float | None, float | None]] = [("unconstrained", None, None)]
    if budget_quantiles:
        levels += [(f"q{q:g}", q, None) for q in budget_quantiles]
    if bands:
        levels += [(f"band{lo:g}-{hi:g}", hi, lo) for lo, hi in bands]

    out: dict[str, Any] = {
        "budget_head": budget_head,
        "n_cases": len(cases),
        "allow_failed": allow_failed,
        "levels": {},
    }

    for label, quantile, floor_quantile in levels:
        per_head: dict[str, dict[str, list[float]]] = {
            head: defaultdict(list) for head in heads
        }
        picked_failed: dict[str, list[int]] = defaultdict(list)
        n_scored = 0
        for case in cases:
            mask = case.valid[budget_head]
            budget = floor = None
            if quantile is not None:
                if not mask.any():
                    continue
                costs = case.outcomes[budget_head][mask]
                budget = float(np.quantile(costs, quantile))
                if floor_quantile is not None:
                    floor = float(np.quantile(costs, floor_quantile))
            feasible = feasible_mask(case, budget_head, budget, floor, allow_failed)
            if not feasible.any():
                continue
            n_scored += 1

            picks = {name: chooser(case, feasible) for name, chooser in choosers.items()}
            for name, pick in picks.items():
                if pick is not None and case.failed.size and bool(case.failed[pick]):
                    picked_failed[name].append(1)
                elif pick is not None:
                    picked_failed[name].append(0)

            for head in heads:
                best = case.best(head, feasible)
                if not math.isfinite(best):
                    continue
                penalty = case.worst(head, feasible)
                for name, pick in picks.items():
                    if pick is None:
                        continue
                    if case.valid[head][pick]:
                        per_head[head][name].append(float(case.outcomes[head][pick]) - best)
                    elif charge_failures and case.failed.size and bool(case.failed[pick]) \
                            and math.isfinite(penalty):
                        per_head[head][name].append(float(penalty) - best)

        out["levels"][label] = {
            "quantile": quantile,
            "floor_quantile": floor_quantile,
            "n_cases": n_scored,
            "pick_failure_rate": {
                name: float(np.mean(values)) for name, values in sorted(picked_failed.items())
                if values
            },
            "heads": {
                head: {
                    name: {
                        "mean_regret": float(np.mean(values)),
                        "median_regret": float(np.median(values)),
                        "n": len(values),
                    }
                    for name, values in sorted(chooser_values.items()) if values
                }
                for head, chooser_values in per_head.items()
            },
        }
    return out


def paired_regrets(cases: Sequence[Case], a: Chooser, b: Chooser, head: str,
                   budget_head: str = "dof",
                   quantile: float | None = None,
                   allow_failed: bool = False,
                   charge_failures: bool = True) -> tuple[list[float], list[float]]:
    """Per-case regret of two choosers on exactly the cases both could score.

    ``allow_failed`` and ``charge_failures`` MUST match whatever :func:`score`
    was called with. They did not, once: the paired test silently ran on a
    candidate set with no failing actions in it, where a feasibility gate has
    nothing to filter, and duly reported that the gated and ungated choosers
    agreed on all 300 cases while the regret table showed them 0.09 decades
    apart. A paired test on a different candidate set than the table it
    accompanies is worse than no test at all.
    """
    left: list[float] = []
    right: list[float] = []
    for case in cases:
        if quantile is None:
            budget = None
        else:
            mask = case.valid[budget_head]
            if not mask.any():
                continue
            budget = float(np.quantile(case.outcomes[budget_head][mask], quantile))
        feasible = feasible_mask(case, budget_head, budget, None, allow_failed)
        if not feasible.any():
            continue
        best = case.best(head, feasible)
        pick_a, pick_b = a(case, feasible), b(case, feasible)
        if pick_a is None or pick_b is None or not math.isfinite(best):
            continue
        penalty = case.worst(head, feasible)

        def outcome(pick: int) -> float | None:
            if case.valid[head][pick]:
                return float(case.outcomes[head][pick]) - best
            if charge_failures and case.failed.size and bool(case.failed[pick]) \
                    and math.isfinite(penalty):
                return float(penalty) - best
            return None

        value_a, value_b = outcome(pick_a), outcome(pick_b)
        if value_a is None or value_b is None:
            continue
        left.append(value_a)
        right.append(value_b)
    return left, right


def sign_test(left: Sequence[float], right: Sequence[float],
              tolerance: float = 1e-9) -> dict[str, Any]:
    """Exact two-sided sign test: does ``left`` beat ``right`` per case?

    A mean over a dozen cases hides whether one catastrophe carried it. The
    sign test asks the question a reviewer actually has -- "does it win *more
    often*" -- and its exact binomial p-value needs no scipy.
    """
    wins = losses = ties = 0
    for a, b in zip(left, right):
        if abs(a - b) <= tolerance:
            ties += 1
        elif a < b:
            wins += 1
        else:
            losses += 1
    n = wins + losses
    if n == 0:
        p = float("nan")
    else:
        extreme = min(wins, losses)
        tail = sum(math.comb(n, k) for k in range(extreme + 1)) / (2.0 ** n)
        p = min(1.0, 2.0 * tail)
    return {"wins": wins, "losses": losses, "ties": ties, "n_paired": n, "p_value": p}


def dof_to_target(cases: Sequence[Case], choosers: dict[str, Chooser],
                  targets: Sequence[float] = DOF_TARGETS) -> dict[str, Any]:
    """Active DOF each chooser spends to reach a relative-error target.

    The interpretable metric: "how many degrees of freedom does this policy
    need to get within 1 % ?". Reported as the median over cases of
    ``log10(dof)``, plus how often the chooser failed to reach the target at
    all -- a censored case is a real outcome and is counted, never dropped.
    """
    out: dict[str, Any] = {}
    for target in targets:
        log_target = math.log10(target)
        rows: dict[str, dict[str, Any]] = {}
        for name, chooser in choosers.items():
            spend: list[float] = []
            reached = attempted = 0
            for case in cases:
                ok = case.valid["rel_err"] & case.valid["dof"]
                if not ok.any() or not (case.outcomes["rel_err"][ok] <= log_target).any():
                    continue  # no action reaches the target; the case cannot test it
                attempted += 1
                feasible = np.ones(case.n_actions, dtype=bool)
                pick = chooser(case, feasible)
                if pick is None or not (case.valid["rel_err"][pick] and case.valid["dof"][pick]):
                    continue
                if case.outcomes["rel_err"][pick] <= log_target:
                    reached += 1
                    spend.append(float(case.outcomes["dof"][pick]))
            rows[name] = {
                "median_log10_dof": float(np.median(spend)) if spend else float("nan"),
                "reached": reached,
                "attempted": attempted,
                "reach_rate": float(reached / attempted) if attempted else float("nan"),
            }
        out[f"rel_err<={target:g}"] = rows
    return out
