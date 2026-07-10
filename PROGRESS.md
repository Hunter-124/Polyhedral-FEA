# PROGRESS

## Current phase
**P1 — Reference solver: core + Tier-0 + Tier-2 MMS green.**
Remaining for GATE 1: Lamé cylinder + Kirsch/Goodier Tier-1 analytical cases
(needs surface-traction loads + curved mapped meshes), convergence plots for
review, mesh file import.
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
