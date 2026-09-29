# SPDX-License-Identifier: BSD-3-Clause
"""The advisor's action space (contract C4): vocabulary, clamp box, encoding.

Builds the policy head's ``action_dims`` layout and the ``clamps.json``
payload, including the measured candidate grid the C++ chooser enumerates.
"""

from __future__ import annotations

import math
from typing import Any

import numpy as np

try:
    from .csv_io import to_float
except ImportError:  # direct `python scripts/advisor/dataset.py`
    from csv_io import to_float

#: Only orders 1 and 2 exist. ``fea::promote_to_quadratic``
#: (``src/fea/include/fea/p_elevate.hpp``) is a single linear->quadratic
#: step, and the CLI solve command (``apps/cli/commands_solve.cpp``) warns and
#: downgrades anything higher. The vocabulary previously advertised 3 and 4, so
#: the policy head spent two of its ten outputs on actions that could never be
#: performed.
ORDER_CHOICES: list[int] = [1, 2]
CONTINUOUS_ACTION_DIMS: list[str] = ["h_rel", "adapt_passes", "eta_target"]
CLAMP_BOX: dict[str, tuple[float, float]] = {
    "h_rel": (0.005, 0.28),
    # The plan's floor of 0.005 excluded eta_target's own default. In the
    # harness 0.0 means "no adaptive error target", which is a legal and common
    # action (every adapt_passes=0 row uses it), so the box has to contain it —
    # otherwise the clamp would silently turn adaptivity on.
    "eta_target": (0.0, 0.3),
    "adapt_passes": (0.0, 6.0),
}
#: Refuses the whole recommendation after the fact — an abstention.
VETO_THRESHOLD = 0.5
#: Drops individual candidates before ranking. A DIFFERENT decision from the
#: veto, and deliberately a different number: the C++ used to inherit this from
#: `veto_threshold` when the key was absent, which shipped the gate at 0.5 —
#: the weakest member of its own sweep — while looking deliberate. The C++ now
#: rejects a clamps.json that omits it.
#:
#: 0.05 is chosen on pick-failure rate, not regret. Across thresholds 0.05–0.8
#: held-out regret spans only 0.3233–0.3350 (leave-one-family-out, 8 families,
#: 5 seeds), so regret does not single out any threshold — 0.2 is nominally best
#: by 0.012 decades. Pick-failure does separate them: 27.5 % at 0.05 against
#: 31.3 % at 0.2 and 31.2 % at 0.5. Given the failure head's calibration is
#: mediocre (ECE 0.263), a rule that avoids doomed picks is worth more to a user
#: than a hair of median accuracy inside the noise band.
GATE_THRESHOLD = 0.05
ACTION_DEFAULTS: dict[str, Any] = {
    "mesher": "hybrid_zoo",
    "h_rel": 0.1,
    "order": 1,
    "adapt_passes": 0,
    "eta_target": 0.0,
}


def build_action_dims(mesher_choices: list[str]) -> list[str]:
    """The policy head's output layout: 3 continuous dims then two argmax blocks.

    Width is ``3 + len(ORDER_CHOICES) + len(mesher_choices)``. A
    ``p_elevate_logit`` and two unreachable order logits were dropped from it.
    """
    dims = ["h_rel", "adapt_passes", "eta_target"]
    dims += [f"order_logit_{value}" for value in ORDER_CHOICES]
    dims += [f"mesher_logit_{name}" for name in mesher_choices]
    return dims


def action_group_slices(mesher_choices: list[str]) -> dict[str, slice]:
    """Index ranges of each logical group inside the action vector."""
    n_continuous = len(CONTINUOUS_ACTION_DIMS)
    n_order = len(ORDER_CHOICES)
    n_mesher = len(mesher_choices)
    return {
        "continuous": slice(0, n_continuous),
        "order": slice(n_continuous, n_continuous + n_order),
        "mesher": slice(n_continuous + n_order, n_continuous + n_order + n_mesher),
    }


