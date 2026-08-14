#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end assertion for the campaign artifact-write guard.

Field defect (advisor-batch-1-s3, 2026-08-13): ``polymesh_testlab`` died twice with
exit code 1, no log and no status row. Not a crash -- an artifact write threw from
inside ``run_one``'s exception handler, escaped ``run_one`` past its own catch-all, and
returned 1 from ``main``. The trigger was our own monitoring: peer processes reading
``result.json`` while the runner renamed over it, which on Windows is a
``MoveFileExW(REPLACE_EXISTING)`` sharing violation.

THE INVARIANT, in one test: when the per-run artifact cannot be written, the campaign
still records its status ROW and still exits 0. This would have caught the original bug.

Injection: a *directory* is created where ``result.json`` must go, so neither the
``.tmp`` open nor the rename can succeed. Both failure modes are confirmed to fire on
this filesystem (``PermissionError``), so the guard is genuinely exercised.

The scratch campaign is created under the system temp directory, never under
``bench/campaigns/`` -- that tree is owned by the campaign runner.

Run from the repo root, after a build::

    python scripts/verify_artifact_guard.py
    python scripts/verify_artifact_guard.py --part bench/geometries/corpus/primitives/box_hole_s0_c0.case.json
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
TESTLAB_DIR = ROOT / "build" / "apps" / "testlab"
DEFAULT_PART = "bench/geometries/corpus/primitives/box_hole_s0_c0.case.json"


def campaign_json(part: str) -> dict[str, Any]:
    """One coarse config on one part: the cheapest campaign that still writes artifacts."""
    return {
        "name": "verify-artifact-guard",
        "comment": "scratch campaign for scripts/verify_artifact_guard.py",
        # warehouse=true is what makes apps/testlab/main.cpp set wh_dir and write
        # per-run artifacts at all; without it there is nothing to obstruct.
        "warehouse": True,
        "parts": [part],
        "tiers": [{"h_scale": 1.0, "keep_frac": 1.0}],
        "grid": {
            "mesher": ["graded_tet"],
            "h_rel": [0.25],
            "order": [1],
            "adapt_passes": [0],
            "feature_refine": [False],
            "bc_grading": [False],
            # testlab rejects skin_layers < 1, so 0 made this verifier abort during
            # grid validation and never reach the artifact guard it exists to test.
            "skin_layers": [2],
        },
    }


def testlab_path() -> Path | None:
    """The newest built testlab binary, or None when the tree has no build.

    Windows builds ``polymesh_testlab.exe``, Linux builds it extensionless; this
    verifier must run on either box and must not prefer a stale name.
    """
    candidates = [
        cand
        for cand in (TESTLAB_DIR / "polymesh_testlab.exe", TESTLAB_DIR / "polymesh_testlab")
        if cand.is_file() and os.access(cand, os.X_OK)
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda c: c.stat().st_mtime)


def run_campaign(camp_dir: Path, testlab: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(testlab), "run", str(camp_dir)],
        cwd=str(ROOT), capture_output=True, text=True, check=False,
    )


def rows(camp_dir: Path) -> list[dict[str, Any]]:
    path = camp_dir / "results.jsonl"
    if not path.is_file():
        return []
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.strip():
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--part", default=DEFAULT_PART, help="case file, repo-relative")
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    args = parser.parse_args(argv)

    testlab = testlab_path()
    if testlab is None:
        print(f"error: no polymesh_testlab under {TESTLAB_DIR}; build first")
        return 2
    if not (ROOT / args.part).is_file():
        print(f"error: part {args.part} not found")
        return 2

    failures: list[str] = []

    def check(label: str, ok: bool, detail: str = "") -> None:
        print(f"  {'ok  ' if ok else 'FAIL'} {label}" + (f"  [{detail}]" if detail else ""))
        if not ok:
            failures.append(label)

    scratch = Path(tempfile.mkdtemp(prefix="polymesh_artifact_guard_"))
    try:
        # ---- Phase 1: baseline. Learn the cfg_id (an FNV-1a hash we must not
        # reimplement) and confirm the campaign writes artifacts at all.
        base = scratch / "baseline"
        base.mkdir()
        (base / "campaign.json").write_text(
            json.dumps(campaign_json(args.part), indent=2), encoding="utf-8")
        print("[1] baseline run (nothing obstructed)")
        proc = run_campaign(base, testlab)
        base_rows = rows(base)
        check("exit code 0", proc.returncode == 0, f"rc={proc.returncode}")
        check("one row recorded", len(base_rows) == 1, f"rows={len(base_rows)}")
        if not base_rows:
            print("  cannot continue without a baseline row")
            print(f"  stdout:\n{proc.stdout}\n  stderr:\n{proc.stderr}")
            return 1
        row = base_rows[0]
        cfg_id, part_name, tier = row["cfg_id"], row["part"], row["tier"]
        rel = f"runs/{cfg_id}/{part_name}/t{tier}"
        check("result.json written", (base / rel / "result.json").is_file(), rel)
        print(f"  cfg_id={cfg_id} part={part_name} tier={tier} status={row.get('status')}")

        # ---- Phase 2: THE REGRESSION. Same campaign, result.json path obstructed
        # by a directory so the artifact write must fail.
        blocked = scratch / "blocked"
        blocked.mkdir()
        (blocked / "campaign.json").write_text(
            json.dumps(campaign_json(args.part), indent=2), encoding="utf-8")
        (blocked / rel / "result.json").mkdir(parents=True)
        print("[2] obstructed run (a DIRECTORY sits where result.json must go)")
        proc2 = run_campaign(blocked, testlab)
        blocked_rows = rows(blocked)

        # The invariant, stated three ways.
        check("exit code 0 despite the artifact failure",
              proc2.returncode == 0, f"rc={proc2.returncode}")
        check("THE ROW IS STILL RECORDED", len(blocked_rows) == 1,
              f"rows={len(blocked_rows)}")
        if blocked_rows:
            check("row carries the same identity and a real status",
                  blocked_rows[0].get("cfg_id") == cfg_id
                  and blocked_rows[0].get("part") == part_name
                  and isinstance(blocked_rows[0].get("status"), str),
                  str(blocked_rows[0].get("status")))
        check("the failure was reported, not silent",
              "warehouse:" in proc2.stderr, proc2.stderr.strip().splitlines()[-1]
              if proc2.stderr.strip() else "(no stderr)")
        # Before the fix this is exactly what happened instead:
        check("no 'polymesh_testlab:' abort on stderr",
              "polymesh_testlab:" not in proc2.stderr)

        print()
        if failures:
            print(f"FAILED ({len(failures)}): " + "; ".join(failures))
            print(f"\n--- obstructed stdout ---\n{proc2.stdout}")
            print(f"--- obstructed stderr ---\n{proc2.stderr}")
            return 1
        print("artifact guard verified: artifact write failed, row survived, exit 0")
        return 0
    finally:
        if args.keep:
            print(f"scratch kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
