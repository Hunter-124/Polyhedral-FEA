# Competitive benchmark harness

Peer-solver comparison for PolyMesh: same analytical cases, labeled commits,
JSON results under `bench/results/`, scoreboard at `docs/bench/scoreboard.md`.

Internal baseline remains the frozen P1 uniform path (ADR-0005). Peers are an
**audit / honesty** cross-check, not the primary DOF-reduction baseline.

## Result format

Each file in `bench/results/*.json` is either one result object or an array of
objects. Schema: [`schema.json`](schema.json).

Required fields:

| Field | Meaning |
|---|---|
| `solver` | Product name (`PolyMesh`, `CalculiX`, …) |
| `version` | Version string |
| `case_id` | Case id (`lame-cylinder`, `kirsch-plate`, `timoshenko-cantilever`, …) |
| `dofs` | Active DOFs, or `null` |
| `wall_time_s` | `{mesh, solve, total}` seconds; any may be `null` |
| `accuracy` | `{name, value}` metric (e.g. relative error %) |
| `label` | Git sha, tag, or milestone for the scoreboard |
| `timestamp` | ISO-8601 UTC |

Do **not** write reference answers into `src/`. Reference closed forms stay in
`bench/reference/*.json` (anti-cheat boundary).

## Scoreboard

```sh
python3 bench/competitive/render_scoreboard.py
# writes docs/bench/scoreboard.md
```

Stdlib only. Re-run after adding result JSONs; commit the regenerated markdown
with the results when publishing a labeled point.

## PolyMesh smoke (internal)

```sh
./bench/competitive/run_polymesh_smoke.sh
```

Builds (if needed) and runs the Tier-0/Tier-1 ctest subset. This is not the
full peer harness — peer runners land later under `peers/`.

## Peer priority (headless practicality)

Order is driven by **ease of scripting a headless linear-elasticity run**, not
by marketing claims.

| Priority | Solver | Why |
|---:|---|---|
| **1** | **CalculiX (`ccx`)** | Already required for `/audit` (ADR-0005). Single CLI binary, `.inp` decks, headless. Best first peer for Lamé / Kirsch / cantilever. |
| **2** | **Elmer (`ElmerSolver`)** | Packaged, sif-driven, no GUI required. Slightly more setup than ccx; good second independent stack. |
| **3** | **Code_Aster** | Industrial and free, but heavier install (Salome-Meca) and more complex command files. After CalculiX path is solid. |
| later | FEniCSx / deal.II / MFEM | Libraries, not drop-in solvers; useful for custom weak forms, not first scoreboard rows. |

**Not first:** GUI-only commercial demos, or anything that cannot run unattended
from a shell with a file-based deck.

## CalculiX

If `ccx` is not installed, see [`peers/calculix_stub.md`](peers/calculix_stub.md).
No network package installs are required by this scaffold; peer execution is
optional until the real runner lands.

## Adding a labeled PolyMesh point

1. Run the suite (smoke script or full `ctest`).
2. Drop a JSON under `bench/results/` matching `schema.json`.
3. `python3 bench/competitive/render_scoreboard.py`
4. Commit results + scoreboard together with the label (sha/tag).
