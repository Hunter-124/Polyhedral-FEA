# PROGRESS

## Current phase
**P0 — Decisions & scaffolding: complete, pending GATE 0 human review.**
Next: P1 — reference solver on standard elements (tet4/tet10, hex8/hex20).

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
| Tier 0 (patch/rigid-body/eigen/validity) | not yet — needs P1 elements |
| Tier 1 analytical suite | not yet |
| Tier 2 MMS | not yet |
| Tier 3 performance | not yet |

## Open issues
- GATE 0 review by owner: repo skeleton + ratified decisions.
- CLA/DCO policy before first external contribution (ADR-0002).
- `POLYMESH_WITH_OCC` wiring deferred until P3 consumes exact geometry
  (ADR-0001).
- CUDA toolkit not installed on dev machine; `POLYMESH_WITH_CUDA` untested
  until it is (ADR-0008). RTX 3080 Ti present.
- Geometric validity checks (watertight, Jacobians, conforming interfaces)
  are P2 scope; only structural checks exist today.
