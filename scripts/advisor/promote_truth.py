#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Promote overkill reference answers into the corpus truth files, once and forever.

``bench/campaigns/advisor-truth-0`` solves every procedural-corpus case at
``order = 2 / adapt_passes = 3`` over a three-rung ``h_rel`` ladder (see
``TRUTH_H_REL_LADDER`` in ``scripts/gen_primitive_corpus.py`` for why the reference is a
ladder and not one very fine run). The run is sharded, so this script reads the union of
that directory's ``results.jsonl`` and every ``advisor-truth-0-s*/results.jsonl``, then
rewrites the matching ``bench/reference/corpus/<part>.json`` metrics so subsequent (cheap)
campaigns score against the finest converged answer each part could afford, instead of
the first-order surrogate that ``scripts/gen_primitive_corpus.py`` seeds.

Rules:

* Metrics whose ``source`` is ``"analytic"`` are never touched. A closed form outranks
  any solve.
* The promoted row is the health-ok row with the largest ``n_dof`` for that part, so the
  result depends only on the campaign contents -- re-running is a byte-identical no-op.
* The truth run is executed as shards (``<campaign>-s0..-sN``, same grid, disjoint part
  lists) that are never merged back, so all of their ``results.jsonl`` files are read as
  one pool; a part solved in a shard is not reported as provisional.
* A promoted metric's ``tol`` becomes ``PROMOTED_TOL`` (0.15), never the provisional
  seed's 1.0, which would leave the campaign accuracy score nearly insensitive.
* Nothing about the promoted value depends on the clock, so truth is computed once and
  cached in git.

Run from the repo root::

    python scripts/advisor/promote_truth.py
    python scripts/advisor/promote_truth.py --dry-run
    python scripts/advisor/promote_truth.py --require-all
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[2]
CAMPAIGNS = ROOT / "bench" / "campaigns"
REFERENCE_DIR = ROOT / "bench" / "reference" / "corpus"

#: Tolerance stamped on a promoted metric. The provisional seeds carry ``tol = 1.0``,
#: which makes the campaign accuracy *score* nearly insensitive (``rel_err``, the thing
#: the advisor trains on, is unaffected either way). 0.15 is the band the analytic
#: ``stepped_shaft`` cantilever references already use, and an overkill reference is
#: itself a discrete solve rather than a converged limit, so nothing tighter than the
#: hand-calc band is defensible here.
PROMOTED_TOL = 0.15


def rel(path: Path) -> str:
    """Repo-relative POSIX path for logs and derivation strings."""
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def truth_results_paths(campaign: str) -> list[Path]:
    """Every ``results.jsonl`` of the truth run: the campaign plus its shards.

    The truth run is sharded into ``<campaign>-s0..-sN`` (same grid, disjoint part
    lists, one ``results.jsonl`` each) and nothing merges them back, so reading only
    the parent directory reports parts as provisional that were in fact solved. Same
    glob style as ``run_batch.completed_pairs``/``run_batch.truth_results_paths``.
    """
    paths: list[Path] = []
    if not CAMPAIGNS.is_dir():
        return paths
    for directory in [CAMPAIGNS / campaign, *sorted(CAMPAIGNS.glob(f"{campaign}-s*"))]:
        results = directory / "results.jsonl"
        if results.is_file():
            paths.append(results)
    return paths


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--campaign", default="advisor-truth-0",
                        help="campaign directory under bench/campaigns (default: %(default)s)")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would change; write nothing")
    parser.add_argument("--require-all", action="store_true",
                        help="exit non-zero if any non-analytic corpus truth is still "
                             "provisional after promotion")
    return parser.parse_args(argv)


