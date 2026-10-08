# ADR-0022: Full experiment warehouse

- Status: accepted (2026-07-12)
- Decision: D22

## Context

Campaigns already write `results.jsonl` / checkpoints. Owner wants a full
experiment warehouse (every mesh VTU, wire PNG, quality, result) in git (LFS for
large binaries) so no campaign is re-run blind.

## Decision

### Warehouse layout

Under `bench/campaigns/<name>/`:

```
runs/<cfg_id>/<part>/t<tier>/
  mesh.vtu
  wire.png          # optional until shot harness lands
  quality.json
  result.json       # single-run mirror of the jsonl line
results.jsonl
checkpoint.json
progress.json
PARETO.md / PARETO.json
```

- Track `*.vtu` and large `*.png` via **git-LFS** (`.gitattributes`).
- Commit after each campaign batch.

### Short campaign shape set

Product campaign geometries (STEP only):

- `plate_hole`
- `cylinder`
- `sphere`
- `icecream_cone` (triangular pyramid with ball intersecting a face)

Meshers: **`varyhedron`**, **`hybrid_zoo`**. Approximately **3 tiers/runs per
shape** for solvetime/quality trends (`keep_frac: 1.0`, no aggressive trim).

## Consequences

- Interfaces.md §7 defines the warehouse schema.
- GUI Test Lab shows git HEAD + campaign sync state.

## Alternatives rejected

- Results-only git (no VTU/PNG) — owner chose full warehouse.
