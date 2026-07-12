# Grok improvement handoff — `varyhedron-short-1`

You are working autonomously on **Polyhedral-FEA** (PolyMesh). Continue packing /
meshing improvements from campaign results. Read `docs/dag/AGENT_BOOTSTRAP.md`
and obey anti-cheat + pull-rebase + Hunter-124 identity (no AI trailers).

## Sync first
```bash
git fetch origin && git status && git pull --rebase origin master
```
Never force-push. Cap scope to open PROGRAM nodes related to varyhedron packing.

## Campaign snapshot
- **Campaign:** `varyhedron-short-1`
- **git HEAD:** `77f4bfefdd7053194b9dc5103aca70c318470e75`
- **checkpoint:** `finished`
- **results:** `bench/campaigns/varyhedron-short-1/results.jsonl`
- **PARETO:** `bench/campaigns/varyhedron-short-1/PARETO.md`
- **Open PROGRAM nodes:** campaign-1, feedback-loop, V6d, V6e, V8, V9a, V10c, V11

## Trends (mesh/solve/quality vs tier)
| part | mesher | tier | status | mesh_ms | solve_ms | quality | rel_err |
|------|--------|-----:|--------|--------:|---------:|--------:|--------:|
| cylinder | hybrid_zoo | 0 | ok | 75.233532 | 1039.45523 | 0.9263108670683365 | 641069330416739.4 |
| cylinder | hybrid_zoo | 1 | ok | 258.892482 | 9730.602252 | 0.9194164135296229 | 554424736750517.4 |
| cylinder | hybrid_zoo | 2 | over_budget | 809.517725 | 0.0 | 0.7332981469839908 |  |
| cylinder | varyhedron | 0 | ok | 148.665998 | 94.358597 | 0.7733821735544429 | 1.383741516390807 |
| cylinder | varyhedron | 1 | ok | 420.001827 | 647.676342 | 0.7032715171477764 | 1.6881194096651642 |
| cylinder | varyhedron | 2 | over_budget | 818.718481 | 0.0 | 0.7019978999555634 |  |
| icecream_cone | hybrid_zoo | 0 | ok | 172.82962 | 74.446561 | 0.6715095469989768 | 97487662856869.81 |
| icecream_cone | hybrid_zoo | 1 | ok | 409.403749 | 476.213897 | 0.6692274457354261 | 597771499849311.6 |
| icecream_cone | hybrid_zoo | 2 | ok | 967.646509 | 3164.386303 | 0.6319128212058321 | 309115117992179.44 |
| icecream_cone | varyhedron | 0 | ok | 264.920028 | 27.901538 | 0.5345981827394666 | 15.901699201078154 |
| icecream_cone | varyhedron | 1 | ok | 682.155023 | 116.657895 | 0.0020769547025447236 | 34.6142152445138 |
| icecream_cone | varyhedron | 2 | ok | 1269.148128 | 554.145712 | 0.0014212672276489651 | 22.723932369376342 |
| plate_hole | hybrid_zoo | 0 | ok | 36.689493 | 54.833013 | 0.7642857142857142 | 0.6279460863279877 |
| plate_hole | hybrid_zoo | 1 | ok | 65.892213 | 111.205293 | 0.8575079277342674 | 1.3699409209654398 |
| plate_hole | hybrid_zoo | 2 | ok | 152.710347 | 1740.652483 | 0.8345335700530725 | 1.9019764084139006 |
| plate_hole | varyhedron | 0 | ok | 42.54654 | 5.372381 | 0.6684541288024382 | 0.8574426207848568 |
| plate_hole | varyhedron | 1 | ok | 96.479831 | 24.195081 | 0.6695650928636382 | 1.078245956468086 |
| plate_hole | varyhedron | 2 | ok | 197.27045 | 109.781212 | 0.7319064825874585 | 1.5655912843632727 |
| sphere | hybrid_zoo | 0 | ok | 38.675425 | 12.229908 | 0.9795232556185703 | 1.505291985156422 |
| sphere | hybrid_zoo | 1 | ok | 88.927056 | 86.621847 | 0.9866759271061611 | 1.3703525531791727 |
| sphere | hybrid_zoo | 2 | ok | 145.008212 | 377.237911 | 0.9848866570519903 | 1.4581229283729968 |
| sphere | varyhedron | 0 | ok | 57.352743 | 5.728867 | 0.8198454275432979 | 1.4736265154645947 |
| sphere | varyhedron | 1 | ok | 144.892114 | 33.454287 | 0.8191517220792394 | 1.5258520512112184 |
| sphere | varyhedron | 2 | ok | 255.46704 | 86.47889 | 0.7994496301355529 | 1.4804211299896073 |

## Warehouse / visuals
Paths under the repo (use `read_file` on PNGs when present):
```
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/cylinder/t0/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/cylinder/t1/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/cylinder/t2/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/icecream_cone/t0/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/icecream_cone/t1/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/icecream_cone/t2/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/plate_hole/t0/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/plate_hole/t1/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/plate_hole/t2/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/sphere/t0/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/sphere/t1/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/sphere/t2/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/cylinder/t0/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/cylinder/t1/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/cylinder/t2/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/icecream_cone/t0/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/icecream_cone/t1/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/icecream_cone/t2/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/plate_hole/t0/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/plate_hole/t1/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/plate_hole/t2/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/sphere/t0/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/sphere/t1/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/sphere/t2/wire.png
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/cylinder/t0/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/cylinder/t1/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/cylinder/t2/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/icecream_cone/t0/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/icecream_cone/t1/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/icecream_cone/t2/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/plate_hole/t0/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/plate_hole/t1/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/plate_hole/t2/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/sphere/t0/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/sphere/t1/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-028399df/sphere/t2/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/cylinder/t0/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/cylinder/t1/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/cylinder/t2/mesh.vtu
bench/campaigns/varyhedron-short-1/runs/cfg-89f62376/icecream_cone/t0/mesh.vtu
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
  --prompt-file /home/hunter/Desktop/Polyhedral-FEA/bench/campaigns/varyhedron-short-1/HANDOFF.md
```
