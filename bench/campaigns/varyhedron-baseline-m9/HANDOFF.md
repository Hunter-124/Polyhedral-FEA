# Grok improvement handoff — `varyhedron-baseline-m9`

You are working autonomously on **Polyhedral-FEA** (PolyMesh). Continue packing /
meshing improvements from campaign results. Read `docs/dag/AGENT_BOOTSTRAP.md`
and obey anti-cheat + pull-rebase + Hunter-124 identity (no AI trailers).

## Sync first
```bash
git fetch origin && git status && git pull --rebase origin master
```
Never force-push. Cap scope to open PROGRAM nodes related to varyhedron packing.

## Campaign snapshot
- **Campaign:** `varyhedron-baseline-m9`
- **git HEAD:** `dcb2baa380be1bafc2b4d9ff5d585c5d31ab964c`
- **checkpoint:** `finished`
- **results:** `bench/campaigns/varyhedron-baseline-m9/results.jsonl`
- **PARETO:** `bench/campaigns/varyhedron-baseline-m9/PARETO.md`
- **Open PROGRAM nodes:** campaign-1, feedback-loop, V6d, V6e, V10c, V11, M5, M9, M10, M11, M12, M13, M14, G0, G1, G2, G3, G4

## Trends (mesh/solve/quality vs tier)
| part | mesher | tier | status | mesh_ms | solve_ms | quality | rel_err |
|------|--------|-----:|--------|--------:|---------:|--------:|--------:|
| cylinder | hybrid_zoo | 0 | solve_suspect | 94.701212 | 1011.227777 | 0.9263108670683365 | 1.4285118627095361 |
| cylinder | varyhedron | 0 | solve_suspect | 186.820699 | 95.006974 | 0.7733821735544429 | 1.4604244133510944 |
| icecream_cone | hybrid_zoo | 0 | ok | 213.781871 | 99.077793 | 0.6715095469989768 | 97487662856869.81 |
| icecream_cone | varyhedron | 0 | ok | 309.787582 | 42.628205 | 0.5328693784384204 | 15.936522624205683 |
| plate_hole | hybrid_zoo | 0 | ok | 55.358709 | 63.235421 | 0.7642857142857142 | 0.25010906978831765 |
| plate_hole | varyhedron | 0 | ok | 78.399845 | 16.260522 | 0.6684541288024382 | 0.6212357589594882 |
| sphere | hybrid_zoo | 0 | ok | 77.877258 | 23.015738 | 0.9795232556185703 | 0.9740873510038534 |
| sphere | varyhedron | 0 | ok | 69.79989 | 14.921269 | 0.8242846784188972 | 0.9352760956727952 |

## Warehouse / visuals
Paths under the repo (use `read_file` on PNGs when present):
```
bench/campaigns/varyhedron-baseline-m9/runs/cfg-028399df/cylinder/t0/wire.png
bench/campaigns/varyhedron-baseline-m9/runs/cfg-028399df/icecream_cone/t0/wire.png
bench/campaigns/varyhedron-baseline-m9/runs/cfg-028399df/plate_hole/t0/wire.png
bench/campaigns/varyhedron-baseline-m9/runs/cfg-028399df/sphere/t0/wire.png
bench/campaigns/varyhedron-baseline-m9/runs/cfg-89f62376/cylinder/t0/wire.png
bench/campaigns/varyhedron-baseline-m9/runs/cfg-89f62376/icecream_cone/t0/wire.png
bench/campaigns/varyhedron-baseline-m9/runs/cfg-89f62376/plate_hole/t0/wire.png
bench/campaigns/varyhedron-baseline-m9/runs/cfg-89f62376/sphere/t0/wire.png
bench/campaigns/varyhedron-baseline-m9/runs/cfg-028399df/cylinder/t0/mesh.vtu
bench/campaigns/varyhedron-baseline-m9/runs/cfg-028399df/icecream_cone/t0/mesh.vtu
bench/campaigns/varyhedron-baseline-m9/runs/cfg-028399df/plate_hole/t0/mesh.vtu
bench/campaigns/varyhedron-baseline-m9/runs/cfg-028399df/sphere/t0/mesh.vtu
bench/campaigns/varyhedron-baseline-m9/runs/cfg-89f62376/cylinder/t0/mesh.vtu
bench/campaigns/varyhedron-baseline-m9/runs/cfg-89f62376/icecream_cone/t0/mesh.vtu
bench/campaigns/varyhedron-baseline-m9/runs/cfg-89f62376/plate_hole/t0/mesh.vtu
bench/campaigns/varyhedron-baseline-m9/runs/cfg-89f62376/sphere/t0/mesh.vtu
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
  --prompt-file /home/hunter/Desktop/Polyhedral-FEA/bench/campaigns/varyhedron-baseline-m9/HANDOFF.md
```
