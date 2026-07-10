# Holdout geometry audits

Protocol for **private** verification geometries that the agent (or CI peer)
must not see answers for. Public fixtures live under `bench/geometries/public/`;
holdouts stay off-repo with the owner.

This directory is a **stub**: no secret STL/STEP, no reference answers, no
machine-local paths committed here.

## Goals

1. Owner can prove PolyMesh meshes and solves a real part without the agent
   hardcoding closed-form targets.
2. Agent records only **metrics** (DOF, wall time, max |u|, max σ_vm, mesh
   quality notes) — never the owner's reference solution.
3. Scoreboard / honesty: peer CalculiX (or other) can be run on the same
   private file **by the owner**, not by committing peer decks that embed answers.

## Roles

| Role | Sees geometry? | Sees reference solution? | Records |
|------|----------------|--------------------------|---------|
| Owner | Yes | Yes (optional analytical or commercial baseline) | Supplies path + success criteria |
| Agent / harness | Yes (file path only) | **No** | Mesh notes, solve metrics, VTU path |
| CI default | No | No | Skip (no private assets in tree) |

## Owner supply (private)

Place holdout files **outside** the git worktree, e.g.:

```text
~/polymesh-holdouts/
  bracket_v3.stl          # or .step
  bracket_v3.meta.json    # optional: material, BC description in plain language
  bracket_v3.reference/   # owner-only: commercial solve, expected tip u, …
```

Suggested `*.meta.json` (illustrative; not schema-locked yet):

```json
{
  "name": "bracket_v3",
  "material": {"E": 2.0e11, "nu": 0.3},
  "bc_plain": "fix bolt holes; vertical load 1 kN on top pad",
  "mesh_hint_h": 0.005,
  "success": {
    "require_finite_stress": true,
    "min_tip_displacement_m": null,
    "max_eta": null
  }
}
```

Do **not** put numerical reference answers in the agent prompt if the goal is a
blind audit. Give BC intent in words; agent implements; owner compares offline.

## Agent procedure (blind)

1. Receive absolute path to STL/STEP + meta (no reference folder).
2. Mesh: CLI or GUI product path, e.g.

   ```sh
   ./build/apps/cli/polymesh mesh /path/to/holdout.stl -o /tmp/holdout.msh
   ./build/apps/cli/polymesh solve /path/to/holdout.stl -o /tmp/holdout.vtu
   ```

3. Record **only** public metrics into a local (gitignored) result, e.g.
   `audits/local/<name>-<sha>.json` — see schema below. Commit nothing private.
4. Do not open or copy `*.reference/` if present on the owner's machine.
5. Return metrics summary to the owner for offline comparison.

## Result JSON (metrics only)

Align fields with `bench/competitive/schema.json` where possible:

```json
{
  "schema_version": 1,
  "solver": "PolyMesh",
  "version": "0.1.0",
  "case_id": "holdout:<name>",
  "dofs": null,
  "wall_time_s": {"mesh": null, "solve": null, "total": null},
  "accuracy": {"name": "owner_compare_offline", "value": null},
  "label": "<git-sha-or-tag>",
  "timestamp": "2026-07-10T00:00:00Z",
  "notes": "mesh note / max|u| / max σ_vm — no reference residuals",
  "holdout": {
    "geometry_basename": "bracket_v3.stl",
    "geometry_sha256": null,
    "agent_saw_reference": false
  }
}
```

`accuracy.value` stays `null` for blind runs. Owner fills a private comparison
table after the fact.

## What must not be committed

- Holdout STL/STEP/STEP assemblies
- Owner reference solutions, commercial exports, closed-form numbers for holdouts
- Absolute home-directory paths in tracked files
- Screenshots that reveal proprietary part geometry (optional owner policy)

Safe to commit in this repo:

- This protocol (`audits/README.md`)
- Public fixtures under `bench/geometries/public/`
- Competitive scoreboard rows for **public** analytical cases only

## Optional peer cross-check (owner)

Owner may run CalculiX locally on the same mesh export:

```sh
# when ccx is installed
python3 bench/competitive/peers/run_calculix_cantilever.py   # public smoke only
# holdout decks: owner-authored .inp; keep out of git
```

Public peer smoke documents CLI availability; holdout peer runs stay private.

## Status

| Item | State |
|------|--------|
| Protocol README | **this file** |
| Local result dir | create `audits/local/` yourself; gitignored |
| Secret geometries | none in tree (by design) |
| Automated holdout CI | not required for E3 stub |

See also: `docs/decisions/0005-benchmark-baseline.md`, `bench/competitive/README.md`.
