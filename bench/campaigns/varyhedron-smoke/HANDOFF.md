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
- **git HEAD:** `2c9482e3cfb06feea0c0cd42d913a1f242452260`
- **checkpoint:** `finished`
- **results:** `bench/campaigns/varyhedron-smoke/results.jsonl`
- **PARETO:** `bench/campaigns/varyhedron-smoke/PARETO.md`
- **Open PROGRAM nodes:** campaign-1, feedback-loop, V3c, V6d, V6e, V8, V9a, V10c, V11

## Trends (mesh/solve/quality vs tier)
| part | mesher | tier | status | mesh_ms | solve_ms | quality | rel_err |
|------|--------|-----:|--------|--------:|---------:|--------:|--------:|
| cylinder | hybrid_zoo | 0 | ok | 8792.70781 | 10701.167185 | 0.9263108670683365 | 641069330416739.4 |
| cylinder | varyhedron | 0 | ok | 11905.066083 | 1053.855444 | 0.7733821735544429 | 1.383741516390807 |
| plate_hole | hybrid_zoo | 0 | ok | 4163.668249 | 555.269483 | 0.7642857142857142 | 0.6279460863279877 |
| plate_hole | varyhedron | 0 | ok | 4831.921059 | 122.107119 | 0.6684541288024382 | 0.8574426207848568 |

## Warehouse / visuals
Paths under the repo (use `read_file` on PNGs when present):
```
bench/campaigns/varyhedron-smoke/runs/cfg-028399df/cylinder/t0/wire.png
bench/campaigns/varyhedron-smoke/runs/cfg-028399df/plate_hole/t0/wire.png
bench/campaigns/varyhedron-smoke/runs/cfg-89f62376/cylinder/t0/wire.png
bench/campaigns/varyhedron-smoke/runs/cfg-89f62376/plate_hole/t0/wire.png
bench/campaigns/varyhedron-smoke/runs/cfg-028399df/cylinder/t0/mesh.vtu
bench/campaigns/varyhedron-smoke/runs/cfg-028399df/plate_hole/t0/mesh.vtu
bench/campaigns/varyhedron-smoke/runs/cfg-89f62376/cylinder/t0/mesh.vtu
bench/campaigns/varyhedron-smoke/runs/cfg-89f62376/plate_hole/t0/mesh.vtu
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