def candidate_grid(rows: list[dict[str, str]], mesher_choices: list[str],
                   max_candidates: int = 128) -> dict[str, Any]:
    """The explicit list of actions a deployed chooser enumerates and scores.

    A LIST of measured actions, not a cross product of per-dial levels. The
    difference matters twice over.

    First, a cross product invents combinations. The corpus runs
    ``adapt_passes = 0`` only at ``h_rel = 0.12``, so crossing the dials would
    manufacture "no adaptivity at h_rel = 0.08" and ask the regression heads to
    extrapolate to it. That is the failure mode that produced
    ``predicted_dof = 1.5e15`` on an unseen part. Every action here was actually
    run, so no query leaves the training support.

    Second, it lets provably inert dials be collapsed. Measured on this corpus,
    ``order`` has NO effect when ``adapt_passes > 0``: of 264 matched pairs
    differing only in ``order``, 264 are bit-identical in ``n_dof``, ``n_nodes``
    and ``rel_err``, because that path takes the adaptive driver's marked p-set
    and never consults ``cfg.order`` (the adaptive path of ``SolveJob``,
    ``src/pipeline/src/solve_job.cpp``). Those candidates are duplicates and are
    dropped -- 26 distinct measured tuples collapse to 20. ``eta_target`` is NOT collapsed: at 193 of 237 matched pairs
    it is inert too, but not always, so dropping it would discard real actions.

    ``max_candidates`` is a latency budget: each action costs one forward pass in
    the C++ chooser, against roughly 2 for the retired single-shot rule.
    """
    seen: dict[tuple[Any, ...], int] = {}
    for row in rows:
        mesher = str(row.get("mesher", "") or "").strip()
        if mesher not in mesher_choices:
            continue
        order = to_float(row.get("order"))
        h_rel = to_float(row.get("h_rel"))
        passes = to_float(row.get("adapt_passes"))
        eta = to_float(row.get("eta_target"))
        if not all(math.isfinite(v) for v in (order, h_rel, passes, eta)):
            continue
        order_int = int(round(order))
        passes_int = int(round(passes))
        if order_int not in ORDER_CHOICES:
            continue
        # Collapse the inert order dial rather than scoring duplicate actions.
        if passes_int > 0:
            order_int = ORDER_CHOICES[0]
        key = (mesher, order_int, round(h_rel, 6), passes_int, round(eta, 6))
        seen[key] = seen.get(key, 0) + 1

    actions = [
        {"mesher": m, "order": o, "h_rel": h, "adapt_passes": p, "eta_target": e,
         "measured_rows": n}
        for (m, o, h, p, e), n in sorted(seen.items(), key=lambda kv: kv[0])
    ]
    grid: dict[str, Any] = {
        "actions": actions,
        "n_candidates": len(actions),
        "order_collapsed_when_adapt_passes_positive": True,
        "collapse_evidence": ("264 of 264 matched pairs differing only in order at "
                              "adapt_passes > 0 are bit-identical in n_dof, n_nodes "
                              "and rel_err"),
        # Kept for readability and for the figures generator; the C++ side reads
        # `actions`, never these.
        "observed_levels": {
            "h_rel": sorted({a["h_rel"] for a in actions}),
            "adapt_passes": sorted({a["adapt_passes"] for a in actions}),
            "eta_target": sorted({a["eta_target"] for a in actions}),
            "order": sorted({a["order"] for a in actions}),
            "mesher": sorted({a["mesher"] for a in actions}),
        },
    }
    if not actions:
        raise SystemExit("candidate grid is empty; the dataset has no usable actions")
    if len(actions) > max_candidates:
        raise SystemExit(
            f"candidate grid has {len(actions)} actions, over the {max_candidates} "
            "ceiling; each one costs a forward pass in the C++ chooser"
        )
    return grid


def clamp_table(mesher_choices: list[str],
                rows: list[dict[str, str]] | None = None) -> dict[str, Any]:
    """The C4 ``clamps.json`` payload."""
    # The default action is what a feasibility veto falls back to, so it has to
    # be a legal action for THIS model: a default mesher absent from the trained
    # vocabulary would hand the C++ side a name its own clamp table rejects.
    defaults = dict(ACTION_DEFAULTS)
    if defaults["mesher"] not in mesher_choices:
        defaults["mesher"] = mesher_choices[0]
    if defaults["order"] not in ORDER_CHOICES:
        defaults["order"] = ORDER_CHOICES[0]
    lo, hi = CLAMP_BOX["h_rel"]
    defaults["h_rel"] = min(max(float(defaults["h_rel"]), lo), hi)
    lo, hi = CLAMP_BOX["eta_target"]
    defaults["eta_target"] = min(max(float(defaults["eta_target"]), lo), hi)
    return {
        "h_rel": list(CLAMP_BOX["h_rel"]),
        "eta_target": list(CLAMP_BOX["eta_target"]),
        "adapt_passes": [int(CLAMP_BOX["adapt_passes"][0]), int(CLAMP_BOX["adapt_passes"][1])],
        "order_choices": list(ORDER_CHOICES),
        "mesher_choices": list(mesher_choices),
        "action_dims": build_action_dims(mesher_choices),
        "veto_threshold": VETO_THRESHOLD,
        "gate_threshold": GATE_THRESHOLD,
        "candidate_grid": candidate_grid(rows, mesher_choices) if rows else None,
        "defaults": defaults,
    }


def continuous_box_halfwidths() -> np.ndarray:
    """Barrier half-widths for the three continuous policy dims.

    The penalty is ``beta * sum(relu(|value| - halfwidth))``, i.e. a box
    centred on the origin, so the half-width of dim *d* is
    ``max(|lo|, |hi|)`` of its clamp interval.
    """
    return np.asarray(
        [max(abs(CLAMP_BOX[name][0]), abs(CLAMP_BOX[name][1])) for name in CONTINUOUS_ACTION_DIMS],
        dtype=np.float32,
    )
