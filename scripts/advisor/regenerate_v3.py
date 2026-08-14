#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Regenerate every advisor campaign row on the untangled mesher.

Every fix in the geometry cycle that ends at ``798ef79`` changed graded_tet
output, so no row labelled before it can be trusted: the S7 overlapped-sheet
carve alone moved sphere_box_s0 from rel_err 9.9e-04 to 1.1e-04 and deleted
self-intersecting material that had been counted as volume. The stale rows are
therefore not corrected in place -- they are archived whole under
``bench/campaigns/archive-v3/`` and re-measured from the STEP files.

This driver only sequences :mod:`scripts.advisor.run_batch`, which owns dedup,
sharding and the truth gate. It exists so the regeneration is one resumable
command instead of six hand-typed ones, and so the batch order is recorded:
the pilot grid first (it is the widest and shakes out any refusal that the new
mesher introduces), then the four follow-up grids, then the spine families.

Resume is free. ``run_batch`` skips any ``(part, cfg_id)`` already recorded
under ``bench/campaigns/advisor-*``, so re-running after a crash re-plans
against whatever landed.

    python scripts/advisor/regenerate_v3.py --dry-run
    python scripts/advisor/regenerate_v3.py
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN_BATCH = ROOT / "scripts" / "advisor" / "run_batch.py"
ARCHIVE = ROOT / "bench" / "campaigns" / "archive-v3"
ARCHIVE_V2 = ROOT / "bench" / "campaigns" / "archive-v2"

# (batch number, template, parts glob or None to use the template's own parts)
STAGES: list[tuple[int, str, str | None]] = [
    (1, "bench/campaigns/archive-v2/advisor-pilot-1/campaign.json", None),
    (2, "bench/campaigns/advisor-batch-2-template/campaign.json", None),
    (3, "bench/campaigns/advisor-batch-3-template/campaign.json", None),
    (4, "bench/campaigns/advisor-batch-4-template/campaign.json", None),
    (5, "bench/campaigns/advisor-batch-5-template/campaign.json", None),
]


def stage_argv(batch: int, template: str, parts_glob: str | None,
               shards: int, omp: int, dry_run: bool) -> list[str]:
    argv = [sys.executable, str(RUN_BATCH), "--batch", str(batch),
            "--campaign-template", template,
            "--shards", str(shards), "--omp-threads", str(omp)]
    if parts_glob:
        argv += ["--parts-glob", parts_glob]
    if dry_run:
        argv.append("--dry-run")
    return argv


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--shards", type=int, default=4)
    parser.add_argument("--omp-threads", type=int, default=2)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--from-stage", type=int, default=1,
                        help="skip stages with a batch number below this")
    args = parser.parse_args(argv)

    if not ARCHIVE.is_dir():
        raise SystemExit(
            f"{ARCHIVE} absent: the pre-798ef79 rows must be archived out of the "
            "advisor-* namespace before regeneration, or run_batch will dedup "
            "every new pair against a stale one"
        )

    started = time.time()
    for batch, template, parts_glob in STAGES:
        if batch < args.from_stage:
            print(f"=== stage {batch}: skipped")
            continue
        if not (ROOT / template).is_file():
            raise SystemExit(f"stage {batch}: template not found: {template}")
        print(f"=== stage {batch}: {template}", flush=True)
        code = subprocess.run(
            stage_argv(batch, template, parts_glob, args.shards, args.omp_threads,
                       args.dry_run),
            cwd=str(ROOT), check=False,
        ).returncode
        if code != 0:
            print(f"=== stage {batch} exited {code}; stopping", flush=True)
            return code
        print(f"=== stage {batch} done at {(time.time() - started) / 60.0:.1f} min",
              flush=True)

    print(f"=== all stages done in {(time.time() - started) / 60.0:.1f} min")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
