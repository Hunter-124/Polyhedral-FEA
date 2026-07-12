#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Write Grok HANDOFF pack for a finished (or partial) campaign (ADR-0022).

Usage (from repo root):
  python3 scripts/write_grok_handoff.py varyhedron-short-1
  python3 scripts/write_grok_handoff.py bench/campaigns/varyhedron-short-1
"""
from __future__ import annotations

import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def git_head() -> str:
    try:
        return (
            subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True)
            .strip()
        )
    except Exception:
        return "unknown"


def open_program_nodes() -> list[str]:
    path = ROOT / "docs" / "dag" / "PROGRAM.yaml"
    if not path.exists():
        return []
    open_ids: list[str] = []
    cur_id = None
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith("- id:"):
            cur_id = s.split(":", 1)[1].strip()
        elif s.startswith("status:") and cur_id:
            st = s.split(":", 1)[1].strip()
            if st in ("todo", "in_progress"):
                open_ids.append(cur_id)
            cur_id = None
    return open_ids


def resolve_campaign(arg: str) -> Path:
    p = Path(arg)
    if p.is_dir() and (p / "campaign.json").exists():
        return p.resolve()
    cand = ROOT / "bench" / "campaigns" / arg
    if (cand / "campaign.json").exists():
        return cand.resolve()
    raise SystemExit(f"campaign not found: {arg}")


def load_results(camp: Path) -> list[dict]:
    path = camp / "results.jsonl"
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows


def trend_table(rows: list[dict]) -> str:
    # Group by part + mesher + tier
    lines = [
        "| part | mesher | tier | status | mesh_ms | solve_ms | quality | rel_err |",
        "|------|--------|-----:|--------|--------:|---------:|--------:|--------:|",
    ]
    def key(r):
        cfg = r.get("config") or {}
        return (r.get("part", ""), str(cfg.get("mesher", "")), int(r.get("tier", 0)))

    for r in sorted(rows, key=key):
        cfg = r.get("config") or {}
        q = (r.get("quality") or {}).get("score", "")
        acc = r.get("accuracy") or {}
        rel = acc.get("rel_err", "")
        lines.append(
            f"| {r.get('part','')} | {cfg.get('mesher','')} | {r.get('tier','')} | "
            f"{r.get('status','')} | {r.get('mesh_ms','')} | {r.get('solve_ms','')} | "
            f"{q} | {rel} |"
        )
    if len(lines) == 2:
        lines.append("| (no results yet) | | | | | | | |")
    return "\n".join(lines)


def collect_shots(camp: Path) -> list[str]:
    shots = []
    runs = camp / "runs"
    if not runs.is_dir():
        return shots
    for p in sorted(runs.rglob("wire.png")):
        shots.append(str(p.relative_to(ROOT)))
    for p in sorted(runs.rglob("mesh.vtu")):
        # Prefer listing a few VTUs as visual inputs too
        if len(shots) < 40:
            shots.append(str(p.relative_to(ROOT)))
    return shots


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    camp = resolve_campaign(sys.argv[1])
    name = camp.name
    rows = load_results(camp)
    head = git_head()
    open_nodes = open_program_nodes()
    shots = collect_shots(camp)
    finished = ""
    cp = camp / "checkpoint.json"
    if cp.exists():
        try:
            finished = json.loads(cp.read_text(encoding="utf-8")).get("state", "")
        except Exception:
            pass

    handoff = {
        "campaign": name,
        "git_head": head,
        "finished_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "checkpoint_state": finished,
        "pareto": str((camp / "PARETO.md").relative_to(ROOT))
        if (camp / "PARETO.md").exists()
        else None,
        "results": str((camp / "results.jsonl").relative_to(ROOT)),
        "shots": shots,
        "open_program_nodes": open_nodes,
        "mode": "autonomous",
        "max_turns": 80,
    }
    (camp / "handoff.json").write_text(json.dumps(handoff, indent=2) + "\n", encoding="utf-8")

    md = f"""# Grok improvement handoff — `{name}`

You are working autonomously on **Polyhedral-FEA** (PolyMesh). Continue packing /
meshing improvements from campaign results. Read `docs/dag/AGENT_BOOTSTRAP.md`
and obey anti-cheat + pull-rebase + Hunter-124 identity (no AI trailers).

## Sync first
```bash
git fetch origin && git status && git pull --rebase origin master
```
Never force-push. Cap scope to open PROGRAM nodes related to varyhedron packing.

## Campaign snapshot
- **Campaign:** `{name}`
- **git HEAD:** `{head}`
- **checkpoint:** `{finished or "unknown"}`
- **results:** `{handoff["results"]}`
- **PARETO:** `{handoff["pareto"] or "(run analyze_campaign.py first)"}`
- **Open PROGRAM nodes:** {", ".join(open_nodes) or "(none)"}

## Trends (mesh/solve/quality vs tier)
{trend_table(rows)}

## Warehouse / visuals
Paths under the repo (use `read_file` on PNGs when present):
```
{chr(10).join(shots) if shots else "(no runs/ warehouse artifacts yet)"}
```

## Autonomous defaults
When you would ask a question, pick the **recommended** option and continue.
Do not block. Prefer small, tested commits pushed to master.

## Your mission this session
1. Inspect failed/weak rows (high rel_err, mesh_fail, edge_profile residual).
2. Propose **one** packing/sizing/edge-profile change (ADR-0021 / docs/research/varyhedron-packing.md).
3. Implement + Catch2/smoke verify.
4. Re-run a thin campaign slice or full `varyhedron-short-1` if cheap enough.
5. Commit + push; rewrite this handoff if looping.

## Invoke (already done if you are reading this from invoke_grok_improve.sh)
```bash
grok -p --yolo --permission-mode bypassPermissions \\
  --cwd {ROOT} --max-turns 80 \\
  --prompt-file {camp / "HANDOFF.md"}
```
"""
    (camp / "HANDOFF.md").write_text(md, encoding="utf-8")
    print(f"wrote {camp / 'HANDOFF.md'}")
    print(f"wrote {camp / 'handoff.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
