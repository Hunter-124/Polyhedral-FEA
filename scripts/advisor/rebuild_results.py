#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Rebuild a campaign's ``results.jsonl`` from its per-run ``result.json`` artifacts.

``apps/testlab/main.cpp`` appends one summary row per run to ``results.jsonl`` and also
writes the same object to ``runs/<cfg_id>/<part>/t<tier>/result.json``. The per-run copy
goes through ``atomic_write`` (tmp + flush + checked + rename, ``apps/testlab/main.cpp``
:99-113), so it is strictly more durable than the summary append -- and it is written
*before* the row is appended. Any failure between those two points therefore leaves
completed, durably-recorded work missing from the summary file.

This tool makes that class of loss recoverable instead of merely rare. A field audit of
``advisor-batch-1-s3`` found all 197 rows reconstructing from their artifacts with
identical key sets and equal values, and no field existing only in the summary row.

Rules:

* The per-run ``result.json`` is ground truth for rows that are MISSING from the summary.
  Rows already present in ``results.jsonl`` are re-emitted as their original bytes, never
  re-serialised, so a successful rebuild leaves the existing content untouched.
* Rows are deduplicated on ``(cfg_id, part, tier)`` -- the same key the runner uses to
  decide what to skip on resume (``completed_keys``, ``apps/testlab/main.cpp``:2762).
* Rows are emitted in campaign-plan order: tier, then config, then the part order of
  ``campaign.json``. Config order is taken from the order config ids first appear in the
  existing ``results.jsonl`` (which the runner wrote in plan order), with any config seen
  only under ``runs/`` appended in sorted order. This deliberately avoids reimplementing
  the C++ ``cfg_id_of`` FNV-1a hash in Python, where it could silently drift.
* REFUSAL IS THE DEFAULT. The rebuild is applied only if the existing ``results.jsonl``
  is a strict prefix of the reconstruction. If it is not, some existing row would be
  discarded or reordered, and the tool refuses and reports the divergence; ``--force``
  overrides. A recovery tool that can silently drop rows is worse than the problem.
* Only campaigns that persist artifacts are recoverable: ``apps/testlab/main.cpp``:3047
  writes a run directory only when ``camp.warehouse`` is set or ``adapt_passes > 0``.
  Without artifacts there is nothing to rebuild from, and the tool says so rather than
  producing a short file.
* ``checkpoint.json``'s ``completed_runs`` is recomputed to the rebuilt row count. Every
  other checkpoint field is preserved; resume correctness does not depend on the counter
  (``done`` is rebuilt from ``results.jsonl`` itself), but leaving it stale is misleading.
* The previous ``results.jsonl`` is copied to ``results.pre-rebuild.jsonl`` before any
  write, matching how campaign state is archived elsewhere in this repo.

Run from the repo root::

    python scripts/advisor/rebuild_results.py --campaign advisor-batch-1-s3 --dry-run
    python scripts/advisor/rebuild_results.py --campaign advisor-batch-1-s3
    python scripts/advisor/rebuild_results.py --selftest
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
CAMPAIGNS = ROOT / "bench" / "campaigns"

#: Same separators/ordering as nlohmann::json's default ``dump()`` so a recovered row is
#: byte-comparable with a runner-emitted one (nlohmann stores objects key-sorted).
_DUMP_KWARGS: dict[str, Any] = {
    "sort_keys": True,
    "separators": (",", ":"),
    "ensure_ascii": False,
}


