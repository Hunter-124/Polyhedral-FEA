#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Which reference truths this repo is allowed to overwrite.

``bench/reference/corpus/*.json`` holds the answers every campaign is scored
against. Most of it is now INDEPENDENT of this engine: 64 references come from
Gmsh meshing the STEP plus CalculiX solving it (``external-gmsh-mesh+calculix-solver``)
and 8 are closed-form (``analytic``), with tolerances derived from measured
convergence rather than guessed. That independence is the entire value of the
corpus -- it is what makes a campaign score evidence rather than self-assessment.

Two scripts can write those files (``scripts/advisor/promote_truth.py`` and
``scripts/gen_primitive_corpus.py``), and both used to overwrite whatever was
there. The rule below is therefore an ALLOWLIST, not a denylist: only truth this
repo generated itself may be rewritten. A denylist keyed on the sources that
existed when it was written (``source == "analytic"``) silently stops protecting
anything added later -- exactly how 128 external metrics ended up one command away
from being replaced by our own overkill mesher's numbers.

Keep this the single definition. Copying the set into each caller reintroduces
the drift it exists to prevent.
"""
from __future__ import annotations

from typing import Any

#: Metric ``source`` values this repo produced itself, and may overwrite.
#: ``provisional`` is the first-order seed from scripts/gen_primitive_corpus.py;
#: ``overkill-reference`` is an earlier promotion of our own finest solve.
SELF_GENERATED_SOURCES = frozenset({"provisional", "overkill-reference"})


def protected_source(metric: dict[str, Any]) -> str | None:
    """The protected source of ``metric``, or None when it may be overwritten.

    A metric carrying no ``source`` is protected: failing closed is the only safe
    default for truth whose provenance cannot be established.
    """
    source = metric.get("source")
    if source in SELF_GENERATED_SOURCES:
        return None
    return source if isinstance(source, str) and source else "<no source field>"


def protected_metrics(reference: dict[str, Any]) -> list[tuple[str, str]]:
    """``(metric_name, source)`` for every metric in ``reference`` that is protected."""
    out: list[tuple[str, str]] = []
    for metric in reference.get("metrics", []):
        if not isinstance(metric, dict):
            continue
        source = protected_source(metric)
        if source is not None:
            out.append((str(metric.get("name", "<unnamed>")), source))
    return out


def reference_is_protected(reference: dict[str, Any]) -> bool:
    """True when any metric in ``reference`` may not be overwritten."""
    return bool(protected_metrics(reference))
