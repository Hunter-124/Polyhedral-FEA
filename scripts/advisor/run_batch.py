#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Dedup + sharded execution of advisor training campaigns.

The expensive resource is the FEA solve. Everything here exists to (a) never
re-solve a ``(part, cfg_id)`` pair that already produced a row anywhere under
``bench/campaigns/advisor-*``, and (b) spread the remaining pairs over several
concurrent ``polymesh_testlab`` processes.

Run from the repo root::

    python scripts/advisor/run_batch.py --batch 1 --dry-run
    python scripts/advisor/run_batch.py --batch 1

Testlab invocation encoded here (``apps/testlab/main.cpp``)::

    usage: polymesh_testlab run|resume|validate|pause-status <campaign_dir>   # main.cpp:2237
    if (cmd == "resume") return run_campaign(camp_dir, /*resume=*/true);      # main.cpp:2498

``resume`` is used for every launch, never ``run``: the fresh path truncates
``results.jsonl`` (``std::ofstream trunc(results_path, std::ios::trunc)``,
main.cpp:2327) and seeds ``survivors`` with the *whole* grid, while the resume
path keeps existing rows and honours the hand-written ``checkpoint.json``
(main.cpp:2308-2323).  Both paths then skip any ``cfg_id|part|tier`` already in
``results.jsonl`` (main.cpp:2343 + 2397-2400), which gives pair-level restart
safety inside a campaign directory for free.

Config-subset control therefore comes from ``checkpoint.json.survivors``
(main.cpp:2378-2382), and part-subset control from ``campaign.json.parts``.  A
campaign is always a full ``configs x parts`` rectangle, so the missing-pair set
is decomposed into rectangles by grouping configs that miss exactly the same
parts; in the common case (nothing done yet, or whole configs done) that is a
single group and each shard gets exactly one campaign directory.

``cfg_id`` strings are mirrored from ``cfg_id_of`` (main.cpp:117-134) rather than
invented: testlab derives them itself from the grid, and a mismatch would make
``survivors`` empty, which testlab silently expands back to the full grid
(main.cpp:2383-2387).  :func:`verify_cfg_id_mirror` re-proves the mirror against
every recorded row on every invocation and aborts on the first disagreement.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[2]
CAMPAIGNS = ROOT / "bench" / "campaigns"
ADVISOR_DIR = ROOT / "bench" / "advisor"
THROUGHPUT_PATH = ADVISOR_DIR / "throughput.json"
TESTLAB = ROOT / "build" / "apps" / "testlab" / "polymesh_testlab.exe"
DATASET_BUILDER = ROOT / "scripts" / "build_advisor_dataset.py"
PROMOTE_TRUTH = ROOT / "scripts" / "advisor" / "promote_truth.py"

TRUTH_CAMPAIGN = "advisor-truth-0"
DEFAULT_TEMPLATE = "bench/campaigns/advisor-pilot-1/campaign.json"
DEFAULT_PARTS_GLOB = "bench/geometries/corpus/primitives/*.case.json"

# Fixed for this 6-core / 12-thread machine (plan step 3): 4 workers x 2 OpenMP
# threads = 8 busy threads, leaving OS headroom. Not discovered dynamically.
SHARDS = 4
OMP_THREADS_PER_SHARD = 2

# Step-3 contingency trigger: mesh time above this fraction of mesh+solve time
# means p-order-only variants are paying for remeshing, so a mesh cache would
# pay off. This script only reports it; it never builds a cache.
MESH_CACHE_FRAC_THRESHOLD = 0.30


# ── testlab-compatible primitives ───────────────────────────────────────────