def read_rows(results: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with results.open("r", encoding="utf-8") as stream:
        for line_number, text in enumerate(stream, 1):
            if not text.strip():
                continue
            try:
                rows.append(json.loads(text))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{results}:{line_number}: {exc}") from exc
    return rows


def usable(row: dict[str, Any]) -> bool:
    """Only converged, health-passing runs may define truth.

    testlab status vocabulary (apps/testlab/main.cpp): ok | solve_suspect | over_budget
    | solve_fail | mesh_fail. Only "ok" (health gates all passed) can define a truth.
    """
    if row.get("status") != "ok":
        return False
    if row.get("error"):
        return False
    health = row.get("health")
    if not isinstance(health, dict) or health.get("ok") is not True:
        return False
    measured = row.get("accuracy", {}).get("all")
    return isinstance(measured, list) and bool(measured)


def best_rows(rows: Iterable[tuple[Path, dict[str, Any]]]
              ) -> dict[str, tuple[Path, dict[str, Any]]]:
    """Highest-resolution usable row per part; ties broken by cfg_id for determinism.

    Rows arrive from the truth campaign *and* its shards, so each winner is carried
    together with the ``results.jsonl`` it came from, for provenance.
    """
    best: dict[str, tuple[Path, dict[str, Any]]] = {}
    for results, row in rows:
        part = row.get("part")
        if not isinstance(part, str) or not usable(row):
            continue
        current = best.get(part)
        key = (float(row.get("n_dof") or 0.0), str(row.get("cfg_id", "")))
        if current is None or key > (float(current[1].get("n_dof") or 0.0),
                                     str(current[1].get("cfg_id", ""))):
            best[part] = (results, row)
    return best


def measured_by_metric(row: dict[str, Any]) -> dict[str, float]:
    """`accuracy.all[]` carries the evaluate_probe() output per reference metric."""
    out: dict[str, float] = {}
    for entry in row.get("accuracy", {}).get("all", []):
        name = entry.get("metric")
        value = entry.get("value")
        if isinstance(name, str) and isinstance(value, (int, float)) \
                and entry.get("trusted") is not False:
            out[name] = float(value)
    return out


def provenance(row: dict[str, Any], results: Path) -> dict[str, Any]:
    action = row.get("action") or {}
    return {
        "campaign": results.parent.name,
        "results": rel(results),
        "cfg_id": row.get("cfg_id"),
        "tier": row.get("tier"),
        "h_rel": action.get("h_rel"),
        "order": action.get("order"),
        "adapt_passes": action.get("adapt_passes"),
        "mesher": action.get("mesher"),
        "n_dof": row.get("n_dof"),
        "n_elems": row.get("n_elems"),
    }


def promote(reference: dict[str, Any], row: dict[str, Any], results: Path
            ) -> tuple[dict[str, Any], int, int]:
    """Return (updated reference, promoted metric count, analytic metrics skipped)."""
    measured = measured_by_metric(row)
    updated = json.loads(json.dumps(reference))  # deep copy, no shared substructure
    promoted = 0
    skipped = 0
    source_run = provenance(row, results)
    for metric in updated.get("metrics", []):
        if metric.get("source") == "analytic":
            skipped += 1
            continue
        name = metric.get("name")
        if name not in measured:
            continue
        metric["value"] = measured[name]
        metric["tol"] = PROMOTED_TOL
        metric["source"] = "overkill-reference"
        metric["derivation"] = (
            f"{rel(results)} (overkill reference solve at the finest h_rel this part "
            "could afford under the campaign element/DOF budget; NOT a converged limit "
            "-- see docs/validation/hand-calcs.md#corpus-primitives-provisional). "
            f"tol={PROMOTED_TOL:g} replaces the provisional seed's tol=1.0 and matches "
            "the analytic cantilever references; the reference is itself a discrete "
            "solve, so a tighter band would score its own discretization error. "
            "Promoted by scripts/advisor/promote_truth.py."
        )
        metric["source_run"] = source_run
        metric.pop("inputs", None)
        promoted += 1
    if promoted:
        if all(m.get("source") == "overkill-reference"
               for m in updated.get("metrics", [])):
            updated["truth_source"] = "overkill-reference"
        else:
            updated["truth_source"] = "mixed"
        updated.pop("notes", None)
    return updated, promoted, skipped


def write_json(path: Path, payload: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, indent=2, allow_nan=False)
        stream.write("\n")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    results_paths = truth_results_paths(args.campaign)
    if not results_paths:
        print(f"error: no results.jsonl under bench/campaigns/{args.campaign}"
              f"[-s*]; run the {args.campaign} campaign before promoting truth",
              file=sys.stderr)
        return 1
    if not REFERENCE_DIR.is_dir():
        print(f"error: {rel(REFERENCE_DIR)} not found; run "
              "scripts/gen_primitive_corpus.py first", file=sys.stderr)
        return 1

    scanned = [(path, row) for path in results_paths for row in read_rows(path)]
    rows = best_rows(scanned)
    print(f"truth rows: {len(scanned)} row(s) from {len(results_paths)} results.jsonl "
          f"({', '.join(p.parent.name for p in results_paths)}); "
          f"{len(rows)} part(s) have a usable overkill row")
    promoted_files = 0
    promoted_metrics = 0
    analytic_skipped = 0
    unchanged = 0
    missing: list[str] = []
    still_provisional: list[str] = []

    for path in sorted(REFERENCE_DIR.glob("*.json")):
        reference = json.loads(path.read_text(encoding="utf-8"))
        part = reference.get("part", path.stem)
        entry = rows.get(part)
        if entry is None:
            if any(m.get("source") == "provisional"
                   for m in reference.get("metrics", [])):
                missing.append(part)
                still_provisional.append(part)
            else:
                analytic_skipped += sum(
                    1 for m in reference.get("metrics", [])
                    if m.get("source") == "analytic")
            continue
        source_results, row = entry
        updated, n_promoted, n_analytic = promote(reference, row, source_results)
        analytic_skipped += n_analytic
        if updated == reference:
            unchanged += 1
        else:
            promoted_files += 1
            promoted_metrics += n_promoted
            if not args.dry_run:
                write_json(path, updated)
        if any(m.get("source") == "provisional" for m in updated.get("metrics", [])):
            still_provisional.append(part)

    verb = "would promote" if args.dry_run else "promoted"
    print(f"{verb}={promoted_files} files / {promoted_metrics} metrics  "
          f"analytic_skipped={analytic_skipped}  unchanged={unchanged}  "
          f"missing={len(missing)}")
    if missing:
        print("  no usable overkill row for: " + ", ".join(missing[:8])
              + (" ..." if len(missing) > 8 else ""))
    if args.require_all and still_provisional:
        print(f"error: {len(still_provisional)} corpus truths are still provisional",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
