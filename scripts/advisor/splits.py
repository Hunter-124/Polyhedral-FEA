# SPDX-License-Identifier: BSD-3-Clause
"""Leakage-resistant hold-out grouping for the advisor dataset.

The corpus is parametric -- ``box_hole_s0_c0`` and ``box_hole_s0_c1`` are the
same CAD solid under a different load case -- so rows are never split
individually: every row of a held-out GROUP goes to validation.
"""

from __future__ import annotations

import re
from typing import Any

import numpy as np

#: parts are named ``<family>_s<shape>_c<case>``; ``_s\d+_c\d+`` is the suffix
#: that distinguishes a shape variant and a load case of one family.
PART_SUFFIX_RE = re.compile(r"_s\d+_c\d+$")
CASE_SUFFIX_RE = re.compile(r"_c\d+$")

#: How ``dataset.load_dataset`` groups parts before holding a fold out.
#:
#: ``family``   all shape variants and load cases of one base geometry, e.g.
#:              every ``box_hole_*``. This is the only split that measures
#:              generalization to an unseen geometry family, and it is the
#:              default because it is the only defensible one.
#: ``geometry`` one CAD solid, its load cases held together, e.g. every
#:              ``box_hole_s0_*``. Weaker: a held-out geometry still has its
#:              siblings from the same family in train.
#: ``part``     one (geometry, load case) pair. Measured on the v3 corpus,
#:              *every* held-out row then has a row in train with an identical
#:              geometry-feature and action vector, so this mode exists only to
#:              reproduce the leakage it causes and must never ship a number.
SPLIT_MODES: tuple[str, ...] = ("family", "geometry", "part")
DEFAULT_SPLIT_MODE = "family"


def family_of(part: str) -> str:
    """``box_hole_s0_c1`` -> ``box_hole``."""
    return PART_SUFFIX_RE.sub("", part)


def geometry_of(part: str) -> str:
    """``box_hole_s0_c1`` -> ``box_hole_s0`` (one CAD solid, any load case)."""
    return CASE_SUFFIX_RE.sub("", part)


def group_of(part: str, mode: str = DEFAULT_SPLIT_MODE) -> str:
    """The hold-out group a part belongs to under ``mode``."""
    if mode == "family":
        return family_of(part)
    if mode == "geometry":
        return geometry_of(part)
    if mode == "part":
        return part
    raise ValueError(f"unknown split mode {mode!r}; expected one of {SPLIT_MODES}")


def split_groups(parts: list[str], mode: str = DEFAULT_SPLIT_MODE) -> list[str]:
    """Sorted unique hold-out groups present in ``parts``."""
    return sorted({group_of(part, mode) for part in parts})


def fold_groups(groups: list[str], fold: int, n_folds: int) -> list[str]:
    """The groups held out by ``fold``, dealt round-robin over sorted groups.

    Round-robin rather than contiguous blocks: contiguous blocks over an
    alphabetically sorted list would put related families in one fold the
    moment the corpus grows names like ``box_hole`` / ``box_slot``.
    """
    if not groups:
        return []
    n_folds = max(1, min(int(n_folds), len(groups)))
    fold = int(fold) % n_folds
    return [group for i, group in enumerate(groups) if i % n_folds == fold]


def split_mask(parts: list[str], split: str = DEFAULT_SPLIT_MODE,
               fold: int = 0, n_folds: int | None = None) -> np.ndarray:
    """Boolean mask selecting the validation rows of ``fold``.

    Every row of a held-out group goes to validation, so no geometry -- and
    under the default ``family`` mode no *relative* of a geometry -- straddles
    the split.
    """
    groups = split_groups(parts, split)
    if len(groups) < 2:
        raise SystemExit(
            f"split mode {split!r} yields {len(groups)} group(s); at least 2 are "
            "needed to hold one out"
        )
    total = len(groups) if n_folds is None else int(n_folds)
    held = set(fold_groups(groups, fold, total))
    return np.asarray([group_of(part, split) in held for part in parts], dtype=bool)


def add_split_args(parser: Any) -> None:
    """Attach the ``--split`` / ``--fold`` / ``--n-folds`` trio to a parser.

    Shared so every entry point that reads the dataset describes the hold-out
    the same way and cannot quietly disagree with the others.
    """
    parser.add_argument("--split", choices=list(SPLIT_MODES), default=DEFAULT_SPLIT_MODE,
                        help="hold-out grouping (default: family, the only leakage-safe one)")
    parser.add_argument("--fold", type=int, default=0,
                        help="which group block to hold out (default: 0)")
    parser.add_argument("--n-folds", type=int, default=None,
                        help="fold count (default: one fold per group, i.e. leave-one-out)")
