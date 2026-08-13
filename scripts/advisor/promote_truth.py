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

* Promotion may only overwrite truth that THIS repo generated (``provisional`` seeds
  and earlier ``overkill-reference`` promotions). Every other source is protected: a
  closed-form ``analytic`` value, any ``external-*`` chain (Gmsh mesh + CalculiX
  solve), and any third-party source added later. The rule is an ALLOWLIST, so a new
  externally sourced truth is protected the moment it lands, without editing this
  script. Overwriting a protected metric requires ``--force-overwrite-external``.
* The promoted row is the health-ok row with the largest ``n_dof`` for that part whose
  solved-stage ``geometry_volume_err`` is below 1%. Fill-stage geometry may be degraded
  before quadratic mid-node projection; only the geometry actually solved determines
  truth eligibility. The result depends only on the campaign contents -- re-running is
  a byte-identical no-op.
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
import math

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
#: A solved geometry at or above 1% relative CAD-volume error remains useful
#: advisor training data, but may not define reference truth.
GEOMETRY_VOLUME_TRUTH_LIMIT = 0.01
#: Promotion may only overwrite truth this repo generated itself. The ALLOWLIST
#: (and the reason it is not a denylist) lives in scripts/truth_guard.py so that
#: scripts/gen_primitive_corpus.py enforces exactly the same rule and a newly
#: added external source is protected in both places without editing either.
_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))
from truth_guard import SELF_GENERATED_SOURCES, protected_source  # noqa: E402


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
    parser.add_argument("--part", action="append", default=None, metavar="PART",
                        help="only promote these parts (repeatable). Naming a part "
                             "whose truth is protected is an error, not a silent skip")
    parser.add_argument("--force-overwrite-external", action="store_true",
                        help="DESTRUCTIVE: also overwrite protected truth (analytic / "
                             "external-*). Off by default; names every reference it "
                             "would clobber before writing")
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
    geometry_volume_error = row.get("geometry_volume_err")
    if geometry_volume_error is not None:
        if isinstance(geometry_volume_error, bool) or not isinstance(
                geometry_volume_error, (int, float)):
            return False
        if not math.isfinite(float(geometry_volume_error)) or \
                float(geometry_volume_error) >= GEOMETRY_VOLUME_TRUTH_LIMIT:
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


