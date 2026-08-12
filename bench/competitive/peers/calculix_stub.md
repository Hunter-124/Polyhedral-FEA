# CalculiX / PolyMesh cantilever cross-validation

**Status:** [`run_calculix_cantilever.py`](run_calculix_cantilever.py) runs
CalculiX 2.23 and PolyMesh on identical structured hex meshes through a
4x1x1 → 32x8x8 refinement ladder. It uses the shared cantilever
tip-deflection reference and does not install packages or require network.

## Run (CI-safe)

```sh
python3 bench/competitive/peers/run_calculix_cantilever.py
```

| Required executable(s) | Exit code | Effect |
|---|---:|---|
| `ccx` and `polymesh` | 0 / 1 | Writes schema-validated `bench/results/calculix-cantilever.json` only after every rung succeeds |
| Either missing | **1** | Prints the missing executable(s); writes no new rows |

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

1. Emits identical coordinates and hex8 connectivity to a CalculiX C3D8 deck
   and Gmsh 2.2 `.msh` file for every refinement rung.
2. Runs both solvers serially with `OMP_NUM_THREADS=1`.
3. Applies the same \(-1000\,\mathrm{N}\) load-face resultant using equivalent
   CLOAD weights in CalculiX and PolyMesh's consistent face-load assembly.
4. Parses max \(|U_3|\), validates all rows against `../schema.json`, and writes
   `bench/results/calculix-cantilever.json`.

## Cases to port next

1. `timoshenko-cantilever` — calibrated tip deflection vs beam theory / PolyMesh  
2. `lame-cylinder` — pressurized thick wall, u_r / hoop  
3. `kirsch-plate` — hole SCF under uniaxial tension  

Keep mesh choice explicit in `notes` (peer mesh vs imported Gmsh) so DOF and
time comparisons stay honest. Holdout geometries: see
[`audits/README.md`](../../../audits/README.md) (owner-private only).
