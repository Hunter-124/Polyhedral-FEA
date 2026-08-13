#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Re-derive the peer matrix against the CURRENT references, from its stored probes.

This produces an EXPECTATION, never a measurement. Each row of
``bench/results/gmsh-peer.json`` records the raw probe value it measured and the
truth it was scored against, so ``rel_err`` can be recomputed against a
replacement truth without re-solving anything. That is useful for exactly one
purpose: to have a baseline to diff a re-run against, so movement in the re-run
can be ATTRIBUTED instead of merely observed.

  * a gmsh-mesh row whose probe moves in the re-run isolates a SOLVER change,
    because the mesh comes from Gmsh and Gmsh has not changed;
  * a native or graded row whose probe moves isolates a MESHER change;
  * a row whose probe is unchanged but whose rel_err moves is purely the truth
    replacement, and this file already tells you that part.

The re-derived numbers are NOT fit to publish as results: 96 of the 144 rows were
produced by our own mesher before the feature-aware classification, load-area
rescale and load-rule asymmetry fixes, and the remaining 48 were solved by our
own CG before its true-residual/acceptance change. Scoring stale probes against
new truth would report stale physics with a fresh-looking number.

usage: rederive_peer_baseline.py [--peer <in.json>] [--out <out.json>]
                                 [--baseline-ref <git-rev>]
