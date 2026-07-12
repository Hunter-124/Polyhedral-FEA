# Grok improvement handoff — `varyhedron-smoke`

You are working autonomously on **Polyhedral-FEA** (PolyMesh). Continue packing /
meshing improvements from campaign results. Read `docs/dag/AGENT_BOOTSTRAP.md`
and obey anti-cheat + pull-rebase + Hunter-124 identity (no AI trailers).

## Sync first
```bash
git fetch origin && git status && git pull --rebase origin master
```
Never force-push. Cap scope to open PROGRAM nodes related to varyhedron packing.

## Campaign snapshot
- **Campaign:** `varyhedron-smoke`
- **git HEAD:** `c247c0aebf96c121d266760e3361cec754e5ee2c`
- **checkpoint:** `finished`
- **results:** `bench/campaigns/varyhedron-smoke/results.jsonl`
- **PARETO:** `bench/campaigns/varyhedron-smoke/PARETO.md`
- **Open PROGRAM nodes:** campaign-1, feedback-loop, V1c, V1d, V2d, V3c, V4, V6c, V6d, V6e, V8, V9a, V9b, V10c, V10d, V11

## Trends (mesh/solve/quality vs tier)
| part | mesher | tier | status | mesh_ms | solve_ms | quality | rel_err |
|------|--------|-----:|--------|--------:|---------:|--------:|--------:|
| cylinder | hybrid_zoo | 0 | ok | 8814.60098 | 46931.086012 | 0.9246161294542614 | 39387743565368.13 |
| cylinder | varyhedron | 0 | solve_fail | 11261.453119 | 0.0 | 0.7681297350178713 |  |
| plate_hole | hybrid_zoo | 0 | ok | 3102.71569 | 440.636989 | 0.623302664483781 | 0.6287242456556233 |
| plate_hole | varyhedron | 0 | solve_fail | 4345.133049 | 0.0 | 0.5493946213334598 |  |

## Warehouse / visuals
Paths under the repo (use `read_file` on PNGs when present):
```
bench/campaigns/varyhedron-smoke/runs/cfg-028399df/cylinder/t0/mesh.vtu
bench/campaigns/varyhedron-smoke/runs/cfg-028399df/plate_hole/t0/mesh.vtu
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
grok -p --yolo --permission-mode bypassPermissions \
  --cwd /home/hunter/Desktop/Polyhedral-FEA --max-turns 80 \
  --prompt-file /home/hunter/Desktop/Polyhedral-FEA/bench/campaigns/varyhedron-smoke/HANDOFF.md
```