def cfg_id_of(config: dict[str, Any]) -> str:
    """FNV-1a 64 over the canonical JSON dump of a config object.

    Mirror of ``cfg_id_of`` in apps/testlab/main.cpp:117-134::

        std::map<std::string, json> ordered;   // key-sorted
        const std::string s = canon.dump();    // compact, no spaces
        std::uint64_t h = 14695981039346656037ull;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        std::snprintf(buf, sizeof(buf), "cfg-%08x", (unsigned)(h & 0xffffffffu));
    """
    ordered = {key: config[key] for key in sorted(config)}
    text = json.dumps(ordered, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    digest = 14695981039346656037
    for byte in text.encode("utf-8"):
        digest = ((digest ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "cfg-%08x" % (digest & 0xFFFFFFFF)


def expand_grid(grid: dict[str, list[Any]]) -> list[tuple[str, dict[str, Any]]]:
    """Full-factorial expansion, mirroring expand_grid (main.cpp:480-504).

    nlohmann's default object is a ``std::map``, so testlab walks the grid keys
    in sorted order; we do the same to keep the emitted run order identical.
    """
    if not isinstance(grid, dict) or not grid:
        raise SystemExit("campaign grid is empty")
    keys = sorted(grid)
    for key in keys:
        if not isinstance(grid[key], list) or not grid[key]:
            raise SystemExit(f"grid.{key} must be a non-empty array")
    configs: list[tuple[str, dict[str, Any]]] = []
    index = [0] * len(keys)
    while True:
        values = {key: grid[key][index[i]] for i, key in enumerate(keys)}
        configs.append((cfg_id_of(values), values))
        pos = len(keys) - 1
        while pos >= 0:
            index[pos] += 1
            if index[pos] < len(grid[keys[pos]]):
                break
            index[pos] = 0
            pos -= 1
        if pos < 0:
            break
    seen: dict[str, dict[str, Any]] = {}
    for cfg_id, values in configs:
        if cfg_id in seen and seen[cfg_id] != values:
            raise SystemExit(f"cfg_id collision between {seen[cfg_id]} and {values}")
        seen[cfg_id] = values
    return configs


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise SystemExit(f"{path}: expected a JSON object")
    return value


def iter_rows(path: Path, start_line: int = 0) -> Iterable[dict[str, Any]]:
    """Yield JSON objects from a results.jsonl, skipping the first N lines."""
    if not path.exists():
        return
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for number, text in enumerate(stream):
            if number < start_line or not text.strip():
                continue
            try:
                row = json.loads(text)
            except json.JSONDecodeError:
                continue  # partially flushed tail of a killed process
            if isinstance(row, dict):
                yield row


def count_lines(path: Path) -> int:
    if not path.exists():
        return 0
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        return sum(1 for _ in stream)


def rel(path: Path) -> str:
    """Repo-relative POSIX path for logs, falling back to the absolute path."""
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


# ── dedup ───────────────────────────────────────────────────────────────────


def verify_cfg_id_mirror() -> int:
    """Re-derive cfg_id for every recorded row; abort on any disagreement."""
    checked = 0
    if not CAMPAIGNS.is_dir():
        return 0
    for results in sorted(CAMPAIGNS.glob("*/results.jsonl")):
        for row in iter_rows(results):
            config = row.get("config")
            recorded = row.get("cfg_id")
            if not isinstance(config, dict) or not isinstance(recorded, str):
                continue
            checked += 1
            derived = cfg_id_of(config)
            if derived != recorded:
                raise SystemExit(
                    f"cfg_id mirror broken: {results} recorded {recorded}, "
                    f"this script derives {derived} for {config}. "
                    "Re-check cfg_id_of() against apps/testlab/main.cpp before running."
                )
    return checked


def completed_pairs() -> tuple[set[tuple[str, str]], list[Path]]:
    """(part, cfg_id) pairs already recorded under bench/campaigns/advisor-*.

    The suppression this drives is GENERATION-BLIND and PERMANENT. The key holds
    no tier, no campaign and no engine identity, so a pair recorded by ANY binary
    -- including one whose loads were later proven wrong -- counts as done for
    ever. That is deliberate for the current use (spreading unfinished work over
    shards), but it means a fix requiring the same pairs to be re-solved cannot be
    scheduled through this function at all: it reports those pairs complete and
    plans nothing.

    The two escape routes, for whoever needs them next:

    * write the re-run into a campaign directory OUTSIDE the ``advisor-*`` glob, or
    * delete the superseded rows from the recorded ``results.jsonl``.

    ``advisor-batch-1-affected*`` took neither: those names DO match ``advisor-*``,
    so their pairs are now permanently suppressed here. It did not bite because
    testlab was driven directly for those re-runs. The duplicate rows that produced
    are resolved explicitly by ``CAMPAIGN_PRIORITY`` in
    scripts/build_advisor_dataset.py; this function does not rank generations, and
    does not need to, because it only ever asks "has this been solved at all".
    """
    done: set[tuple[str, str]] = set()
    scanned: list[Path] = []
    if not CAMPAIGNS.is_dir():
        return done, scanned
    for campaign_dir in sorted(CAMPAIGNS.glob("advisor-*")):
        results = campaign_dir / "results.jsonl"
        if not results.is_file():
            continue
        scanned.append(results)
        for row in iter_rows(results):
            part = row.get("part")
            cfg_id = row.get("cfg_id")
            if isinstance(part, str) and isinstance(cfg_id, str):
                done.add((part, cfg_id))
    return done, scanned


def truth_results_paths(campaign: str = TRUTH_CAMPAIGN) -> list[Path]:
    """Every ``results.jsonl`` that belongs to the truth run, sharded or not.

    The truth run is sharded into ``<campaign>-s0..-sN`` -- separate campaign
    directories over the same grid with disjoint part lists, each keeping its own
    ``results.jsonl``, and nothing merges them back into the parent.  ``(part,
    cfg_id)`` keys are directly comparable across all of them, so the gate and
    :mod:`promote_truth` must read the union, exactly as :func:`completed_pairs`
    already does for the batch campaigns.
    """
    paths: list[Path] = []
    if not CAMPAIGNS.is_dir():
        return paths
    for directory in [CAMPAIGNS / campaign, *sorted(CAMPAIGNS.glob(f"{campaign}-s*"))]:
        results = directory / "results.jsonl"
        if results.is_file():
            paths.append(results)
    return paths


# ── plan ────────────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class Rect:
    """One campaign directory: every config x every part, all of them missing."""

    shard: int
    name: str
    cfg_ids: tuple[str, ...]
    part_ids: tuple[str, ...]
    case_paths: tuple[str, ...]

    @property
    def pairs(self) -> int:
        return len(self.cfg_ids) * len(self.part_ids)

    @property
    def directory(self) -> Path:
        return CAMPAIGNS / self.name


@dataclass
class Plan:
    batch: int
    template_path: Path
    template: dict[str, Any]
    configs: list[tuple[str, dict[str, Any]]]
    parts: list[tuple[str, str]]
    completed: set[tuple[str, str]]
    scanned: list[Path]
    rects: list[Rect] = field(default_factory=list)
    parts_source: str = ""

    @property
    def pairs_total(self) -> int:
        return len(self.configs) * len(self.parts)

    @property
    def missing(self) -> int:
        return sum(rect.pairs for rect in self.rects)

    @property
    def dedup_hits(self) -> int:
        return self.pairs_total - self.missing


def load_parts(parts_glob: str | None, template: dict[str, Any]) -> tuple[list[tuple[str, str]], str]:
    """Resolve (part_id, case-json path) pairs.

    Precedence, most specific first: an explicit ``--parts-glob`` beats the
    template, a template that lists its own ``parts`` beats the built-in
    default glob, and the default glob is the last resort. ``parts_glob`` is
    ``None`` when the flag was not passed.
    """
    template_parts = [entry for entry in template.get("parts", []) if isinstance(entry, str)]
    if parts_glob is not None:
        matches = sorted(ROOT.glob(parts_glob))
        source = f"glob {parts_glob}"
        if not matches:
            raise SystemExit(f"--parts-glob {parts_glob} matched no case json under {ROOT}")
    elif template_parts:
        matches = [ROOT / entry for entry in template_parts]
        source = "template parts"
    else:
        matches = sorted(ROOT.glob(DEFAULT_PARTS_GLOB))
        source = f"default glob {DEFAULT_PARTS_GLOB}"
        if not matches:
            raise SystemExit(
                f"template lists no parts and {DEFAULT_PARTS_GLOB} matched nothing; "
                "pass --parts-glob"
            )
    parts: list[tuple[str, str]] = []
    seen: dict[str, str] = {}
    for path in matches:
        if not path.is_file():
            raise SystemExit(f"case json not found: {path}")
        case = read_json(path)
        part_id = case.get("part")
        if not isinstance(part_id, str) or not part_id:
            raise SystemExit(f"{path}: case json has no string 'part' field")
        case_rel = path.relative_to(ROOT).as_posix()
        if part_id in seen:
            raise SystemExit(f"duplicate part id {part_id!r} in {seen[part_id]} and {case_rel}")
        seen[part_id] = case_rel
        parts.append((part_id, case_rel))
    if not parts:
        raise SystemExit("no parts resolved; pass --parts-glob")
    return parts, source


def build_rects(batch: int, plan: Plan, shards: int) -> list[Rect]:
    """Decompose the missing pairs into per-shard campaign rectangles.

    Missing pairs are grouped by the exact set of parts a config still needs;
    each group is a true ``configs x parts`` rectangle, which is the only thing
    a campaign.json can express. Configs inside a group are dealt out
    round-robin so the shards get near-equal pair counts.
    """
    part_order = {part_id: i for i, (part_id, _) in enumerate(plan.parts)}
    case_of = dict(plan.parts)

    missing_by_cfg: dict[str, list[str]] = {}
    for cfg_id, _ in plan.configs:
        wanted = [p for p, _ in plan.parts if (p, cfg_id) not in plan.completed]
        if wanted:
            missing_by_cfg[cfg_id] = wanted

    groups: dict[tuple[str, ...], list[str]] = {}
    for cfg_id, wanted in missing_by_cfg.items():
        groups.setdefault(tuple(wanted), []).append(cfg_id)

    ordered_groups = sorted(
        groups.items(), key=lambda item: (-len(item[1]) * len(item[0]), item[0])
    )
    per_shard: dict[int, list[tuple[tuple[str, ...], tuple[str, ...]]]] = {
        k: [] for k in range(shards)
    }
    # missing_by_cfg was filled in plan.configs order, so each group's list is
    # already in grid order; dealing it out round-robin balances the shards.
    for signature, cfg_ids in ordered_groups:
        for shard in range(shards):
            slice_ = tuple(cfg_ids[shard::shards])
            if slice_:
                per_shard[shard].append((signature, slice_))

    rects: list[Rect] = []
    for shard in range(shards):
        assigned = per_shard[shard]
        for index, (signature, cfg_ids) in enumerate(assigned):
            name = f"advisor-batch-{batch}-s{shard}"
            if len(assigned) > 1:
                name = f"{name}-g{index}"
            part_ids = tuple(sorted(signature, key=lambda p: part_order[p]))
            rects.append(
                Rect(
                    shard=shard,
                    name=name,
                    cfg_ids=cfg_ids,
                    part_ids=part_ids,
                    case_paths=tuple(case_of[p] for p in part_ids),
                )
            )
    return rects


def make_plan(args: argparse.Namespace) -> Plan:
    template_path = (ROOT / args.campaign_template).resolve()
    if not template_path.is_file():
        raise SystemExit(f"campaign template not found: {template_path}")
    template = read_json(template_path)
    tiers = template.get("tiers", [])
    if len(tiers) != 1:
        raise SystemExit(
            f"{template_path}: batch templates must declare exactly one tier "
            f"(found {len(tiers)}); dedup keys are (part, cfg_id) at tier 0"
        )
    configs = expand_grid(template.get("grid", {}))
    parts, parts_source = load_parts(args.parts_glob, template)
    completed, scanned = completed_pairs()
    plan = Plan(
        batch=args.batch,
        template_path=template_path,
        template=template,
        configs=configs,
        parts=parts,
        completed=completed,
        scanned=scanned,
        parts_source=parts_source,
    )
    plan.rects = build_rects(args.batch, plan, args.shards)
    return plan


# ── campaign directory emission ─────────────────────────────────────────────


def campaign_json(name: str, plan: Plan, case_paths: Iterable[str], comment: str) -> dict[str, Any]:
    template = plan.template
    body: dict[str, Any] = {
        "name": name,
        "comment": comment,
        "warehouse": bool(template.get("warehouse", True)),
        "parts": list(case_paths),
        "tiers": template["tiers"],
        "grid": template["grid"],
    }
    if "score" in template:
        body["score"] = template["score"]
    if "resources" in template:
        body["resources"] = template["resources"]
    return body


def write_checkpoint(path: Path, name: str, survivors: Iterable[str]) -> None:
    """checkpoint.json in the exact shape load_checkpoint reads (main.cpp:598-612)."""
    now = utc_now()
    payload = {
        "campaign": name,
        "state": "running",
        "tier": 0,
        "completed_runs": 0,
        "survivors": list(survivors),
        "started_utc": now,
        "updated_utc": now,
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def materialize(rect: Rect, plan: Plan) -> None:
    directory = rect.directory
    directory.mkdir(parents=True, exist_ok=True)
    comment = (
        f"advisor batch {plan.batch} shard {rect.shard}: {len(rect.cfg_ids)} configs x "
        f"{len(rect.part_ids)} parts = {rect.pairs} pairs still missing "
        f"(generated by scripts/advisor/run_batch.py from {plan.template_path.name})"
    )
    body = campaign_json(rect.name, plan, rect.case_paths, comment)
    (directory / "campaign.json").write_text(json.dumps(body, indent=2) + "\n", encoding="utf-8")
    write_checkpoint(directory / "checkpoint.json", rect.name, rect.cfg_ids)


# ── execution ───────────────────────────────────────────────────────────────


def launch(directory: Path, omp_threads: int) -> tuple[int, float]:
    """Run `polymesh_testlab resume <dir>`, streaming output to run.log."""
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = str(omp_threads)
    # testlab's on_finish hooks are Python. Hand them THIS interpreter rather than
    # letting them resolve `python3` from PATH: on the workstation that resolves to
    # a bare install with no numpy, which made warehouse_shots.py render nothing
    # for a whole regeneration while reporting only an exit code.
    env["POLYMESH_PYTHON"] = sys.executable
    command = [str(TESTLAB), "resume", str(directory)]
    started = time.monotonic()
    log_path = directory / "run.log"
    with log_path.open("a", encoding="utf-8", errors="replace") as log:
        log.write(f"\n=== {utc_now()} OMP_NUM_THREADS={omp_threads} :: {' '.join(command)}\n")
        log.flush()
        try:
            completed = subprocess.run(
                command, cwd=str(ROOT), env=env, stdout=log, stderr=subprocess.STDOUT, check=False
            )
            code = completed.returncode
        except OSError as exc:  # missing binary, permission, ...
            log.write(f"launch failed: {exc}\n")
            code = -1
        wall = time.monotonic() - started
        log.write(f"=== exit {code} after {wall:.1f}s\n")
    return code, wall


def run_shard(shard: int, rects: list[Rect], omp_threads: int) -> list[dict[str, Any]]:
    """Run one shard's campaign directories sequentially. Crashes do not abort."""
    report: list[dict[str, Any]] = []
    for rect in rects:
        before = count_lines(rect.directory / "results.jsonl")
        code, wall = launch(rect.directory, omp_threads)
        after = count_lines(rect.directory / "results.jsonl")
        if code != 0:
            print(
                f"  shard {shard}: {rect.name} exited {code} "
                f"(see {rel(rect.directory / 'run.log')})",
                flush=True,
            )
        report.append(
            {
                "shard": shard,
                "campaign": rect.name,
                "wall_s": round(wall, 1),
                "rows": after - before,
                "returncode": code,
            }
        )
    return report


def run_truth_gate(dry_run: bool) -> bool:
    """Run advisor-truth-0 to completion first. Returns True if it executed."""
    directory = CAMPAIGNS / TRUTH_CAMPAIGN
    campaign_path = directory / "campaign.json"
    if not campaign_path.is_file():
        print(f"truth gate: {rel(campaign_path)} absent — nothing to run yet")
        return False
    truth = read_json(campaign_path)
    configs = expand_grid(truth.get("grid", {}))
    truth_parts: list[str] = []
    for entry in truth.get("parts", []):
        case = read_json(ROOT / entry)
        part_id = case.get("part")
        if not isinstance(part_id, str):
            raise SystemExit(f"{entry}: case json has no string 'part' field")
        truth_parts.append(part_id)
    tiers = truth.get("tiers", [])
    if len(tiers) != 1:
        raise SystemExit(f"{campaign_path}: truth campaign must declare exactly one tier")

    scanned = truth_results_paths()
    recorded = {
        (row.get("part"), row.get("cfg_id"))
        for results in scanned
        for row in iter_rows(results)
        if isinstance(row.get("part"), str) and isinstance(row.get("cfg_id"), str)
    }
    wanted = {(part, cfg_id) for cfg_id, _ in configs for part in truth_parts}
    have = wanted & recorded
    # checkpoint.json only selects configs, so a config with any part still missing
    # is relaunched whole; pairs already recorded in a *shard* directory are then
    # re-solved, because testlab's own skip list is per campaign directory.
    missing_cfgs = [
        cfg_id
        for cfg_id, _ in configs
        if any((part, cfg_id) not in have for part in truth_parts)
    ]
    print(
        f"truth gate: {TRUTH_CAMPAIGN} has {len(have)}/{len(wanted)} pairs over "
        f"{len(scanned)} results.jsonl ({', '.join(p.parent.name for p in scanned)}), "
        f"{len(missing_cfgs)} config(s) incomplete"
    )
    if not missing_cfgs:
        return False
    if dry_run:
        print(f"  would run: {TESTLAB.name} resume {rel(directory)} (single process)")
        return False

    require_testlab()
    write_checkpoint(directory / "checkpoint.json", truth.get("name", TRUTH_CAMPAIGN), missing_cfgs)
    threads = max(1, os.cpu_count() or 1)
    print(f"  running truth campaign single-process with OMP_NUM_THREADS={threads} ...")
    code, wall = launch(directory, threads)
    print(f"  truth campaign exited {code} after {wall / 60.0:.1f} min")
    if code != 0:
        raise SystemExit("truth campaign failed; refusing to launch batch shards on partial truth")
    if not PROMOTE_TRUTH.is_file():
        raise SystemExit(
            f"missing {rel(PROMOTE_TRUTH)}: overkill truths are provisional until "
            "promoted, refusing to launch batch shards"
        )
    # --require-all: non-zero if any non-analytic corpus reference is still
    # provisional after promotion, i.e. the batch would train on fake truth.
    promote = subprocess.run(
        [sys.executable, rel(PROMOTE_TRUTH), "--require-all"],
        cwd=str(ROOT),
        check=False,
    )
    if promote.returncode != 0:
        raise SystemExit(f"promote_truth.py exited {promote.returncode}")
    return True


def require_testlab() -> None:
    if not TESTLAB.is_file():
        raise SystemExit(f"testlab binary not found: {TESTLAB} (the lead owns builds)")


# ── reporting ───────────────────────────────────────────────────────────────


def mesh_fraction(rects: list[Rect], offsets: dict[str, int]) -> tuple[float, int]:
    """mean(mesh_ms) / mean(mesh_ms + solve_ms) over rows this batch produced."""
    mesh_total = 0.0
    both_total = 0.0
    rows = 0
    for rect in rects:
        results = rect.directory / "results.jsonl"
        for row in iter_rows(results, offsets.get(rect.name, 0)):
            mesh_ms = row.get("mesh_ms")
            solve_ms = row.get("solve_ms")
            if not isinstance(mesh_ms, (int, float)) or not isinstance(solve_ms, (int, float)):
                continue
            mesh_total += float(mesh_ms)
            both_total += float(mesh_ms) + float(solve_ms)
            rows += 1
    if rows == 0 or both_total <= 0.0:
        return 0.0, rows
    return mesh_total / both_total, rows


def unique_path(path: Path) -> Path:
    """``path``, or the first free ``path.2`` / ``path.3`` / ... variant."""
    candidate = path
    suffix = 2
    while candidate.exists():
        candidate = path.with_name(f"{path.name}.{suffix}")
        suffix += 1
    return candidate


def append_throughput(entry: dict[str, Any]) -> None:
    ADVISOR_DIR.mkdir(parents=True, exist_ok=True)
    document: dict[str, Any] = {"batches": []}
    if THROUGHPUT_PATH.is_file():
        quarantine: str | None = None
        try:
            loaded: Any = json.loads(THROUGHPUT_PATH.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            quarantine = f"unparseable ({exc})"
        else:
            if isinstance(loaded, dict) and isinstance(loaded.get("batches"), list):
                document = loaded
            else:
                quarantine = "no 'batches' array"
        if quarantine is not None:
            # This file is the only record of past dedup hits and shard wall-times
            # (the dashboard's throughput panel reads nothing else), so a history we
            # could not parse is moved aside instead of being overwritten.
            aside = unique_path(THROUGHPUT_PATH.with_name(THROUGHPUT_PATH.name + ".corrupt"))
            THROUGHPUT_PATH.replace(aside)
            print(
                f"warning: {rel(THROUGHPUT_PATH)} {quarantine}; moved to {rel(aside)}, "
                "starting a fresh throughput document",
                file=sys.stderr,
            )
    document["batches"].append(entry)
    THROUGHPUT_PATH.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def print_plan(plan: Plan, args: argparse.Namespace) -> None:
    print(f"template:   {rel(plan.template_path)}")
    print(f"parts:      {len(plan.parts)} from {plan.parts_source}")
    print(f"configs:    {len(plan.configs)} (full-factorial grid expansion)")
    print(f"pairs:      {plan.pairs_total} total (part x cfg_id)")
    print(f"dedup scan: {len(plan.scanned)} results.jsonl under bench/campaigns/advisor-*, "
          f"{len(plan.completed)} completed pairs on record")
    print(f"dedup hits: {plan.dedup_hits}")
    print(f"to run:     {plan.missing} pairs across {len(plan.rects)} campaign dir(s), "
          f"{args.shards} shard(s) x OMP_NUM_THREADS={args.omp_threads}")
    for rect in plan.rects:
        print(
            f"  shard {rect.shard}: {rect.name}/campaign.json "
            f"({len(rect.cfg_ids)} configs x {len(rect.part_ids)} parts = {rect.pairs} pairs)"
        )


# ── main ────────────────────────────────────────────────────────────────────


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--batch", type=int, required=True, help="batch number; names advisor-batch-<N>-s<K>")
    parser.add_argument("--campaign-template", default=DEFAULT_TEMPLATE,
                        help=f"campaign json supplying grid/tiers/score/resources (default {DEFAULT_TEMPLATE})")
    parser.add_argument("--parts-glob", default=None,
                        help="repo-relative glob of case jsons; overrides the template's own "
                             f"parts list. Unset: the template's parts if it declares any, "
                             f"else {DEFAULT_PARTS_GLOB}")
    parser.add_argument("--shards", type=int, default=SHARDS, help=f"concurrent testlab processes (default {SHARDS})")
    parser.add_argument("--omp-threads", type=int, default=OMP_THREADS_PER_SHARD,
                        help=f"OMP_NUM_THREADS per shard (default {OMP_THREADS_PER_SHARD})")
    parser.add_argument("--dry-run", action="store_true", help="plan and print; write and launch nothing")
    args = parser.parse_args(argv)
    if args.shards < 1:
        parser.error("--shards must be >= 1")
    if args.omp_threads < 1:
        parser.error("--omp-threads must be >= 1")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    checked = verify_cfg_id_mirror()
    print(f"cfg_id mirror verified against {checked} recorded row(s)")

    # Truth first: the gate may add rows, so the batch plan is built afterwards.
    run_truth_gate(args.dry_run)
    plan = make_plan(args)
    print_plan(plan, args)

    if args.dry_run:
        print("Dry run: no campaign dirs written, no processes launched")
        return 0

    started_wall = time.monotonic()
    started = utc_now()
    expected_ids = {cfg_id for cfg_id, _ in plan.configs}
    offsets: dict[str, int] = {}
    shard_reports: list[dict[str, Any]] = []

    if plan.rects:
        require_testlab()
        for rect in plan.rects:
            materialize(rect, plan)
            offsets[rect.name] = count_lines(rect.directory / "results.jsonl")
        by_shard: dict[int, list[Rect]] = {}
        for rect in plan.rects:
            by_shard.setdefault(rect.shard, []).append(rect)
        with ThreadPoolExecutor(max_workers=args.shards) as pool:
            futures = [
                pool.submit(run_shard, shard, rects, args.omp_threads)
                for shard, rects in sorted(by_shard.items())
            ]
            for future in futures:
                shard_reports.extend(future.result())
    else:
        print("nothing to run: every requested pair is already on record")

    rows_written = sum(report["rows"] for report in shard_reports)
    stray = sorted(
        {
            row.get("cfg_id")
            for rect in plan.rects
            for row in iter_rows(rect.directory / "results.jsonl", offsets.get(rect.name, 0))
            if row.get("cfg_id") not in expected_ids
        }
    )
    if stray:
        raise SystemExit(
            f"shards produced cfg_ids outside the requested grid: {stray[:5]} — "
            "the cfg_id mirror or the survivors checkpoint is wrong; inspect before continuing"
        )

    print("rebuilding bench/advisor/dataset.csv ...")
    build = subprocess.run(
        [sys.executable, rel(DATASET_BUILDER)], cwd=str(ROOT), check=False
    )
    if build.returncode != 0:
        print(f"error: build_advisor_dataset.py exited {build.returncode}", file=sys.stderr)

    wall_s = time.monotonic() - started_wall
    frac, measured = mesh_fraction(plan.rects, offsets)
    entry = {
        "batch": plan.batch,
        "started": started,
        "wall_s": round(wall_s, 1),
        "pairs_total": plan.pairs_total,
        "dedup_hits": plan.dedup_hits,
        "launched": plan.missing,
        "shards": shard_reports,
        "rows_written": rows_written,
        "rows_per_s": round(rows_written / wall_s, 4) if wall_s > 0 else 0.0,
        "mesh_ms_frac": round(frac, 4),
        "mesh_cache_recommended": frac > MESH_CACHE_FRAC_THRESHOLD,
    }
    append_throughput(entry)
    print(
        f"batch {plan.batch}: {rows_written} new rows in {wall_s / 60.0:.1f} min "
        f"({entry['rows_per_s']} rows/s), dedup_hits={plan.dedup_hits}, "
        f"mesh_ms_frac={entry['mesh_ms_frac']} over {measured} row(s)"
        + (" -> mesh cache recommended" if entry["mesh_cache_recommended"] else "")
    )
    failures = [report for report in shard_reports if report["returncode"] != 0]
    if failures:
        print(f"{len(failures)} shard campaign(s) exited non-zero; re-run --batch "
              f"{plan.batch} to pick up where they stopped", file=sys.stderr)
    # A batch whose shards died, or whose dataset was never rebuilt, is a failed
    # batch: CI and any caller chaining train.py must see it in the exit status.
    if failures or build.returncode != 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