def rel(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def row_key(row: dict[str, Any]) -> tuple[str, str, int] | None:
    """The ``(cfg_id, part, tier)`` identity the runner dedupes on, or None if malformed."""
    cfg_id = row.get("cfg_id")
    part = row.get("part")
    tier = row.get("tier")
    if not isinstance(cfg_id, str) or not isinstance(part, str):
        return None
    if isinstance(tier, bool) or not isinstance(tier, int):
        return None
    return (cfg_id, part, tier)


def part_names(campaign: dict[str, Any]) -> list[str]:
    """Part names in campaign-plan order, derived from ``campaign.json``'s parts list.

    Entries are case-file paths (``.../sphere_box_s0_c0.case.json``); the runner's part
    name is that file's name with the ``.case.json`` suffix removed.
    """
    names: list[str] = []
    for entry in campaign.get("parts") or []:
        if not isinstance(entry, str):
            continue
        name = Path(entry).name
        for suffix in (".case.json", ".json"):
            if name.endswith(suffix):
                name = name[: -len(suffix)]
                break
        if name.endswith(".case"):
            name = name[: -len(".case")]
        if name and name not in names:
            names.append(name)
    return names


def read_existing(path: Path) -> tuple[list[str], list[tuple[str, str, int]], int]:
    """Return (raw line texts, their keys, count of unparseable lines)."""
    if not path.exists():
        return ([], [], 0)
    texts: list[str] = []
    keys: list[tuple[str, str, int]] = []
    bad = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        try:
            row = json.loads(stripped)
        except json.JSONDecodeError:
            bad += 1
            continue
        key = row_key(row) if isinstance(row, dict) else None
        if key is None:
            bad += 1
            continue
        texts.append(stripped)
        keys.append(key)
    return (texts, keys, bad)


def scan_artifacts(runs_dir: Path) -> tuple[dict[tuple[str, str, int], str], int]:
    """Map ``(cfg_id, part, tier)`` -> serialised row, read from every ``result.json``."""
    found: dict[tuple[str, str, int], str] = {}
    bad = 0
    if not runs_dir.is_dir():
        return (found, bad)
    for path in sorted(runs_dir.glob("*/*/t*/result.json")):
        try:
            row = json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            bad += 1
            continue
        if not isinstance(row, dict):
            bad += 1
            continue
        key = row_key(row)
        if key is None:
            bad += 1
            continue
        # Trust the row's own identity, but only when it agrees with its location --
        # a mismatch means the tree was moved or hand-edited and is not ground truth.
        cfg_dir, part_dir, tier_dir = path.parts[-4], path.parts[-3], path.parts[-2]
        if (cfg_dir, part_dir) != (key[0], key[1]) or tier_dir != f"t{key[2]}":
            bad += 1
            continue
        found[key] = json.dumps(row, **_DUMP_KWARGS)
    return (found, bad)


def plan_order(
    campaign: dict[str, Any],
    existing_keys: list[tuple[str, str, int]],
    artifact_keys: list[tuple[str, str, int]],
) -> list[tuple[str, str, int]]:
    """Every known key, ordered tier -> config -> part, as the runner emits them."""
    parts = part_names(campaign)
    part_rank = {name: i for i, name in enumerate(parts)}

    cfg_order: list[str] = []
    for cfg_id, _part, _tier in existing_keys:
        if cfg_id not in cfg_order:
            cfg_order.append(cfg_id)
    for cfg_id in sorted({key[0] for key in artifact_keys}):
        if cfg_id not in cfg_order:
            cfg_order.append(cfg_id)
    cfg_rank = {cfg_id: i for i, cfg_id in enumerate(cfg_order)}

    # Parts absent from campaign.json still have to land somewhere deterministic.
    extra_parts = sorted(
        {key[1] for key in (*existing_keys, *artifact_keys)} - set(part_rank)
    )
    for name in extra_parts:
        part_rank[name] = len(part_rank)

    all_keys = set(existing_keys) | set(artifact_keys)
    return sorted(
        all_keys,
        key=lambda k: (k[2], cfg_rank.get(k[0], len(cfg_rank)), part_rank[k[1]]),
    )


def prefix_divergence(
    existing_keys: list[tuple[str, str, int]],
    rebuilt_keys: list[tuple[str, str, int]],
) -> str | None:
    """None if existing is a prefix of rebuilt, else a human description of the clash."""
    if len(existing_keys) > len(rebuilt_keys):
        return (f"existing file has {len(existing_keys)} rows but the rebuild only "
                f"accounts for {len(rebuilt_keys)}")
    for i, (have, want) in enumerate(zip(existing_keys, rebuilt_keys)):
        if have != want:
            return (f"row {i + 1} is {have} in the existing file but {want} in the "
                    "rebuild (rows would be reordered or discarded)")
    return None


def rebuild(directory: Path, *, dry_run: bool, force: bool) -> int:
    camp_path = directory / "campaign.json"
    if not camp_path.is_file():
        print(f"error: {rel(camp_path)} not found; not a campaign directory")
        return 1
    campaign = json.loads(camp_path.read_text(encoding="utf-8"))

    results_path = directory / "results.jsonl"
    runs_dir = directory / "runs"
    existing_texts, existing_keys, existing_bad = read_existing(results_path)
    artifacts, artifact_bad = scan_artifacts(runs_dir)

    print(f"campaign {rel(directory)}")
    print(f"  results.jsonl      {len(existing_keys)} rows"
          + (f" ({existing_bad} unparseable, ignored)" if existing_bad else ""))
    print(f"  runs/ artifacts    {len(artifacts)} result.json"
          + (f" ({artifact_bad} unusable, ignored)" if artifact_bad else ""))

    if not artifacts:
        warehouse = bool(campaign.get("warehouse"))
        print("  nothing to rebuild from: no usable per-run result.json found.")
        if not warehouse:
            print("  this campaign has warehouse=false, so apps/testlab/main.cpp:3047 "
                  "only writes run directories for configs with adapt_passes > 0.")
        return 1

    rebuilt_keys = plan_order(campaign, existing_keys, sorted(artifacts))
    existing_by_key = dict(zip(existing_keys, existing_texts))
    recovered = [key for key in rebuilt_keys if key not in existing_by_key]
    orphans = [key for key in existing_keys if key not in artifacts]

    print(f"  rebuild            {len(rebuilt_keys)} rows "
          f"({len(recovered)} recovered from artifacts)")
    if orphans:
        print(f"  note: {len(orphans)} existing row(s) have no artifact; kept as-is "
              "(e.g. rows written before warehouse output existed)")
    for key in recovered:
        print(f"    + {key[0]}  {key[1]}  tier={key[2]}")

    # Nothing to recover means nothing to write, whatever the row order is: a tool
    # that rewrites a file it has no rows to add to is pure risk.
    if not recovered:
        print("  already complete; nothing to do.")
        return 0

    divergence = prefix_divergence(existing_keys, rebuilt_keys)
    if divergence is not None:
        print(f"  REFUSING: {divergence}")
        if not force:
            print("  the existing results.jsonl is not a strict prefix of the rebuild, "
                  "so applying it could discard rows. Re-run with --force only after "
                  "checking the divergence above.")
            return 1
        print("  --force given: applying anyway.")

    # Preserve existing rows byte-for-byte; only recovered rows are re-serialised.
    lines = [existing_by_key.get(key) or artifacts[key] for key in rebuilt_keys]

    if dry_run:
        print(f"  --dry-run: would write {len(lines)} rows to {rel(results_path)}")
        print(f"  --dry-run: would set checkpoint completed_runs to {len(lines)}")
        return 0

    if results_path.exists():
        backup = directory / "results.pre-rebuild.jsonl"
        shutil.copy2(results_path, backup)
        print(f"  backed up existing file to {rel(backup)}")
    results_path.write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")
    print(f"  wrote {len(lines)} rows to {rel(results_path)}")

    cp_path = directory / "checkpoint.json"
    if cp_path.is_file():
        checkpoint = json.loads(cp_path.read_text(encoding="utf-8"))
        before = checkpoint.get("completed_runs")
        if before != len(lines):
            checkpoint["completed_runs"] = len(lines)
            cp_path.write_text(json.dumps(checkpoint, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
            print(f"  checkpoint completed_runs {before} -> {len(lines)}")
    return 0


def selftest() -> int:
    """Round-trip a small synthetic campaign directory: truncate, rebuild, verify."""
    parts = ["widget_s0_c0", "widget_s0_c1", "widget_s1_c0"]
    cfgs = ["cfg-aaaa1111", "cfg-bbbb2222"]
    failures: list[str] = []

    def check(label: str, ok: bool) -> None:
        print(f"    {'ok  ' if ok else 'FAIL'} {label}")
        if not ok:
            failures.append(label)

    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp) / "advisor-selftest"
        (directory / "runs").mkdir(parents=True)
        (directory / "campaign.json").write_text(json.dumps({
            "name": "advisor-selftest",
            "warehouse": True,
            "parts": [f"bench/geometries/{p}.case.json" for p in parts],
            "tiers": [{"h_scale": 1.0, "keep_frac": 1.0}],
        }), encoding="utf-8")

        # Emit every row in plan order, writing both artifact and summary row, exactly
        # as the runner does (artifact first, then the append).
        rows: list[str] = []
        for cfg in cfgs:
            for part in parts:
                row = {"cfg_id": cfg, "part": part, "tier": 0,
                       "status": "ok", "wall_time_s": 1.5, "n_dof": 1234,
                       "mesher_note": "curv_turn\u226415\u00b0"}
                run_dir = directory / "runs" / cfg / part / "t0"
                run_dir.mkdir(parents=True)
                (run_dir / "result.json").write_text(
                    json.dumps(row, indent=2) + "\n", encoding="utf-8")
                rows.append(json.dumps(row, **_DUMP_KWARGS))
        results_path = directory / "results.jsonl"
        full = "".join(f"{r}\n" for r in rows)
        results_path.write_text(full, encoding="utf-8")
        (directory / "checkpoint.json").write_text(
            json.dumps({"campaign": "advisor-selftest", "completed_runs": len(rows),
                        "state": "running", "survivors": cfgs, "tier": 0}),
            encoding="utf-8")

        print("  [1] rebuild of a complete campaign is a no-op")
        check("returns 0", rebuild(directory, dry_run=False, force=False) == 0)
        check("file unchanged",
              results_path.read_text(encoding="utf-8") == full)
        check("no backup written", not (directory / "results.pre-rebuild.jsonl").exists())

        print("  [2] truncated tail is recovered exactly (the field failure mode)")
        results_path.write_text("".join(f"{r}\n" for r in rows[:-3]), encoding="utf-8")
        (directory / "checkpoint.json").write_text(
            json.dumps({"campaign": "advisor-selftest",
                        "completed_runs": len(rows) - 3, "state": "running",
                        "survivors": cfgs, "tier": 0}), encoding="utf-8")
        check("dry-run returns 0 and writes nothing",
              rebuild(directory, dry_run=True, force=False) == 0
              and len(results_path.read_text(encoding="utf-8").splitlines())
              == len(rows) - 3)
        check("rebuild returns 0", rebuild(directory, dry_run=False, force=False) == 0)
        check("restored byte-for-byte",
              results_path.read_text(encoding="utf-8") == full)
        check("backup preserved the truncated file",
              len((directory / "results.pre-rebuild.jsonl")
                  .read_text(encoding="utf-8").splitlines()) == len(rows) - 3)
        check("checkpoint completed_runs recomputed",
              json.loads((directory / "checkpoint.json")
                         .read_text(encoding="utf-8"))["completed_runs"] == len(rows))
        check("non-ASCII survives the round-trip",
              "curv_turn\u226415\u00b0" in results_path.read_text(encoding="utf-8"))

        print("  [3] reorder with a missing row is refused, and --force writes")
        # Swap the first two rows AND drop the last, so there is genuinely something to
        # recover: without a recoverable row the tool correctly does nothing at all.
        swapped = [rows[1], rows[0], *rows[2:-1]]
        results_path.write_text("".join(f"{r}\n" for r in swapped), encoding="utf-8")
        check("refuses with exit 1", rebuild(directory, dry_run=False, force=False) == 1)
        check("left the file alone",
              results_path.read_text(encoding="utf-8")
              == "".join(f"{r}\n" for r in swapped))
        check("--force applies it", rebuild(directory, dry_run=False, force=True) == 0)
        check("--force restored plan order and every row",
              results_path.read_text(encoding="utf-8") == full)

        print("  [3b] a pure reorder with nothing to recover is left untouched")
        reordered = [rows[1], rows[0], *rows[2:]]
        results_path.write_text("".join(f"{r}\n" for r in reordered), encoding="utf-8")
        check("returns 0 without rewriting",
              rebuild(directory, dry_run=False, force=False) == 0
              and results_path.read_text(encoding="utf-8")
              == "".join(f"{r}\n" for r in reordered))

        print("  [4] a campaign with no artifacts reports instead of truncating")
        bare = Path(tmp) / "advisor-bare"
        bare.mkdir()
        (bare / "campaign.json").write_text(json.dumps({
            "name": "advisor-bare", "warehouse": False,
            "parts": [f"bench/geometries/{p}.case.json" for p in parts],
            "tiers": [{"h_scale": 1.0, "keep_frac": 1.0}]}), encoding="utf-8")
        (bare / "results.jsonl").write_text(f"{rows[0]}\n", encoding="utf-8")
        check("returns 1", rebuild(bare, dry_run=False, force=False) == 1)
        check("did not touch results.jsonl",
              (bare / "results.jsonl").read_text(encoding="utf-8") == f"{rows[0]}\n")

    print()
    if failures:
        print(f"selftest FAILED ({len(failures)} check(s)): " + "; ".join(failures))
        return 1
    print("selftest passed")
    return 0


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--campaign", default=None, metavar="NAME_OR_DIR",
                        help="campaign name under bench/campaigns, or a directory path")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would change without writing anything")
    parser.add_argument("--force", action="store_true",
                        help="apply even if the existing results.jsonl is not a strict "
                             "prefix of the rebuild (may discard or reorder rows)")
    parser.add_argument("--selftest", action="store_true",
                        help="round-trip a synthetic campaign directory and exit")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.selftest:
        print("rebuild_results selftest")
        return selftest()
    if not args.campaign:
        print("error: --campaign is required (or pass --selftest)")
        return 2
    candidate = Path(args.campaign)
    directory = candidate if candidate.is_dir() else CAMPAIGNS / args.campaign
    if not directory.is_dir():
        print(f"error: {rel(directory)} is not a directory")
        return 2
    return rebuild(directory, dry_run=args.dry_run, force=args.force)


if __name__ == "__main__":
    sys.exit(main())
