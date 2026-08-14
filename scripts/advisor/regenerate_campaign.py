#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Re-measure every advisor campaign row, one generation at a time.

A generation is retired whenever a mesher change moves the labels. Rows are
never corrected in place: the whole generation is archived under
``bench/campaigns/archive-<generation>/`` and re-measured from the STEP files,
because a corpus half-measured on two different meshers puts the difference
inside a training fold where nothing can see it.

Generations so far:

* ``v3`` — the untangle cycle ending at ``798ef79``. The S7 overlapped-sheet
  carve alone moved sphere_box_s0 from rel_err 9.9e-04 to 1.1e-04.
* ``v4`` — ADR-0032. The mesher no longer depends on the standard library's
  hash order, which changed the answer on some parts, and labelling moves from
  the laptop's MSVC build to gcc on the Linux boxes.

This driver only sequences :mod:`scripts.advisor.run_batch`, which owns dedup,
sharding and the truth gate. It exists so a regeneration is one resumable
command instead of six hand-typed ones, and so the batch order is recorded:
the pilot grid first (it is the widest and shakes out any refusal that the new
mesher introduces), then the four follow-up grids.

Resume is free. ``run_batch`` skips any ``(part, cfg_id)`` already recorded
under ``bench/campaigns/advisor-*``, so re-running after a crash re-plans
against whatever landed.

Two machines can share one regeneration: give each a disjoint ``--parts-glob``
and its own ``--host-tag``, then merge the campaign directories. Dedup reads
every ``advisor-*`` directory, so the merged union is what the next plan sees.

    python scripts/advisor/regenerate_campaign.py --archive v4 --dry-run
    python scripts/advisor/regenerate_campaign.py --archive v4 \\
        --host-tag hunter-pc --shards 4 --omp-threads 3
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN_BATCH = ROOT / "scripts" / "advisor" / "run_batch.py"
CAMPAIGNS = ROOT / "bench" / "campaigns"

# (batch number, template, parts glob or None to use the template's own parts)
#
# Stage 4's template is reconstructed from the archived shard campaigns: the v3
# run drove batch 4 from the default template with an overridden grid and never
# wrote one. There is no stage 5 — batch 5 was listed here before it existed and
# never produced a row.
STAGES: list[tuple[int, str, str | None]] = [
    (1, "bench/campaigns/archive-v2/advisor-pilot-1/campaign.json", None),
    (2, "bench/campaigns/advisor-batch-2-template/campaign.json", None),
    (3, "bench/campaigns/advisor-batch-3-template/campaign.json", None),
    (4, "bench/campaigns/advisor-batch-4-template/campaign.json", None),
]


def stage_argv(batch: int, template: str, parts_glob: str | None,
               args: argparse.Namespace) -> list[str]:
    argv = [sys.executable, str(RUN_BATCH), "--batch", str(batch),
            "--campaign-template", template,
            "--shards", str(args.shards), "--omp-threads", str(args.omp_threads)]
    glob = args.parts_glob or parts_glob
    if glob:
        argv += ["--parts-glob", glob]
    if args.host_tag:
        argv += ["--host-tag", args.host_tag]
    if args.dry_run:
        argv.append("--dry-run")
    return argv


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--archive", required=True,
                        help="generation label of the ARCHIVED rows, e.g. v4; "
                             "bench/campaigns/archive-<label> must already exist")
    parser.add_argument("--parts-glob", default=None,
                        help="repo-relative glob of case jsons, overriding every stage's "
                             "own parts. This is how two machines split one regeneration")
    parser.add_argument("--host-tag", default="",
                        help="campaign directory suffix, so two machines writing into the "
                             "same repo cannot collide (passed through to run_batch)")
    parser.add_argument("--shards", type=int, default=4)
    parser.add_argument("--omp-threads", type=int, default=2)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--from-stage", type=int, default=1,
                        help="skip stages with a batch number below this")
    args = parser.parse_args(argv)

    archive = CAMPAIGNS / f"archive-{args.archive}"
    if not archive.is_dir():
        raise SystemExit(
            f"{archive} absent: the retired generation must be moved out of the "
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
            stage_argv(batch, template, parts_glob, args),
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
