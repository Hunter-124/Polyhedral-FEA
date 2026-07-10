# CalculiX peer runner (stub)

**Status:** scaffold only. Full deck generation + parse lands with the audit
harness (ADR-0005). This tree does not install packages or require network.

## Binary

```sh
command -v ccx && ccx 2>&1 | head -n 1
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

## Expected future runner contract

When implemented, a peer script should:

1. Emit a CalculiX `.inp` for a Tier-1 case (same geometry/loads as PolyMesh).
2. Run `ccx <jobname>` with cwd set to a temp or `bench/competitive/work/`.
3. Parse `.dat` / `.frd` for the accuracy metric (tip u, SCF, u_r, …).
4. Write `bench/results/calculix-<case>-<label>.json` per `../schema.json`.

Until then, scoreboard rows for CalculiX are empty; PolyMesh internal snapshots
still render.

## Cases to port first

1. `timoshenko-cantilever` — hex/tet beam, tip deflection  
2. `lame-cylinder` — pressurized thick wall, u_r / hoop  
3. `kirsch-plate` — hole SCF under uniaxial tension  

Keep mesh choice explicit in `notes` (peer mesh vs imported Gmsh) so DOF and
time comparisons stay honest.
