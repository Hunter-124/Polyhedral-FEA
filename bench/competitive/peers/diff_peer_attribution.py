#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Attribute movement between the re-run peer matrix and the pre-engine-fix baseline.

The point of keeping the baseline is that movement can be ASSIGNED to a cause
instead of merely observed:

  * gmsh-mesh rows use a Gmsh mesh, and Gmsh did not change, so a moved PROBE
    isolates our SOLVER (the CG true-residual/acceptance change);
  * native and graded rows moved probe isolates our MESHER (feature-aware
    classification, load-rule asymmetry, load-area rescale);
  * a row whose probe is unchanged but whose rel_err moved is purely the truth
    replacement, which the baseline already quantified;
  * a row that previously reported ok with NO measurement, or failed outright,
    is tracked into what it became: an honest refusal, a real measurement, or
    still a failure.

The uniform variant has no baseline by construction: it is a new discretisation
that did not exist in the old matrix, so it is summarised on its own terms
against the order-2 Gmsh rows it is meant to be comparable with.

usage: diff_peer_attribution.py [--new <matrix.json>] [--baseline <expected.json>]
                                [--out <report.json>]
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NEW_PATH = ROOT / "bench" / "results" / "gmsh-peer.json"
BASELINE_PATH = ROOT / "bench" / "results" / "archive" / "gmsh-peer-expected.json"
OUT_PATH = ROOT / "bench" / "results" / "archive" / "gmsh-peer-attribution.json"
PROBE_RE = re.compile(r"probe=([0-9.eE+-]+)")
#: Relative probe change below which a row is called unmoved. Well above float
#: noise, well below anything a mesher or solver change would produce.
MOVED_THRESHOLD = 1e-9


def family_of(case_id: str) -> str:
    return case_id.rsplit("_s", 1)[0]