"""
from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
#: The baseline is derived from the ARCHIVED pre-engine-fix snapshot, not from the
#: live matrix: once the sweep overwrites bench/results/gmsh-peer.json, deriving
#: from it would produce an "expectation" out of the new numbers, which is
#: meaningless. Both paths sit under bench/results/archive/ so the scoreboard's
#: bench/results/*.json glob cannot reach them.
PEER_PATH = ROOT / "bench" / "results" / "archive" / "gmsh-peer.pre-enginefix.json"
OUT_PATH = ROOT / "bench" / "results" / "archive" / "gmsh-peer-expected.json"
REF_DIR = ROOT / "bench" / "reference" / "corpus"
PROBE_RE = re.compile(r"probe=([0-9.eE+-]+)")
TRUTH_RE = re.compile(r"truth=([0-9.eE+-]+)")


def load_json(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"))


def reference_metric(case_id: str, name: str, revision: str | None = None) -> dict | None:
    """The named metric of a reference, from the working tree or a git revision."""
    relative = f"bench/reference/corpus/{case_id}.json"
    if revision:
        shown = subprocess.run(
            ["git", "show", f"{revision}:{relative}"], cwd=ROOT, capture_output=True, text=True
        )
        if shown.returncode != 0:
            return None
        document = json.loads(shown.stdout)
    else:
        document = load_json(ROOT / relative)
    for metric in document["metrics"]:
        if metric["name"] == name:
            return metric | {"_truth_source": document.get("truth_source")}
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--peer", type=Path, default=PEER_PATH)
    parser.add_argument("--out", type=Path, default=OUT_PATH)
    parser.add_argument("--baseline-ref", default="895f46b~1",
                        help="git revision holding the references the peer file was scored "
                             "against (default: the commit before truth was replaced)")
    args = parser.parse_args()

    rows = load_json(args.peer)
    if not isinstance(rows, list) or not rows:
        print(f"error: {args.peer}: expected a non-empty row array", file=sys.stderr)
        return 2

    out_rows: list[dict] = []
    census: Counter[str] = Counter()
    shifts: defaultdict[str, list[float]] = defaultdict(list)
    for row in rows:
        name = row["accuracy"]["name"].removesuffix("_rel_err")
        probe_match = PROBE_RE.search(row.get("notes", ""))
        stored_truth_match = TRUTH_RE.search(row.get("notes", ""))
        current = reference_metric(row["case_id"], name)
        previous = reference_metric(row["case_id"], name, args.baseline_ref)
        entry = {
            "solver": row["solver"],
            "case_id": row["case_id"],
            "order": row["order"],
            "h_rel": row["h_rel"],
            "label": row["label"],
            "metric": name,
            "status_in_peer_file": row.get("status", "ok"),
            "dofs": row.get("dofs"),
            "stored_rel_err": row["accuracy"]["value"],
        }
        if current is None or previous is None:
            entry["re_derivable"] = False
            entry["reason"] = "reference not found in one of the two revisions"
            census["reference missing"] += 1
            out_rows.append(entry)
            continue
        entry["truth_then"] = previous["value"]
        entry["truth_now"] = current["value"]
        entry["truth_source_now"] = current["_truth_source"]
        entry["truth_shift_rel"] = (
            None if not previous["value"] else current["value"] / previous["value"] - 1.0
        )
        if row.get("status") == "failed" or row["accuracy"]["value"] is None or not probe_match:
            entry["re_derivable"] = False
            entry["reason"] = (
                "row failed, so it recorded no probe" if row.get("status") == "failed"
                else "probe value absent or null in notes (empty probe selection)"
            )
            entry["must_re_run"] = True
            census[entry["reason"]] += 1
            out_rows.append(entry)
            continue
        probe = float(probe_match.group(1))
        entry["probe_then"] = probe
        if stored_truth_match:
            stored_truth = float(stored_truth_match.group(1))
            entry["stored_truth_in_notes"] = stored_truth
            entry["stored_truth_matches_baseline_ref"] = math.isclose(
                stored_truth, previous["value"], rel_tol=1e-6
            )
        rederived = abs(probe - current["value"]) / abs(current["value"])
        entry["rel_err_re_derived_against_current_truth"] = rederived
        entry["rel_err_change_from_truth_replacement"] = rederived - row["accuracy"]["value"]
        entry["reproduces_stored_rel_err_against_old_truth"] = math.isclose(
            abs(probe - previous["value"]) / abs(previous["value"]),
            row["accuracy"]["value"], rel_tol=1e-9, abs_tol=1e-12,
        )
        entry["re_derivable"] = True
        census["re-derived" if entry["truth_shift_rel"] else "truth identical, rel_err stands"] += 1
        shifts[row["solver"]].append(entry["rel_err_change_from_truth_replacement"])
        out_rows.append(entry)

    verified = [r for r in out_rows if r.get("reproduces_stored_rel_err_against_old_truth")
                is False]
    payload = {
        # bench/competitive/render_scoreboard.py globs bench/results/*.json and plots
        # every schema-valid row it finds. A `schema` key makes it skip this document
        # on its FIRST check rather than incidentally, for whichever future reader
        # copies this file somewhere the glob can reach.
        "schema": "polymesh.gmsh-peer-baseline.v1",
        "not_a_result_set": (
            "An EXPECTATION re-derived from archived probes, not a measurement. Never "
            "plot it as results: it exists only so a re-run's movement can be attributed."
        ),
        "purpose": "EXPECTATION derived from the stored probes of "
        f"{args.peer.relative_to(ROOT).as_posix()}, re-scored against the current references. "
        "Not a measurement, and not fit to publish as results: it is the baseline a re-run "
        "is diffed against so movement can be attributed to the mesher, the solver or the "
        "truth replacement.",
        "attribution_key": {
            "gmsh-mesh+polymesh-solver": "mesh is Gmsh and unchanged, so probe movement in a "
            "re-run isolates our SOLVER (CG true-residual/acceptance)",
            "polymesh-native": "probe movement isolates our default MESHER (feature-aware "
            "classification, load-rule asymmetry)",
            "polymesh-native-graded": "probe movement isolates the GRADED mesher",
            "rel_err movement with an unchanged probe": "purely the truth replacement, already "
            "quantified in this file",
        },
        "peer_file": args.peer.relative_to(ROOT).as_posix(),
        "peer_file_rows": len(rows),
        "baseline_reference_revision": args.baseline_ref,
        "generated": datetime.now(timezone.utc).isoformat(),
        "census": dict(census),
        "rows_needing_a_re_run_regardless": sum(
            1 for r in out_rows if r.get("must_re_run") or not r.get("re_derivable")
        ),
        "consistency_check": {
            "rows_that_fail_to_reproduce_their_stored_rel_err": len(verified),
            "note": "every re-derivable row must reproduce its stored rel_err when scored "
            "against the baseline revision's truth; a non-zero count here would mean the "
            "peer file and that revision disagree",
        },
        "expected_rel_err_change_by_solver": {
            solver: {
                "n": len(values),
                "min": min(values),
                "max": max(values),
                "mean": sum(values) / len(values),
            }
            for solver, values in sorted(shifts.items())
        },
        "rows": out_rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    print(f"peer rows read: {len(rows)}")
    for key, count in census.most_common():
        print(f"  {key:<52}{count:>4}")
    print(f"rows needing a re-run regardless: {payload['rows_needing_a_re_run_regardless']}")
    print(f"rows failing their consistency check: {len(verified)}")
    for solver, stats in payload["expected_rel_err_change_by_solver"].items():
        print(f"  expected rel_err change {solver:<32} n={stats['n']:<3} "
              f"min {stats['min']:+.4f} max {stats['max']:+.4f} mean {stats['mean']:+.4f}")
    print("wrote", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
