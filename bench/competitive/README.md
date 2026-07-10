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

## CalculiX peer (E1)

Headless smoke runner (optional peer — not required for CI green):

```sh
# when ccx is on PATH: writes bench/results/calculix-cantilever-smoke.json
# and refreshes docs/bench/scoreboard.md
python3 bench/competitive/peers/run_calculix_cantilever.py

# when ccx is missing: prints a skip message and exits 0 (CI-safe)
```

| Behavior | Exit | Output |
|---|---:|---|
| `ccx` present, deck OK | 0 | JSON row + scoreboard refresh |
| `ccx` present, deck fails | 1 | stderr from ccx |
| `ccx` absent | 0 | `ccx not found; skip …` |

Install notes and future Lamé/Kirsch deck plans:
[`peers/calculix_stub.md`](peers/calculix_stub.md). No network package installs
from this harness.

## PolyMesh labeled points (E2)

GATE 1 scoreboard rows (Lamé + Kirsch + cantilever) live in
`bench/results/polymesh-gate1-p1.json`. Accuracy comes from the frozen GATE 1
report; DOFs are best-effort estimates from structured-mesh dims in the Tier-1
tests (not hardcoded into `src/`).

Refresh / fill missing fields from the GATE 1 snapshot:

```sh
# dry-run to stdout
python3 bench/competitive/emit_polymesh_gate1.py

# write JSON + regenerate scoreboard
python3 bench/competitive/emit_polymesh_gate1.py --write --render
```

Or hand-edit a result JSON after `ctest` and re-run
`python3 bench/competitive/render_scoreboard.py`.

## Holdout audits (E3)

Private geometries are **not** in this tree. Protocol:
[`audits/README.md`](../../audits/README.md). Agent records metrics only;
owner keeps reference solutions offline.

## D6 Tier-3 (uniform tet10 vs graded)

L-domain energy instrument (ADR-0005): same solver, geometric grading toward the
re-entrant corner vs uniform tet10. Driver + binary:

```sh
cmake --build build --target polymesh-d6-tier3 -j
python3 bench/d6/run_tier3.py --full --render   # writes bench/results + scoreboard
python3 bench/d6/run_tier3.py --help            # smoke
```

| Artifact | Role |
|---|---|
| `apps/bench/polymesh-d6-tier3` | C++ timed suite |
| `bench/d6/run_tier3.py` | Build/run/split JSON + `docs/bench/d6-tier3.md` |
| `bench/d6/out/polymesh-d6-l-domain-raw.json` | Full run grid + summary ratios |
| `bench/results/polymesh-d6-l-domain.json` | Scoreboard rows (`d6-tier3`) |

Not a multi-minute ctest gate; Catch2 only checks `--help` / committed JSON shape.

## Adding a labeled PolyMesh point

1. Run the suite (smoke script or full `ctest`).
2. Drop a JSON under `bench/results/` matching `schema.json`
   (or refresh gate1-p1 via `emit_polymesh_gate1.py --write`).
3. `python3 bench/competitive/render_scoreboard.py`
4. Commit results + scoreboard together with the label (sha/tag).