def probe_of(row: dict) -> float | None:
    match = PROBE_RE.search(row.get("notes", ""))
    return float(match.group(1)) if match else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--new", type=Path, default=NEW_PATH)
    parser.add_argument("--baseline", type=Path, default=BASELINE_PATH)
    parser.add_argument("--out", type=Path, default=OUT_PATH)
    args = parser.parse_args()

    new_rows = json.loads(args.new.read_text(encoding="utf-8"))
    baseline = json.loads(args.baseline.read_text(encoding="utf-8"))["rows"]
    key = lambda r: (r["solver"], r["case_id"], r["order"], r["h_rel"])
    before = {key(r): r for r in baseline}

    records = []
    for row in new_rows:
        identity = key(row)
        old = before.get(identity)
        probe_now = probe_of(row)
        record = {
            "solver": row["solver"],
            "case_id": row["case_id"],
            "family": family_of(row["case_id"]),
            "order": row["order"],
            "h_rel": row["h_rel"],
            "status_now": row.get("status", "ok"),
            "rel_err_now": row["accuracy"]["value"],
            "probe_now": probe_now,
            "dofs_now": row.get("dofs"),
            "has_baseline": old is not None,
        }
        if row.get("refusal"):
            record["refusal_kind"] = row["refusal"]["kind"]
            record["recommended_h_m"] = row["refusal"].get("recommended_h_m")
        if row.get("promotion"):
            promotion = row["promotion"]
            record["promotion"] = {
                k: promotion.get(k) for k in
                ("mode_reported", "mode_matches_request", "n_elements_promoted",
                 "n_promoted_in_result", "n_rejected_by_promotion",
                 "all_passed_elements_promoted")
            }
        if old is not None:
            record["status_then"] = old.get("status_in_peer_file", "ok")
            record["rel_err_then_old_truth"] = old.get("stored_rel_err")
            record["rel_err_then_current_truth"] = old.get(
                "rel_err_re_derived_against_current_truth"
            )
            record["probe_then"] = old.get("probe_then")
            record["was_measurable"] = old.get("re_derivable", False)
            probe_then = old.get("probe_then")
            if probe_now is not None and probe_then:
                change = abs(probe_now - probe_then) / abs(probe_then)
                record["probe_rel_change"] = change
                record["probe_moved"] = change > MOVED_THRESHOLD
            # what the truth replacement alone would have predicted
            expected = old.get("rel_err_re_derived_against_current_truth")
            if expected is not None and record["rel_err_now"] is not None:
                record["rel_err_vs_expected"] = record["rel_err_now"] - expected
            record["attribution"] = (
                "solver (mesh is Gmsh and unchanged)"
                if row["solver"] == "gmsh-mesh+polymesh-solver"
                else "mesher"
            ) if record.get("probe_moved") else (
                "truth replacement only" if record["rel_err_now"] is not None else "n/a"
            )
        records.append(record)

    # --- conversions of the previously unmeasurable rows -------------------- #
    conversions: Counter[str] = Counter()
    conversion_rows = []
    for record in records:
        if not record.get("has_baseline"):
            continue
        was_bad = record.get("status_then") == "failed" or not record.get("was_measurable")
        if not was_bad:
            continue
        became = (
            "honest refusal" if record["status_now"] == "refused"
            else "real measurement" if record["rel_err_now"] is not None
            else f"still {record['status_now']}"
        )
        conversions[became] += 1
        conversion_rows.append({**{k: record[k] for k in
                                   ("solver", "case_id", "order", "h_rel", "status_then")},
                                "became": became,
                                "status_now": record["status_now"],
                                "rel_err_now": record["rel_err_now"],
                                "refusal_kind": record.get("refusal_kind"),
                                "recommended_h_m": record.get("recommended_h_m")})

    # --- movement census by solver ------------------------------------------ #
    movement = {}
    for solver in sorted({r["solver"] for r in records}):
        rows = [r for r in records if r["solver"] == solver and "probe_rel_change" in r]
        moved = [r for r in rows if r["probe_moved"]]
        movement[solver] = {
            "rows_comparable": len(rows),
            "rows_with_moved_probe": len(moved),
            "isolates": ("our solver: the mesh is Gmsh and unchanged"
                         if solver == "gmsh-mesh+polymesh-solver"
                         else "our mesher" if rows else "no baseline (new variant)"),
            "probe_change_median": statistics.median(
                [r["probe_rel_change"] for r in moved]) if moved else None,
            "probe_change_max": max([r["probe_rel_change"] for r in moved]) if moved else None,
        }

    # --- headline accuracy by family and order ----------------------------- #
    headline: dict = {}
    for order in sorted({r["order"] for r in records}):
        for family in sorted({r["family"] for r in records}):
            for solver in sorted({r["solver"] for r in records}):
                values = [r["rel_err_now"] for r in records
                          if r["order"] == order and r["family"] == family
                          and r["solver"] == solver and r["rel_err_now"] is not None]
                if not values:
                    continue
                headline.setdefault(f"order{order}", {}).setdefault(family, {})[solver] = {
                    "n": len(values),
                    "median_rel_err": statistics.median(values),
                    "best_rel_err": min(values),
                }

    payload = {
        "schema": "polymesh.gmsh-peer-attribution.v1",
        "not_a_result_set": "An attribution report, not measurements. Lives in archive/ so "
                            "the scoreboard glob cannot reach it.",
        "new_matrix": args.new.relative_to(ROOT).as_posix(),
        "baseline": args.baseline.relative_to(ROOT).as_posix(),
        "reading_guide": "Order-1 rows are the clean cross-variant comparison. Order-2 rows "
                         "for native/graded use the shipping SELECTIVE promotion and are "
                         "therefore a mixed discretisation, so they are indicative only; the "
                         "uniform variant is the order-2 parity comparison.",
        "row_counts": {
            "new": len(new_rows),
            "baseline": len(baseline),
            "matched_to_a_baseline_row": sum(1 for r in records if r["has_baseline"]),
            "new_without_baseline": sum(1 for r in records if not r["has_baseline"]),
        },
        "status_census_now": dict(Counter(r["status_now"] for r in records)),
        "refusal_census_by_kind": dict(Counter(
            r["refusal_kind"] for r in records if r.get("refusal_kind"))),
        "movement_by_solver": movement,
        "conversions_of_previously_unmeasurable_rows": dict(conversions),
        "conversion_detail": conversion_rows,
        "headline_by_order_family_solver": headline,
        "rows": records,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    print(f"new rows {len(new_rows)}, baseline rows {len(baseline)}, "
          f"matched {payload['row_counts']['matched_to_a_baseline_row']}")
    print("status now:", payload["status_census_now"])
    print("refusals by kind:", payload["refusal_census_by_kind"] or "none")
    print("\nmovement (probe change isolates the cause):")
    for solver, stats in movement.items():
        median = stats["probe_change_median"]
        print(f"  {solver:<34}{stats['rows_with_moved_probe']:>3}/{stats['rows_comparable']:<3} "
              f"moved | median {('%.3e' % median) if median else '-':>9} | {stats['isolates']}")
    print("\nconversions of the previously unmeasurable rows:", dict(conversions))
    print("\nheadline median rel_err by order/family/solver:")
    for order, families in headline.items():
        for family, solvers in families.items():
            parts = ", ".join(f"{s.replace('polymesh-','').replace('gmsh-mesh+polymesh-solver','gmsh')}"
                              f"={v['median_rel_err']:.4f}" for s, v in solvers.items())
            print(f"  {order:<7}{family:<18}{parts}")
    print("\nwrote", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