def promote(reference: dict[str, Any], row: dict[str, Any], results: Path,
            *, force_external: bool = False
            ) -> tuple[dict[str, Any], int, list[tuple[str, str]]]:
    """Return (updated reference, promoted count, protected metrics refused).

    ``protected`` lists ``(metric_name, source)`` for every metric promotion was
    NOT allowed to touch, so the caller can name them instead of counting them.
    """
    measured = measured_by_metric(row)
    updated = json.loads(json.dumps(reference))  # deep copy, no shared substructure
    promoted = 0
    protected: list[tuple[str, str]] = []
    source_run = provenance(row, results)
    for metric in updated.get("metrics", []):
        blocked = protected_source(metric)
        if blocked is not None:
            # Allowlist: only our own provisional/overkill truth is overwritable.
            if not force_external:
                protected.append((str(metric.get("name", "<unnamed>")), blocked))
                continue
            protected.append((str(metric.get("name", "<unnamed>")), blocked))
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
    return updated, promoted, protected


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
    requested: set[str] | None = set(args.part) if args.part else None
    promoted_files = 0
    promoted_metrics = 0
    unchanged = 0
    missing: list[str] = []
    still_provisional: list[str] = []
    #: part -> [(metric, source)] refused because the truth is not ours to rewrite.
    refused: dict[str, list[tuple[str, str]]] = {}

    references = [(path, json.loads(path.read_text(encoding="utf-8")))
                  for path in sorted(REFERENCE_DIR.glob("*.json"))]

    # Pre-pass: with --force-overwrite-external the user is about to destroy
    # independent truth, so name every reference BEFORE writing anything.
    if args.force_overwrite_external:
        doomed: list[tuple[str, str, str]] = []
        for path, reference in references:
            part = reference.get("part", path.stem)
            if (requested is not None and part not in requested) or part not in rows:
                continue
            for metric in reference.get("metrics", []):
                blocked = protected_source(metric)
                if blocked is not None:
                    doomed.append((rel(path), str(metric.get("name", "<unnamed>")),
                                   blocked))
        if doomed:
            print("=" * 72, file=sys.stderr)
            print("WARNING: --force-overwrite-external will REPLACE independently "
                  "sourced truth", file=sys.stderr)
            print("with this repo's own overkill-mesher values and reset tol to "
                  f"{PROMOTED_TOL:g}.", file=sys.stderr)
            print(f"{len(doomed)} protected metric(s) in "
                  f"{len({d[0] for d in doomed})} reference file(s):", file=sys.stderr)
            for ref_path, metric_name, source in doomed:
                print(f"  {ref_path}  metric={metric_name}  source={source}",
                      file=sys.stderr)
            print("=" * 72, file=sys.stderr)

    for path, reference in references:
        part = reference.get("part", path.stem)
        if requested is not None and part not in requested:
            continue
        entry = rows.get(part)
        if entry is None:
            if any(m.get("source") == "provisional"
                   for m in reference.get("metrics", [])):
                missing.append(part)
                still_provisional.append(part)
            else:
                blocked = [(str(m.get("name", "<unnamed>")), src)
                           for m in reference.get("metrics", [])
                           if (src := protected_source(m)) is not None]
                if blocked:
                    refused[part] = blocked
            continue
        source_results, row = entry
        updated, n_promoted, n_protected = promote(
            reference, row, source_results,
            force_external=args.force_overwrite_external)
        if n_protected:
            refused[part] = n_protected
        if updated == reference:
            unchanged += 1
        else:
            promoted_files += 1
            promoted_metrics += n_promoted
            if not args.dry_run:
                write_json(path, updated)
        if any(m.get("source") == "provisional" for m in updated.get("metrics", [])):
            still_provisional.append(part)

    n_refused = sum(len(items) for items in refused.values())
    verb = "would promote" if args.dry_run else "promoted"
    # Under --force the protected metrics were overwritten, not refused; say so.
    protected_label = ("protected_OVERWRITTEN" if args.force_overwrite_external
                       else "protected_refused")
    print(f"{verb}={promoted_files} files / {promoted_metrics} metrics  "
          f"{protected_label}={n_refused}  unchanged={unchanged}  "
          f"missing={len(missing)}")
    if missing:
        print("  no usable overkill row for: " + ", ".join(missing[:8])
              + (" ..." if len(missing) > 8 else ""))

    # Loud, itemised refusal: never let a protected metric be skipped silently.
    if refused and not args.force_overwrite_external:
        by_source: dict[str, int] = {}
        for items in refused.values():
            for _, source in items:
                by_source[source] = by_source.get(source, 0) + 1
        print(f"  REFUSED to overwrite {n_refused} protected metric(s) in "
              f"{len(refused)} reference file(s) -- not this repo's truth to rewrite:")
        for part in sorted(refused):
            detail = ", ".join(f"{name} [{source}]" for name, source in refused[part])
            print(f"    {part}: {detail}")
        print("  by source: "
              + ", ".join(f"{source}={count}" for source, count in sorted(by_source.items())))
        print("  (promotion may only overwrite "
              + "/".join(sorted(SELF_GENERATED_SOURCES))
              + "; pass --force-overwrite-external to override)")

    # An explicit --part naming protected truth is an error, not a silent skip.
    if requested is not None and not args.force_overwrite_external:
        explicit = sorted(requested & refused.keys())
        if explicit:
            print(f"error: --part explicitly named {len(explicit)} part(s) whose truth "
                  f"is protected: {', '.join(explicit)}", file=sys.stderr)
            return 1

    if args.require_all and still_provisional:
        print(f"error: {len(still_provisional)} corpus truths are still provisional",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
