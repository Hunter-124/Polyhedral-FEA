# PROGRESS

## Current phase
**P1 nearly complete + GUI v0 shipped (pulled forward from P6.5).**
- P1 remaining for GATE 1: Kirsch/Goodier/L-domain Tier-1 cases, convergence
  plots for owner review, Gmsh mesh import.
- GUI (`polymesh-gui`): STL import, CAD-style face-region picking, fixtures/
  loads/material/mesh settings, background solve, von Mises + deflection
  results on deformed shape. Solves via draft voxel mesher v0 until the
  P2/P3 conforming mesher replaces it.
GATE 0 was approved by owner on 2026-07-09.

## Done
- 2026-07-09: D1–D5 + GUI scope ratified with owner (ADR-0001..0006).
- 2026-07-09: License AGPL-3.0-or-later applied; git identity policy recorded
  in CLAUDE.md.
- 2026-07-09: Owner switched language to C++ (ADR-0007) and made CUDA a
  first-class optional backend (ADR-0008). Rust scaffold ported the same day:
  CMake/Ninja workspace (geom, mesh, adapt, fea, bench, cli), STL loader with
  welding, face-based mesh structure with structural validity checker,
  Material/D-matrix, backend dispatch (cpu/cuda), reference-case loader,
  CLI `check`/`backend` subcommands, 16 Catch2 tests green.
  CI: warnings-as-errors build + ctest + clang-format + grep audit.

## Benchmark table
| Case | Status |
|---|---|
| Tier 0 patch test (all 4 element types, distorted meshes) | PASS, max error < 1e-12 m |
| Tier 0 rigid-body modes | PASS (< 1e-12 relative) |
| Tier 0 single-element eigenvalues (6 zero modes) | PASS |
| Tier 1 Timoshenko cantilever (hex20, gravity load) | PASS within 3% |
| Tier 1 Lamé cylinder / Kirsch / Goodier / L-domain | not yet (needs tractions + curved meshes) |
| Tier 2 MMS convergence | PASS: tet4 0.997, hex8 0.997, tet10 2.000, hex20 2.000 (theory 1/1/2/2, tol ±0.2) |
| Tier 2 MMS exact-representation sanity (p=2, quadratic field) | PASS (< 1e-9 relative) |
| Tier 3 performance | not yet (needs P2+ adaptive path) |

## Open issues
- GATE 0 review by owner: repo skeleton + ratified decisions.
- CLA/DCO policy before first external contribution (ADR-0002).
- `POLYMESH_WITH_OCC` wiring deferred until P3 consumes exact geometry
  (ADR-0001).
- CUDA toolkit not installed on dev machine; `POLYMESH_WITH_CUDA` untested
  until it is (ADR-0008). RTX 3080 Ti present.
- Geometric validity checks (watertight, Jacobians, conforming interfaces)
  are P2 scope; only structural checks exist today.
