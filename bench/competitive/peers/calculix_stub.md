# CalculiX peer runner

**Status:** cantilever smoke runner lives at
[`run_calculix_cantilever.py`](run_calculix_cantilever.py). Full Lamé / Kirsch
decks + accuracy parse land with the audit harness (ADR-0005). This tree does
not install packages or require network.

## Run (CI-safe)

```sh
python3 bench/competitive/peers/run_calculix_cantilever.py
```

| `ccx` on PATH? | Exit code | Effect |
|---|---:|---|
| Yes | 0 / 1 | Writes `bench/results/calculix-cantilever-smoke.json`; refreshes scoreboard on success |
| No | **0** | Prints skip message — **CI must not fail** |

Check binary:

```sh
command -v ccx && ccx -v 2>&1 | head -n 1
```

Typical names: `ccx`, `ccx_2.21`, `ccx_static`. If missing, install from your
OS or build from [http://www.dhondt.de/](http://www.dhondt.de/) — **do not**
script `apt`/`dnf`/`brew` from CI without an explicit maintainer opt-in.

### Common install paths (documentation only)

| Platform | Notes |
|---|---|
| Debian/Ubuntu | package `calculix-ccx` when available in distro repos |
| Fedora | package `CalculiX-ccx` |
| From source | download `ccx_*.src.tar.bz2` + SPOOLES/ARPACK; build `ccx` |
| Conda-forge | `calculix` feedstock (optional local env; not used by default CI) |

## Runner contract

The peer script:

1. Emits a CalculiX `.inp` for a coarse C3D8 cantilever smoke case.
2. Runs `ccx <jobname>` in a temp directory.
3. Writes `bench/results/calculix-cantilever-smoke.json` per `../schema.json`
   (`accuracy.name = smoke_ran`; not a calibrated tip-error peer yet).
4. Best-effort refreshes `docs/bench/scoreboard.md`.

## Cases to port next

1. `timoshenko-cantilever` — calibrated tip deflection vs beam theory / PolyMesh  
2. `lame-cylinder` — pressurized thick wall, u_r / hoop  
3. `kirsch-plate` — hole SCF under uniaxial tension  

Keep mesh choice explicit in `notes` (peer mesh vs imported Gmsh) so DOF and
time comparisons stay honest. Holdout geometries: see
[`audits/README.md`](../../../audits/README.md) (owner-private only).
